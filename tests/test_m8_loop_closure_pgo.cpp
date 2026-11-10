#include "avscan/backend/GicpVerifier.hpp"
#include "avscan/backend/PoseGraphOptimizer.hpp"
#include "avscan/backend/ScanContext.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace avscan;

namespace {
std::vector<core::LidarPointXYZIRT> generateStructuredRoomCloud(const math::SE3d& T_pose = math::SE3d::Identity()) {
    std::vector<core::LidarPointXYZIRT> cloud;
    // Generate asymmetric room structures (pillars, walls of distinct heights) so yaw shift is unambiguous
    for (double x = -8.0; x <= 8.0; x += 0.4) {
        for (double y = -6.0; y <= 6.0; y += 0.4) {
            double z_max = -1.2; // floor
            if (std::abs(x - 5.0) < 0.6 && std::abs(y - 2.0) < 0.6) {
                z_max = 2.4; // tall pillar in quadrant 1
            } else if (std::abs(x + 6.0) < 0.5 && y < -2.0) {
                z_max = 1.8; // wall structure in quadrant 3
            } else if (x > 2.0 && std::abs(y + 4.5) < 0.5) {
                z_max = 1.1; // medium cabinet in quadrant 4
            }
            for (double z = -1.2; z <= z_max + 1e-3; z += 0.35) {
                const math::Vec3d p = T_pose.transform(math::Vec3d(x, y, z));
                core::LidarPointXYZIRT pt;
                pt.x = static_cast<float>(p.x());
                pt.y = static_cast<float>(p.y());
                pt.z = static_cast<float>(p.z());
                pt.intensity = 120.0f;
                cloud.push_back(pt);
            }
        }
    }
    return cloud;
}
}  // namespace

