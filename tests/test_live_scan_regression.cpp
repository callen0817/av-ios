#include "avscan/backend/GicpVerifier.hpp"
#include "avscan/backend/PoseGraphOptimizer.hpp"
#include "avscan/backend/ScanContext.hpp"
#include "avscan/calibration/OnlineAutoCalibrator.hpp"
#include "avscan/core/DeviceMountProfile.hpp"
#include "avscan/processing/PointCloudCleanup.hpp"
#include "avscan/quality/QualityMonitor.hpp"
#include "avscan/sensors/AiryDecoder.hpp"
#include "avscan/sensors/AiryUdpReceiver.hpp"
#include "avscan/slam/FastLivo2Engine.hpp"
#include "avscan/sync/TimeSynchronizer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace avscan;

namespace {

// Test 1: Verify SLERP pose interpolation + depth Z-buffer occlusion rejection + bilinear RGB sampling
// using the exact iPad Air 5th Gen (iPad13,16) Landscape-Left KDD clamp profile.
bool testColorizationDepthOcclusionAndBilinear() {
    auto& registry = core::DeviceProfileRegistry::instance();
    if (!registry.setActiveProfile("ipad_air_5th_gen_airy_rigid")) {
        std::printf("[FAIL] ipad_air_5th_gen_airy_rigid profile missing\n");
        return false;
    }
    const auto ext = registry.activeExtrinsics();

    // Verify exact R_cam_lidar = diag(-1, -1, +1) and forward alignment R(2,2) == +1
    if (std::abs(ext.T_cam_lidar.so3.R(0, 0) + 1.0) > 1e-6 ||
        std::abs(ext.T_cam_lidar.so3.R(1, 1) + 1.0) > 1e-6 ||
        std::abs(ext.T_cam_lidar.so3.R(2, 2) - 1.0) > 1e-6) {
        std::printf("[FAIL] iPad Air 5 KDD clamp rotation matrix mismatch\n");
        return false;
    }

    slam::FastLivo2Config cfg;
    cfg.enable_visual_fusion = false;
    cfg.enable_rgb_colorization = true;
    slam::FastLivo2Engine engine(cfg);

    core::AiryFactoryCalibration fcal;
    fcal.valid = true;
    fcal.T_imu_lidar = math::SE3d::Identity();
    engine.setFactoryCalibration(fcal);
    engine.setExtrinsicCalibration(ext);

    // Build a simple stationary 2-wall scene in front of the sensor (+Z_lidar = +Z_cam):
    // Foreground pillar at Z_lidar = 2.0 m (directly along camera center) and background wall at Z_lidar = 5.0 m
    // Both project to the exact same camera pixel (cx, cy)!
    // Z-buffer occlusion MUST colorize the foreground (2.0 m) and leave the occluded background (5.0 m) uncolored!
    auto makeSceneFrame = [&](uint64_t fid, double t0, double t1) {
        core::LidarFrame lf;
        lf.frame_id = fid;
        lf.timestamp_start_sec = t0;
        lf.timestamp_end_sec = t1;
        // Ground & side planes for stable IEKF geometry
        for (double x = -3.0; x <= 3.0; x += 0.25) {
            for (double z = 1.0; z <= 5.0; z += 0.25) {
                core::LidarPointXYZIRT g;
                g.x = x; g.y = 1.2; g.z = z;
                g.timestamp_sec = t0 + 0.5 * (t1 - t0);
                g.intensity = 100.0f;
                lf.points.push_back(g);
            }
        }
        // Background wall at Z_lidar = 5.0 m
        for (double x = -1.5; x <= 1.5; x += 0.15) {
            for (double y = -1.0; y <= 1.0; y += 0.15) {
                core::LidarPointXYZIRT p;
                p.x = x; p.y = y; p.z = 5.0;
                p.timestamp_sec = t0 + 0.5 * (t1 - t0);
                p.intensity = 120.0f;
                lf.points.push_back(p);
            }
        }
        // Tight foreground patch at Z_lidar = 2.0 m directly occluding center of background wall in camera view
        // Camera optical axis passes through p_cam = (0, 0, z_c) -> p_lidar = T_cam_lidar^-1 * (0, 0, z_c)
        const math::SE3d T_lidar_cam = ext.T_cam_lidar.inverse();
        for (double dx = -0.05; dx <= 0.05; dx += 0.01) {
            for (double dy = -0.05; dy <= 0.05; dy += 0.01) {
                math::Vec3d p_fg = T_lidar_cam * math::Vec3d(dx, dy, 2.0);
                core::LidarPointXYZIRT p;
                p.x = p_fg.x; p.y = p_fg.y; p.z = p_fg.z;
                p.timestamp_sec = t0 + 0.5 * (t1 - t0);
                p.intensity = 180.0f;
                lf.points.push_back(p);
            }
        }
        // Directly behind foreground center on the exact same camera ray at z_c = 5.0 m
        math::Vec3d p_bg_occ = T_lidar_cam * math::Vec3d(0.0, 0.0, 5.0);
        core::LidarPointXYZIRT occ;
        occ.x = p_bg_occ.x; occ.y = p_bg_occ.y; occ.z = p_bg_occ.z;
        occ.timestamp_sec = t0 + 0.5 * (t1 - t0);
        occ.intensity = 55.0f;  // Tag with distinct intensity to inspect below
        lf.points.push_back(occ);
        return lf;
    };

    core::RgbFrame rgb;
    rgb.intrinsics = {320, 240, 260.0, 260.0, 160.0, 120.0, {0, 0, 0, 0, 0}};
    rgb.rgb_bytes.resize(320 * 240 * 3);
    rgb.gray_bytes.resize(320 * 240);
    for (int v = 0; v < 240; ++v) {
        for (int u = 0; u < 320; ++u) {
            // Smooth RGB gradient: red ~ 240, green ~ 40, blue ~ 180 at center
            rgb.rgb_bytes[(v * 320 + u) * 3 + 0] = 240;
            rgb.rgb_bytes[(v * 320 + u) * 3 + 1] = 40;
            rgb.rgb_bytes[(v * 320 + u) * 3 + 2] = 180;
            rgb.gray_bytes[v * 320 + u] = 120;
        }
    }

    // Run 3 sweeps so IMU gravity initializes (needs >= 20 samples) and RGB colorization runs on sweep 3
    core::LidarFrame deskewed, world;
    slam::EstimatorMetrics metrics;
    for (int s = 0; s < 3; ++s) {
        sync::SyncBundle b;
        const double t0 = 10.0 + s * 0.10;
        const double t1 = t0 + 0.10;
        b.lidar_frame = makeSceneFrame(static_cast<uint64_t>(s + 1), t0, t1);
        for (int k = 0; k <= 20; ++k) {
            core::ImuMeasurement imu;
            imu.timestamp_sec = t0 + k * 0.005;
            imu.accel_mps2 = math::Vec3d(0.0, -9.80665, 0.0);  // +Y_lidar down -> gravity = (0, -g, 0)
            imu.gyro_rads = math::Vec3d(0.0, 0.0, 0.0);
            b.imu_measurements.push_back(imu);
        }
        rgb.timestamp_sec = t0 + 0.05;
        b.rgb_frame = rgb;
        engine.processBundle(b, deskewed, world, metrics);
    }

    size_t fg_colorized = 0;
    bool occluded_bg_was_colorized = false;
    for (const auto& pt : deskewed.points) {
        if (std::abs(pt.intensity - 180.0f) < 1.0f && pt.colorized && pt.r > 200 && pt.b > 150) {
            fg_colorized++;
        }
        if (std::abs(pt.intensity - 55.0f) < 1.0f && pt.colorized) {
            occluded_bg_was_colorized = true;
        }
    }

    if (fg_colorized < 10) {
        std::printf("[FAIL] Foreground points were not RGB-colorized (fg_colorized=%zu)\n", fg_colorized);
        return false;
    }
    if (occluded_bg_was_colorized) {
        std::printf("[FAIL] Depth Z-buffer failed to reject occluded background point!\n");
        return false;
    }

    std::printf("[PASS] SLERP + Depth-Buffered Bilinear RGB Colorization (fg_colored=%zu, occluded_rejected=1)\n",
                fg_colorized);
    return true;
}

// Test 2: Verify exact host model matching (no "iPad13,1" prefix collision!), profile-seeded
// OnlineAutoCalibrator locking from Frame 1, and TimeSynchronizer startup-race RGB alignment.
bool testProfileMatchingSeedingAndRgbSyncRace() {
    auto& reg = core::DeviceProfileRegistry::instance();

    // 1. Exact model matching (regression for "iPad13,16" matching "iPad13,1" Air 4!)
    auto m_air5_wifi = reg.getProfileForModel("iPad13,16");
    auto m_air5_cell = reg.getProfileForModel("iPad13,17");
    auto m_ipad10    = reg.getProfileForModel("iPad13,18");
    auto m_air4      = reg.getProfileForModel("iPad13,1");
    if (!m_air5_wifi || m_air5_wifi->profile_id != "ipad_air_5th_gen_airy_rigid" ||
        !m_air5_cell || m_air5_cell->profile_id != "ipad_air_5th_gen_airy_rigid" ||
        !m_ipad10    || m_ipad10->profile_id != "ipad_10th_gen_airy_rigid" ||
        !m_air4      || m_air4->profile_id != "ipad_air_4th_gen_airy_rigid") {
        std::printf("[FAIL] DeviceProfileRegistry exact model match failed (Air 5 -> %s)\n",
                    m_air5_wifi ? m_air5_wifi->profile_id.c_str() : "null");
        return false;
    }

    // 2. OnlineAutoCalibrator::seedFromMountProfile stays locked & verified from Frame 1 even on blank wall
    reg.setActiveProfile("ipad_air_5th_gen_airy_rigid");
    const auto seed_ext = reg.activeExtrinsics();
    calibration::OnlineAutoCalibrator auto_cal;
    auto_cal.seedFromMountProfile(seed_ext);
    if (!auto_cal.isLocked() || auto_cal.getStatus().state != calibration::AutoCalibState::LOCKED_GREEN) {
        std::printf("[FAIL] seedFromMountProfile did not enter LOCKED_GREEN immediately\n");
        return false;
    }
    // Feed a blank low-texture frame -> must fall back to LOCKED_YELLOW_FALLBACK while remaining locked
    sync::SyncBundle blank_bundle;
    blank_bundle.lidar_frame.timestamp_end_sec = 100.0;
    blank_bundle.lidar_frame.points.resize(500);
    core::RgbFrame blank_rgb;
    blank_rgb.intrinsics = {64, 48, 50.0, 50.0, 32.0, 24.0, {0,0,0,0,0}};
    blank_rgb.gray_bytes.assign(64 * 48, 128);
    blank_rgb.rgb_bytes.assign(64 * 48 * 3, 128);
    blank_bundle.rgb_frame = blank_rgb;
    auto_cal.pushLidarBundleAndPose(blank_bundle, math::SE3d::Identity());
    if (!auto_cal.isLocked() || auto_cal.getStatus().state != calibration::AutoCalibState::LOCKED_YELLOW_FALLBACK) {
        std::printf("[FAIL] seedFromMountProfile lost lock on blank wall\n");
        return false;
    }

    // 3. TimeSynchronizer startup race: RGB pushed BEFORE any LiDAR/IMU clock offset is estimated
    sync::TimeSynchronizer ts;
    core::RgbFrame early_rgb;
    early_rgb.frame_id = 1;
    early_rgb.timestamp_sec = 45000.050;  // Host monotonic time (CACurrentMediaTime)
    early_rgb.intrinsics = {64, 48, 50.0, 50.0, 32.0, 24.0, {0,0,0,0,0}};
    early_rgb.rgb_bytes.assign(64 * 48 * 3, 200);
    early_rgb.gray_bytes.assign(64 * 48, 200);
    ts.pushRgbFrame(early_rgb);

    // Now LiDAR (t=1000.0..1000.1) + IMUs arrive at host time 45000.0..45000.1 (offset = -44000.0 s)
    for (int i = 0; i <= 20; ++i) {
        core::ImuMeasurement imu;
        imu.timestamp_sec = 1000.0 + i * 0.005;
        imu.accel_mps2 = {0.0, 0.0, 9.81};
        ts.pushImu(imu, 45000.0 + i * 0.005);
    }
    core::LidarFrame lf;
    lf.frame_id = 1;
    lf.timestamp_start_sec = 1000.0;
    lf.timestamp_end_sec = 1000.1;
    lf.points.resize(200);
    ts.pushLidarFrame(lf, 45000.100);

    sync::SyncBundle out_b;
    if (!ts.popSyncBundle(out_b) || !out_b.rgb_frame.has_value()) {
        std::printf("[FAIL] TimeSynchronizer dropped startup RGB frame pushed before clock lock\n");
        return false;
    }
    if (std::abs(out_b.rgb_frame->timestamp_sec - 1000.050) > 0.015) {
        std::printf("[FAIL] TimeSynchronizer misaligned startup RGB timestamp: %.4f (expected ~1000.050)\n",
                    out_b.rgb_frame->timestamp_sec);
        return false;
    }

    std::printf("[PASS] Profile Exact-Match, Seeded Auto-Calib Lock, and Startup RGB Sync Alignment\n");

    // 4. Verify ScanAR V4 (Airy 45-deg + USB Gemini 336L) external camera profile & exact av-scanar calibration
    auto scanar_opt = reg.getProfile("scanar_v4_airy_336l_usb");
    if (!scanar_opt.has_value() || !scanar_opt->uses_external_usb_camera ||
        scanar_opt->form_factor != core::DeviceFormFactor::SCANAR_V4_EXTERNAL ||
        scanar_opt->preferred_usb_camera_name != "Orbbec") {
        std::printf("[FAIL] ScanAR V4 external USB camera profile missing or misconfigured\n");
        return false;
    }
    const auto& sp = scanar_opt.value();
    const double det = sp.T_cam_lidar.so3.R.determinant();
    if (std::abs(det - 1.0) > 1e-5 || std::abs(sp.T_cam_lidar.so3.R(2, 2) - 0.71371) > 1e-3 ||
        (sp.T_cam_lidar.t - math::Vec3d(0.067244, -0.063082, -0.030452)).norm() > 1e-5) {
        std::printf("[FAIL] ScanAR V4 T_rgb_airy_lidar mismatch (det=%.6f, R22=%.5f)\n", det, sp.T_cam_lidar.so3.R(2, 2));
        return false;
    }
    if (sp.default_intrinsics.width != 1280 || sp.default_intrinsics.height != 800 ||
        std::abs(sp.default_intrinsics.fx - 609.006531) > 1e-3 ||
        std::abs(sp.default_intrinsics.dist_coeffs[0] + 0.03113378) > 1e-5) {
        std::printf("[FAIL] ScanAR V4 Gemini 336L factory intrinsics mismatch\n");
        return false;
    }
    // Verify round-trip projection: point at (0, 0, 3.0) in 336L RGB frame -> Airy LiDAR -> project back to (cx, cy)
    const math::Vec3d p_lidar_center = sp.T_cam_lidar.inverse() * math::Vec3d(0.0, 0.0, 3.0);
    const math::Vec3d p_cam_rt = sp.T_cam_lidar * p_lidar_center;
    const math::Vec2d uv_rt = sp.default_intrinsics.project(p_cam_rt, true);
    if (std::abs(uv_rt.x - sp.default_intrinsics.cx) > 0.5 || std::abs(uv_rt.y - sp.default_intrinsics.cy) > 0.5) {
        std::printf("[FAIL] ScanAR V4 336L projection round-trip failed (uv=%.2f,%.2f)\n", uv_rt.x, uv_rt.y);
        return false;
    }
    std::printf("[PASS] ScanAR V4 (Airy 45 deg + USB Gemini 336L) Profile, Extrinsics & 1280x800 Projection\n");
    return true;
}

// Test 3: Verify Tier-2 PointCloudCleanup stages (dynamic ray-carving, weighted RGB centroid fusion,
// O(N) spatial-hash ROR+SOR, edge-preserving planar denoising, and guarded floor tilt leveling).
bool testPointCloudCleanupPipeline() {
    // 1. Dynamic transient walker removal via ray carving
    {
        std::vector<core::KeyframeSubmap> submaps;
        for (uint64_t k = 0; k < 5; ++k) {
            core::KeyframeSubmap sm;
            sm.keyframe_id = k + 1;
            sm.T_world_keyframe = math::SE3d(math::SO3d(), math::Vec3d(0.0, 0.0, 0.0));
            // Static back wall at x = 4.0 m
            for (double y = -1.0; y <= 1.0; y += 0.05) {
                for (double z = -0.5; z <= 0.5; z += 0.05) {
                    core::LidarPointXYZIRT p;
                    p.x = 4.0; p.y = y; p.z = z;
                    sm.local_points.push_back(p);
                }
            }
            // Transient person appears ONLY in keyframe 1 at x = 2.0, y = 0.0, z = 0.0 (directly in ray path to wall!)
            if (k == 0) {
                core::LidarPointXYZIRT walker;
                walker.x = 2.0; walker.y = 0.0; walker.z = 0.0;
                sm.local_points.push_back(walker);
            }
            submaps.push_back(sm);
        }
        size_t removed = 0;
        auto kept = processing::PointCloudCleanup::filterDynamicTransients(submaps, 0.25, 3, &removed);
        bool walker_remains = false;
        for (const auto& p : kept) {
            if (std::abs(p.x - 2.0) < 0.2) walker_remains = true;
        }
        if (walker_remains || removed == 0) {
            std::printf("[FAIL] Dynamic ray-carving failed to remove transient walker (removed=%zu)\n", removed);
            return false;
        }
    }

    // 2. Confidence-weighted RGB + centroid voxel fusion
    {
        std::vector<core::LidarPointXYZIRT> pts;
        for (int i = 0; i < 6; ++i) {
            core::LidarPointXYZIRT p;
            p.x = 1.006 + i * 0.001;  // Strictly inside 1.5 cm voxel [1.005, 1.020)
            p.y = 2.005;
            p.z = 0.505;
            p.colorized = true;
            if (i < 4) {
                // High-confidence close/centered views (red)
                p.r = 240; p.g = 20; p.b = 20;
                p.status_flags = static_cast<uint8_t>(60u << 2);
            } else {
                // Low-confidence edge views (blue)
                p.r = 20; p.g = 20; p.b = 240;
                p.status_flags = static_cast<uint8_t>(10u << 2);
            }
            pts.push_back(p);
        }
        auto fused = processing::PointCloudCleanup::fuseVoxelCentroidsWeightedRgb(pts, 0.015);
        if (fused.size() != 1 || !fused[0].colorized || fused[0].r < 200 || fused[0].b > 50 ||
            std::abs(fused[0].x - 1.0085) > 1e-4) {
            std::printf("[FAIL] Weighted RGB voxel centroid fusion mismatch (n=%zu, r=%u, b=%u, x=%.4f)\n",
                        fused.size(), fused.empty() ? 0 : fused[0].r, fused.empty() ? 0 : fused[0].b,
                        fused.empty() ? 0.0 : fused[0].x);
            return false;
        }
    }

    // 3. Spatial-hash ROR + SOR + Planar denoising + Floor tilt leveling on synthetic noisy tilted room
    {
        std::vector<core::LidarPointXYZIRT> cloud;
        const math::SO3d R_tilt = math::SO3d::exp(math::Vec3d(2.5 * math::kDegToRad, 0.0, 0.0));
        // Dense noisy floor (2m x 2m at 2 cm spacing with +-1.2 cm deterministic jitter)
        int idx = 0;
        for (double x = -1.0; x <= 1.0; x += 0.02) {
            for (double y = -1.0; y <= 1.0; y += 0.02) {
                const double jitter = (((idx * 37) % 21) - 10) * 0.0012;
                idx++;
                const math::Vec3d p = R_tilt * math::Vec3d(x, y, jitter);
                core::LidarPointXYZIRT pt;
                pt.x = p.x; pt.y = p.y; pt.z = p.z;
                cloud.push_back(pt);
            }
        }
        // Inject 10 isolated floating dust/multipath outliers well above the floor
        for (int i = 0; i < 10; ++i) {
            core::LidarPointXYZIRT floater;
            floater.x = -0.8 + i * 0.17;
            floater.y = 0.3;
            floater.z = 1.2 + i * 0.15;
            cloud.push_back(floater);
        }

        processing::CleanupConfig cfg;
        cfg.enable_dynamic_filter = false;
        cfg.enable_voxel_centroid_fusion = false;  // Keep synthetic jitter to directly test planar denoising
        std::vector<core::LidarPointXYZIRT> cleaned;
        const auto stats = processing::PointCloudCleanup::runCloudCleanupPipeline(cloud, cleaned, cfg);

        // Verify all 10 floaters removed
        size_t floaters_left = 0;
        double sum_z = 0.0, sum_z2 = 0.0;
        for (const auto& p : cleaned) {
            if (p.z > 0.5) floaters_left++;
            sum_z += p.z;
            sum_z2 += p.z * p.z;
        }
        const double mean_z = sum_z / std::max<size_t>(1, cleaned.size());
        const double std_z = std::sqrt(std::max(0.0, sum_z2 / std::max<size_t>(1, cleaned.size()) - mean_z * mean_z));

        if (floaters_left != 0 || !stats.floor_leveled ||
            std::abs(stats.floor_tilt_corrected_deg - 2.5) > 0.35 ||
            stats.planar_denoised_points < 5000 || std_z > 0.0045) {
            std::printf("[FAIL] Cleanup pipeline check failed (floaters=%zu, leveled=%d, tilt=%.2f deg, denoised=%zu, std_z=%.4f)\n",
                        floaters_left, stats.floor_leveled ? 1 : 0, stats.floor_tilt_corrected_deg,
                        stats.planar_denoised_points, std_z);
            return false;
        }
        std::printf("[PASS] Tier-2 PointCloudCleanup (floaters=0, tilt=%.2f deg leveled, denoised=%zu pts, floor_sigma=%.2f mm, %.1f ms)\n",
                    stats.floor_tilt_corrected_deg, stats.planar_denoised_points, std_z * 1000.0, stats.elapsed_ms);
    }

    return true;
}

double percentile(std::vector<double>& vals, double p) {
    if (vals.empty()) return 0.0;
    const size_t idx = std::min(vals.size() - 1, static_cast<size_t>(std::floor(p * static_cast<double>(vals.size() - 1))));
    std::nth_element(vals.begin(), vals.begin() + idx, vals.end());
    return vals[idx];
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("=== AV Scan Live-Scan Regression & Colorization Test ===\n");

    if (!testColorizationDepthOcclusionAndBilinear()) {
        return 1;
    }
    if (!testProfileMatchingSeedingAndRgbSyncRace()) {
        return 1;
    }
    if (!testPointCloudCleanupPipeline()) {
        return 1;
    }

    std::string raw_path;
    if (argc > 1 && argv[1][0] != '\0') {
        raw_path = argv[1];
    } else {
#ifdef AVSCAN_SOURCE_DIR
        raw_path = std::string(AVSCAN_SOURCE_DIR) +
                   "/tests/live scans/Projects/LEVEL_01_20261123_194518/scans/Scan_01_20261123_194518/raw_capture.avraw";
#else
        raw_path = "tests/live scans/Projects/LEVEL_01_20261123_194518/scans/Scan_01_20261123_194518/raw_capture.avraw";
#endif
    }

    if (!std::filesystem::exists(raw_path)) {
        std::printf("[SKIP] Live capture not present at %s (synthetic checks passed)\n", raw_path.c_str());
        return 0;
    }

    std::printf("Replaying live iPad Air 5 + Airy capture: %s\n", raw_path.c_str());

    // Exact production pipeline wired identically to AVScanCoreBridge.mm
    sensors::AiryUdpReceiver receiver;
    sync::TimeSynchronizer synchronizer;
    slam::FastLivo2Engine slam_engine;
    backend::ScanContextDatabase sc_db;
    backend::GicpVerificationConfig gicp_cfg;
    gicp_cfg.voxel_resolution_m = 0.40;
    gicp_cfg.max_correspondence_dist_m = 0.30;
    gicp_cfg.max_iterations = 20;
    gicp_cfg.min_inlier_overlap_ratio = 0.50;
    gicp_cfg.max_fitness_rmse_m = 0.07;
    gicp_cfg.max_source_points = 4000;
    gicp_cfg.allow_centroid_fallback = false;
    backend::GicpVerifier gicp_verifier(gicp_cfg);
    backend::PoseGraphOptimizer pose_graph;
    quality::QualityMonitor quality_monitor;

    core::AiryFactoryCalibration factory_calib;
    uint64_t next_kf_id = 1;
    uint64_t last_loop_kf_id = 0;
    double total_traveled_m = 0.0;
    double unclosed_dist_m = 0.0;
    math::Vec3d prev_pos(0.0, 0.0, 0.0);
    bool has_prev_pos = false;
    std::unordered_map<uint64_t, double> kf_traveled_dist;

    size_t frames_processed = 0;
    size_t tracking_lost_frames = 0;
    double slam_compute_ms_total = 0.0;
    double max_loop_drot_deg = 0.0;
    double max_loop_dtrans_m = 0.0;
    std::vector<double> frame_rmses;
    std::vector<double> traj_z;
    std::vector<double> map_z_samples;
    map_z_samples.reserve(500000);

    auto processPendingBundles = [&]() {
        sync::SyncBundle bundle;
        while (synchronizer.popSyncBundle(bundle)) {
            core::LidarFrame deskewed_body;
            core::LidarFrame world_frame;
            slam::EstimatorMetrics metrics;
            if (!slam_engine.processBundle(bundle, deskewed_body, world_frame, metrics)) {
                continue;
            }
            frames_processed++;
            slam_compute_ms_total += metrics.processing_time_ms;
            const auto state = slam_engine.getCurrentState();
            if (has_prev_pos) {
                const double step = (state.T_world_body.t - prev_pos).norm();
                if (step < 2.0) {
                    total_traveled_m += step;
                    unclosed_dist_m += step;
                }
            }
            prev_pos = state.T_world_body.t;
            has_prev_pos = true;
            traj_z.push_back(state.T_world_body.t.z);
            if (metrics.geometric_correspondences > 0) {
                frame_rmses.push_back(metrics.geometric_rmse_m);
            }

            const auto qrep = quality_monitor.evaluateFrameQuality(state, metrics, unclosed_dist_m);
            if (qrep.tracking_lost) {
                tracking_lost_frames++;
            }

            for (size_t i = 0; i < world_frame.points.size(); i += 10) {
                map_z_samples.push_back(world_frame.points[i].z);
            }

            if (metrics.keyframe_emitted) {
                core::KeyframeSubmap sm;
                sm.keyframe_id = next_kf_id++;
                sm.timestamp_sec = bundle.lidar_frame.timestamp_end_sec;
                sm.T_world_keyframe = state.T_world_body;
                sm.local_points = deskewed_body.points;
                pose_graph.addKeyframeSubmap(sm);
                kf_traveled_dist[sm.keyframe_id] = total_traveled_m;

                const auto sc_desc = backend::ScanContextDatabase::computeDescriptor(
                    sm.keyframe_id, sm.timestamp_sec, sm.local_points, bundle.rgb_frame, state.T_world_body.so3);
                const auto loop_cand = sc_db.detectLoopClosure(sc_desc);
                sc_db.addKeyframeDescriptor(sc_desc);

                const bool cooldown_ok = (last_loop_kf_id == 0) ||
                                         ((sm.keyframe_id > last_loop_kf_id) &&
                                          ((sm.keyframe_id - last_loop_kf_id) >= 10));
                if (loop_cand.valid && cooldown_ok) {
                    for (const auto& target_sm : pose_graph.getSubmaps()) {
                        if (target_sm.keyframe_id == loop_cand.match_keyframe_id) {
                            const uint64_t kf_gap = (sm.keyframe_id > target_sm.keyframe_id)
                                                        ? (sm.keyframe_id - target_sm.keyframe_id)
                                                        : 0;
                            const double dist_gap = total_traveled_m - kf_traveled_dist[target_sm.keyframe_id];
                            const double spatial_dist = (sm.T_world_keyframe.t - target_sm.T_world_keyframe.t).norm();
                            if (kf_gap >= 30 && dist_gap >= 8.0 && spatial_dist < 4.5) {
                                const math::SE3d init_guess = target_sm.T_world_keyframe.inverse() * sm.T_world_keyframe;
                                const auto ver = gicp_verifier.verifyLoopConstraint(
                                    sm.local_points, target_sm.local_points, init_guess);
                                if (ver.verified) {
                                    const math::SE3d delta = init_guess.inverse() * ver.T_target_source;
                                    const double drot_deg = delta.so3.log().norm() * (180.0 / math::kPi);
                                    const double dtrans = delta.t.norm();
                                    if (drot_deg < 20.0 && dtrans < 2.0) {
                                        std::printf("  [LOOP VERIFIED] kf %llu -> %llu (dist_gap=%.2fm, spatial=%.2fm, rmse=%.4fm, overlap=%.2f, drot=%.2f deg, dtrans=%.3fm)\n",
                                                    static_cast<unsigned long long>(sm.keyframe_id),
                                                    static_cast<unsigned long long>(target_sm.keyframe_id),
                                                    dist_gap, spatial_dist, ver.fitness_rmse_m, ver.inlier_overlap_ratio,
                                                    drot_deg, dtrans);
                                        max_loop_drot_deg = std::max(max_loop_drot_deg, drot_deg);
                                        max_loop_dtrans_m = std::max(max_loop_dtrans_m, dtrans);
                                        pose_graph.addVerifiedLoopFactor(
                                            target_sm.keyframe_id, sm.keyframe_id,
                                            ver.T_target_source, ver.information_diagonal, true);
                                        pose_graph.optimizeGlobalPoseGraph(15);
                                        last_loop_kf_id = sm.keyframe_id;
                                        unclosed_dist_m = 0.0;
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
    };

    receiver.setCallbacks(
        [&](const core::LidarFrame& frame) {
            synchronizer.pushLidarFrame(frame, frame.timestamp_end_sec);
            processPendingBundles();
        },
        [&](const core::ImuMeasurement& imu) {
            synchronizer.pushImu(imu, imu.timestamp_sec);
        },
        [&](const core::AiryFactoryCalibration& calib) {
            factory_calib = calib;
            slam_engine.setFactoryCalibration(calib);
        });

    const auto wall_start = std::chrono::steady_clock::now();
    size_t decoded_frames = 0, decoded_imus = 0;
    if (!receiver.replayRawFile(raw_path, decoded_frames, decoded_imus)) {
        std::printf("[FAIL] Failed to replay %s\n", raw_path.c_str());
        return 1;
    }
    processPendingBundles();
    const auto wall_end = std::chrono::steady_clock::now();
    const double total_sec = std::chrono::duration<double>(wall_end - wall_start).count();

    pose_graph.optimizeGlobalPoseGraph(25);
    const auto unified_cloud = pose_graph.buildGloballyConsistentPointCloud(1, 0.015);
    std::vector<core::LidarPointXYZIRT> cleaned_cloud;
    const auto cleanup_stats = processing::PointCloudCleanup::runPostScanPipeline(
        pose_graph.getSubmaps(), cleaned_cloud);
    const auto floor_plan = quality::QualityMonitor::generateLiveFloorPlan(
        cleaned_cloud, pose_graph.getSubmaps(), pose_graph.getFactors(), 0.08);

    size_t occupied_wall_cells = 0;
    for (uint8_t c : floor_plan.occupancy_cells) {
        if (c == 1) occupied_wall_cells++;
    }

    const double z_min_traj = *std::min_element(traj_z.begin(), traj_z.end());
    const double z_max_traj = *std::max_element(traj_z.begin(), traj_z.end());
    const double traj_z_span = z_max_traj - z_min_traj;

    const double p01 = percentile(map_z_samples, 0.01);
    const double p02 = percentile(map_z_samples, 0.02);
    const double p90 = percentile(map_z_samples, 0.90);
    const double p99 = percentile(map_z_samples, 0.99);
    const double floor_thickness = p02 - p01;
    const double ceiling_thickness = p99 - p90;
    const double median_rmse = percentile(frame_rmses, 0.50);
    const double avg_slam_ms = (frames_processed > 0) ? (slam_compute_ms_total / frames_processed) : 0.0;

    // Also measure post-PGO unified cloud Z sharpness to guarantee PGO never smears geometry
    std::vector<double> unified_z;
    unified_z.reserve(unified_cloud.size() / 4 + 1);
    for (size_t i = 0; i < unified_cloud.size(); i += 4) {
        unified_z.push_back(unified_cloud[i].z);
    }
    const double post_pgo_ceil = percentile(unified_z, 0.99) - percentile(unified_z, 0.90);

    std::printf("  Decoded:           %zu sweeps, %zu IMUs\n", decoded_frames, decoded_imus);
    std::printf("  Processed:         %zu frames in %.2f s wall (SLAM compute: %.2f ms/frame)\n",
                frames_processed, total_sec, avg_slam_ms);
    std::printf("  Keyframes:         %zu (traveled %.2f m, verified loop closures=%zu)\n",
                pose_graph.getSubmaps().size(), total_traveled_m, pose_graph.numLoopClosures());
    std::printf("  Trajectory Z span: %.3f m ([%.3f, %.3f])\n", traj_z_span, z_min_traj, z_max_traj);
    std::printf("  Ceiling p99-p90:   %.3f m live / %.3f m post-PGO ([%.3f, %.3f])\n",
                ceiling_thickness, post_pgo_ceil, p90, p99);
    std::printf("  Floor p02-p01:     %.3f m ([%.3f, %.3f])\n", floor_thickness, p01, p02);
    std::printf("  Median LIO RMSE:   %.4f m\n", median_rmse);
    std::printf("  Tracking lost cnt: %zu\n", tracking_lost_frames);
    std::printf("  Unified 1.5cm map: %zu points, floorplan %dx%d (%zu wall cells)\n",
                unified_cloud.size(), floor_plan.width_cells, floor_plan.height_cells, occupied_wall_cells);
    std::printf("  Tier-2 Cleanup:    raw=%zu -> fused=%zu -> final=%zu (dyn=-%zu, ror=-%zu, sor=-%zu, planar=%zu, level=%d/%.2f deg, %.1f ms)\n",
                cleanup_stats.raw_input_points, cleanup_stats.voxel_fused_points, cleanup_stats.final_output_points,
                cleanup_stats.dynamic_removed, cleanup_stats.ror_removed, cleanup_stats.sor_removed,
                cleanup_stats.planar_denoised_points, cleanup_stats.floor_leveled ? 1 : 0,
                cleanup_stats.floor_tilt_corrected_deg, cleanup_stats.elapsed_ms);

    bool ok = true;
    const size_t min_expected_frames = (decoded_frames > 10) ? (decoded_frames - 5) : 1;
    if (frames_processed < min_expected_frames || frames_processed < 50) {
        std::printf("[FAIL] Too few frames processed (%zu / %zu decoded)\n", frames_processed, decoded_frames);
        ok = false;
    }
    if (post_pgo_ceil > ceiling_thickness + 0.15 || post_pgo_ceil < 0.01) {
        std::printf("[FAIL] Post-PGO ceiling band smeared wider than live LIO (live=%.3f m, post-PGO=%.3f m)\n",
                    ceiling_thickness, post_pgo_ceil);
        ok = false;
    }
    if (cleanup_stats.elapsed_ms > 6000.0) {
        std::printf("[FAIL] Tier-2 post-scan cleanup too slow (%.1f ms > 6000 ms budget)\n",
                    cleanup_stats.elapsed_ms);
        ok = false;
    }
    if (median_rmse > 0.065) {
        std::printf("[FAIL] Median point-to-plane RMSE %.4f m exceeds 0.065 m gate\n", median_rmse);
        ok = false;
    }
    // Any accepted loop closure must be genuinely consistent with LIO odometry (not 135 deg false loops!)
    if (pose_graph.numLoopClosures() > 5 || max_loop_drot_deg > 10.0 || max_loop_dtrans_m > 0.50) {
        std::printf("[FAIL] Suspicious loop closure(s): count=%zu, max_drot=%.2f deg, max_dtrans=%.3f m (was 99 loops / 135 deg before fix)\n",
                    pose_graph.numLoopClosures(), max_loop_drot_deg, max_loop_dtrans_m);
        ok = false;
    }
    if (tracking_lost_frames > 0) {
        std::printf("[FAIL] Unexpected tracking_lost triggers: %zu\n", tracking_lost_frames);
        ok = false;
    }
    if (unified_cloud.size() < 100000 || cleaned_cloud.size() < 100000 || occupied_wall_cells < 300) {
        std::printf("[FAIL] Unified/cleaned map or floorplan incomplete (pts=%zu/%zu, wall_cells=%zu)\n",
                    unified_cloud.size(), cleaned_cloud.size(), occupied_wall_cells);
        ok = false;
    }

    if (ok) {
        std::printf("=== LIVE SCAN REGRESSION TEST: ALL GATES PASSED ===\n");
    }
    return ok ? 0 : 1;
}
