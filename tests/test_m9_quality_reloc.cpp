#include "avscan/quality/QualityMonitor.hpp"
#include <cmath>
#include <cstdio>
#include <tuple>
#include <vector>

using namespace avscan;

bool run_test_m9_quality_reloc() {
    quality::QualityMonitor monitor;

    // 1. Test Quality Evaluation (nominal vs degenerate/high-drift)
    core::SlamState nominal_state{};
    nominal_state.position_covariance = math::Mat3d::Identity() * 0.001;
    slam::EstimatorMetrics nominal_metrics{};
    nominal_metrics.valid_geometric_correspondences = 320;
    nominal_metrics.mean_point_to_plane_residual_m = 0.015;
    nominal_metrics.min_hessian_eigenvalue = 180.0;
    nominal_metrics.is_degenerate = false;

    const auto q_nom = monitor.evaluateFrameQuality(nominal_state, nominal_metrics, 12.0);
    if (q_nom.overall_quality_score < 0.75 || q_nom.rescan_recommended || q_nom.tracking_lost) {
        std::printf("  [M9 FAIL] Nominal quality report unexpectedly flagged error (score=%.2f)\n",
                    q_nom.overall_quality_score);
        return false;
    }

    slam::EstimatorMetrics degen_metrics = nominal_metrics;
    degen_metrics.is_degenerate = true;
    degen_metrics.valid_geometric_correspondences = 40;
    const auto q_deg = monitor.evaluateFrameQuality(nominal_state, degen_metrics, 95.0);
    if (!q_deg.degenerate_corridor_warning || !q_deg.rescan_recommended) {
        std::printf("  [M9 FAIL] Degenerate high-drift state did not trigger operator warnings\n");
        return false;
    }

    // 2. Test Dynamic Object Filtering (pedestrian walking through static room)
    std::vector<core::KeyframeSubmap> submaps;
    for (uint64_t k = 0; k < 4; ++k) {
        core::KeyframeSubmap sm;
        sm.keyframe_id = k;
        sm.T_world_submap = math::SE3d::Identity();
        // Persistent static wall points observed in all 4 keyframes
        for (int i = 0; i < 50; ++i) {
            core::LidarPointXYZIRT pt;
            pt.x = static_cast<float>(i * 0.1);
            pt.y = 3.0f;
            pt.z = 0.5f;
            sm.local_points.push_back(pt);
        }
        // Transient pedestrian cluster present in only 1 keyframe at each location
        core::LidarPointXYZIRT dyn_pt;
        dyn_pt.x = 1.0f + static_cast<float>(k * 2.5);
        dyn_pt.y = 0.0f;
        dyn_pt.z = 0.2f;
        sm.local_points.push_back(dyn_pt);
        submaps.push_back(sm);
    }

    const auto filtered_pts = quality::QualityMonitor::filterDynamicObjects(submaps, 0.25, 2);
    for (const auto& pt : filtered_pts) {
        if (std::abs(pt.y) < 0.5f) {
            std::printf("  [M9 FAIL] Dynamic transient pedestrian point was not removed by filterDynamicObjects\n");
            return false;
        }
    }
    if (filtered_pts.size() < 200) {
        std::printf("  [M9 FAIL] Static wall points were over-filtered (kept=%zu)\n", filtered_pts.size());
        return false;
    }

    // 3. Test Automatic 6-DOF Relocalization after tracking loss
    backend::ScanContextDatabase sc_db(5, 0.28);
    std::vector<core::KeyframeSubmap> reloc_submaps;
    core::KeyframeSubmap ref_sm;
    ref_sm.keyframe_id = 0;
    ref_sm.T_world_submap = math::SE3d(math::SO3d::Identity(), math::Vec3d(4.0, -3.0, 0.0));
    for (double x = -5.0; x <= 5.0; x += 0.4) {
        for (double y = -5.0; y <= 5.0; y += 0.4) {
            double z_top = (x > 2.0 && y > 1.0) ? 2.2 : -1.0;
            for (double z = -1.0; z <= z_top + 1e-3; z += 0.4) {
                core::LidarPointXYZIRT pt;
                pt.x = static_cast<float>(x);
                pt.y = static_cast<float>(y);
                pt.z = static_cast<float>(z);
                ref_sm.local_points.push_back(pt);
            }
        }
    }
    reloc_submaps.push_back(ref_sm);
    sc_db.addKeyframeDescriptor(backend::ScanContextDatabase::computeDescriptor(0, 1.0, ref_sm.local_points));

    core::LidarFrame lost_scan;
    lost_scan.points = ref_sm.local_points;
    backend::GicpVerifier gicp;
    math::SE3d recovered_pose;
    uint64_t matched_id = 999;
    double reloc_rmse = 10.0;
    if (!monitor.attemptRelocalization(lost_scan, sc_db, reloc_submaps, gicp, recovered_pose, matched_id, reloc_rmse)) {
        std::printf("  [M9 FAIL] attemptRelocalization failed to recover pose\n");
        return false;
    }
    if (matched_id != 0 || (recovered_pose.translation() - ref_sm.T_world_submap.translation()).norm() > 0.20) {
        std::printf("  [M9 FAIL] Relocalized pose inaccurate\n");
        return false;
    }

    // 4. Test Live 2D Floor Plan Generation & Reference Distance Accuracy Qualification
    std::vector<backend::PoseGraphFactor> factors;
    const auto floor_plan = quality::QualityMonitor::generateLiveFloorPlan(ref_sm.local_points, reloc_submaps, factors, 0.20);
    if (floor_plan.width_cells <= 0 || floor_plan.height_cells <= 0 || floor_plan.occupancy_cells.empty()) {
        std::printf("  [M9 FAIL] generateLiveFloorPlan produced invalid grid\n");
        return false;
    }

    std::vector< std::tuple<std::string, math::Vec3d, math::Vec3d, double> > ref_checks = {
        {"Corridor_Length", math::Vec3d(0.0, 0.0, 0.0), math::Vec3d(10.012, 0.0, 0.0), 10.000},
        {"Room_Width", math::Vec3d(0.0, 0.0, 0.0), math::Vec3d(0.0, 4.991, 0.0), 5.000}
    };
    const auto qual = quality::QualityMonitor::evaluateReferenceDistances(ref_checks);
    if (qual.num_checks != 2 || qual.rmse_m > 0.02) {
        std::printf("  [M9 FAIL] evaluateReferenceDistances computed unexpected RMSE=%.4f\n", qual.rmse_m);
        return false;
    }

    return true;
}