bool run_test_m8_loop_closure_pgo() {
    // 1. Test Scan Context descriptor generation & yaw-shift recovery
    const auto base_cloud = generateStructuredRoomCloud();
    const double yaw_gt_rad = 42.0 * math::kPi / 180.0; // 7 sectors shift (6 deg/sector)
    const math::SE3d T_rotated(math::SO3d::exp(math::Vec3d(0.0, 0.0, yaw_gt_rad)), math::Vec3d(0.15, -0.10, 0.0));
    const auto rotated_cloud = generateStructuredRoomCloud(T_rotated);

    backend::ScanContextDatabase sc_db(5, 0.25);
    const auto desc_0 = backend::ScanContextDatabase::computeDescriptor(0, 0.0, base_cloud);
    sc_db.addKeyframeDescriptor(desc_0);

    // Add intermediate keyframes along a corridor loop
    for (uint64_t kf = 1; kf <= 10; ++kf) {
        std::vector<core::LidarPointXYZIRT> tunnel_pts;
        for (int i = 0; i < 200; ++i) {
            core::LidarPointXYZIRT pt;
            pt.x = static_cast<float>((i % 20) * 0.5);
            pt.y = (i % 2 == 0) ? -2.0f : 2.0f;
            pt.z = -0.5f;
            tunnel_pts.push_back(pt);
        }
        sc_db.addKeyframeDescriptor(backend::ScanContextDatabase::computeDescriptor(kf, static_cast<double>(kf), tunnel_pts));
    }

    const auto query_desc = backend::ScanContextDatabase::computeDescriptor(11, 11.0, rotated_cloud);
    const auto loop_cand = sc_db.detectLoopClosure(query_desc);
    if (!loop_cand.valid || loop_cand.match_keyframe_id != 0 || loop_cand.sc_cosine_distance > 0.20) {
        std::printf("  [M8 FAIL] Scan Context failed to detect true loop closure (valid=%d, match=%llu, dist=%.3f)\n",
                    loop_cand.valid ? 1 : 0,
                    static_cast<unsigned long long>(loop_cand.match_keyframe_id),
                    loop_cand.sc_cosine_distance);
        return false;
    }

    // 2. Test GICP Geometric Verification (True loop acceptance + False loop rejection)
    backend::GicpVerifier gicp;
    const math::SE3d initial_guess = T_rotated * math::SE3d(math::SO3d::exp(math::Vec3d(0.0, 0.0, 0.05)), math::Vec3d(0.1, -0.1, 0.02));
    const auto gicp_res = gicp.verifyLoopConstraint(base_cloud, rotated_cloud, initial_guess);
    if (!gicp_res.verified || gicp_res.fitness_rmse_m > 0.18 || gicp_res.inlier_overlap_ratio < 0.35) {
        std::printf("  [M8 FAIL] GICP rejected true loop match (verified=%d, rmse=%.4f, overlap=%.2f)\n",
                    gicp_res.verified ? 1 : 0, gicp_res.fitness_rmse_m, gicp_res.inlier_overlap_ratio);
        return false;
    }

    // Verify false loop match is strictly rejected
    std::vector<core::LidarPointXYZIRT> unrelated_cloud;
    for (int i = 0; i < 300; ++i) {
        core::LidarPointXYZIRT pt;
        pt.x = 25.0f + static_cast<float>(i * 0.05);
        pt.y = -18.0f;
        pt.z = 3.0f;
        unrelated_cloud.push_back(pt);
    }
    const auto false_res = gicp.verifyLoopConstraint(base_cloud, unrelated_cloud, math::SE3d::Identity());
    if (false_res.verified) {
        std::printf("  [M8 FAIL] GICP accepted unrelated false loop match\n");
        return false;
    }

    // 3. Test Pose Graph Optimization (GTSAM-equivalent SE(3) optimizer) & submap reintegration
    backend::PoseGraphOptimizer pgo;
    const int num_kf = 12;
    for (int i = 0; i < num_kf; ++i) {
        const double angle = (2.0 * math::kPi * i) / (num_kf - 1);
        // True circle radius 10m, but inject cumulative Z and XY drift into initial odometry poses
        const math::Vec3d true_pos(10.0 * std::cos(angle), 10.0 * std::sin(angle), 0.0);
        const math::Vec3d drift(0.08 * i, -0.06 * i, 0.05 * i);
        core::KeyframeSubmap kf;
        kf.keyframe_id = static_cast<uint64_t>(i);
        kf.timestamp_sec = static_cast<double>(i);
        kf.T_world_submap = math::SE3d(math::SO3d::Identity(), true_pos + drift);
        kf.local_points = base_cloud;
        pgo.addKeyframeSubmap(kf);
    }

    // Reject unverified loop factor
    if (pgo.addVerifiedLoopFactor(0, num_kf - 1, math::SE3d::Identity(), gicp_res.information_diagonal, false)) {
        std::printf("  [M8 FAIL] PoseGraphOptimizer accepted unverified loop constraint\n");
        return false;
    }

    // Add verified loop closure between start (0) and end (num_kf - 1), which are at the exact same true location
    if (!pgo.addVerifiedLoopFactor(0, num_kf - 1, math::SE3d::Identity(), {400, 400, 400, 1600, 1600, 1600}, true)) {
        std::printf("  [M8 FAIL] PoseGraphOptimizer failed to add verified loop factor\n");
        return false;
    }

    const double pre_opt_closure_err =
        (pgo.getSubmaps().back().T_world_submap.translation() - pgo.getSubmaps().front().T_world_submap.translation()).norm();
    const auto opt_report = pgo.optimizeGlobalPoseGraph(30);
    const double post_opt_closure_err =
        (pgo.getSubmaps().back().T_world_submap.translation() - pgo.getSubmaps().front().T_world_submap.translation()).norm();

    if (!opt_report.converged || post_opt_closure_err >= pre_opt_closure_err * 0.25 ||
        opt_report.final_chi2_error >= opt_report.initial_chi2_error) {
        std::printf("  [M8 FAIL] PoseGraphOptimizer failed to reduce loop closure drift (pre=%.3f m, post=%.3f m)\n",
                    pre_opt_closure_err, post_opt_closure_err);
        return false;
    }

    const auto global_cloud = pgo.buildGloballyConsistentPointCloud(4);
    if (global_cloud.empty()) {
        std::printf("  [M8 FAIL] buildGloballyConsistentPointCloud returned empty cloud\n");
        return false;
    }

    return true;
}
