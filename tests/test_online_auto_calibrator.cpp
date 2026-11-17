#include "avscan/calibration/OnlineAutoCalibrator.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace avscan;

namespace {

math::SE3d openCvToArkitCameraPose(const math::SE3d& T_world_cam_cv) {
    // Inverse of diag(1, -1, -1) is itself
    math::Mat3d R_cv_to_arkit = math::Mat3d::Identity();
    R_cv_to_arkit(1, 1) = -1.0;
    R_cv_to_arkit(2, 2) = -1.0;
    return T_world_cam_cv * math::SE3d(math::SO3d(R_cv_to_arkit), math::Vec3d(0.0, 0.0, 0.0));
}

core::RgbFrame makeFeaturelessRgbFrame(int w, int h, double t_sec) {
    core::RgbFrame rgb;
    rgb.timestamp_sec = t_sec;
    rgb.intrinsics.width = w;
    rgb.intrinsics.height = h;
    rgb.intrinsics.fx = 140.0;
    rgb.intrinsics.fy = 140.0;
    rgb.intrinsics.cx = w * 0.5;
    rgb.intrinsics.cy = h * 0.5;
    rgb.gray_bytes.assign(static_cast<size_t>(w * h), 120);
    rgb.rgb_bytes.assign(static_cast<size_t>(w * h * 3), 120);
    return rgb;
}

core::RgbFrame makeSyntheticRgbFrame(int w, int h, double t_sec) {
    core::RgbFrame rgb;
    rgb.timestamp_sec = t_sec;
    rgb.intrinsics.width = w;
    rgb.intrinsics.height = h;
    rgb.intrinsics.fx = 140.0;
    rgb.intrinsics.fy = 140.0;
    rgb.intrinsics.cx = w * 0.5;
    rgb.intrinsics.cy = h * 0.5;
    rgb.gray_bytes.assign(static_cast<size_t>(w * h), 40);
    rgb.rgb_bytes.assign(static_cast<size_t>(w * h * 3), 40);

    // Non-periodic multi-bar edge pattern so edge_ratio >= 0.035 without periodic aliasing
    for (int v = 20; v < h - 20; ++v) {
        for (int u = 25; u < w - 25; ++u) {
            bool bar1 = (u >= 30 && u < 70 && v >= 24 && v < 60);
            bool bar2 = (u >= 82 && u < 130 && v >= 45 && v < 96);
            bool bar3 = (u >= 40 && u < 110 && v >= 75 && v < 82);
            if (bar1 || bar2 || bar3) {
                size_t idx = static_cast<size_t>(v * w + u);
                rgb.gray_bytes[idx] = 220;
                rgb.rgb_bytes[idx * 3 + 0] = 220;
                rgb.rgb_bytes[idx * 3 + 1] = 180;
                rgb.rgb_bytes[idx * 3 + 2] = 60;
            }
        }
    }
    return rgb;
}

core::LidarFrame makeSyntheticLidarFrame(const math::SE3d& T_cam_lidar_gt,
                                         const core::RgbFrame& rgb,
                                         double t_end_sec) {
    core::LidarFrame lidar;
    lidar.timestamp_start_sec = t_end_sec - 0.1;
    lidar.timestamp_end_sec = t_end_sec;

    const math::SE3d T_lidar_cam_gt = T_cam_lidar_gt.inverse();
    for (double yc = -0.7; yc <= 0.7; yc += 0.06) {
        for (double xc = -1.0; xc <= 1.0; xc += 0.06) {
            const double zc = 2.8;
            const math::Vec3d p_cam(xc, yc, zc);
            const math::Vec2d uv = rgb.intrinsics.project(p_cam, false);
            const int ui = static_cast<int>(uv.x);
            const int vi = static_cast<int>(uv.y);
            const bool inside = (ui >= 30 && ui < 70 && vi >= 24 && vi < 60) ||
                                (ui >= 82 && ui < 130 && vi >= 45 && vi < 96) ||
                                (ui >= 40 && ui < 110 && vi >= 75 && vi < 82);

            const math::Vec3d p_lidar = T_lidar_cam_gt.transform(p_cam);
            core::LidarPointXYZIRT pt;
            pt.x = p_lidar.x;
            pt.y = p_lidar.y;
            pt.z = p_lidar.z;
            pt.intensity = inside ? 220.0f : 40.0f;
            pt.range_m = static_cast<float>(p_cam.norm());
            lidar.points.push_back(pt);
        }
    }
    return lidar;
}

}  // namespace

int main() {
    // Ground-truth arbitrary universal clamp mount transform (Camera <- LiDAR)
    const math::Vec3d gt_rot(3.0 * math::kDegToRad, -4.0 * math::kDegToRad, 2.0 * math::kDegToRad);
    const math::Vec3d gt_trans(-0.06, 0.14, 0.015);
    const math::SE3d T_cam_lidar_gt(math::SO3d::exp(gt_rot), gt_trans);
    const math::SE3d T_lidar_cam_gt = T_cam_lidar_gt.inverse();

    constexpr double kClockOffsetSec = 0.012;

    calibration::OnlineAutoCalibrator auto_calib;
    calibration::AutoCalibConfig cfg;
    cfg.min_motion_pairs = 12;
    cfg.min_vl_pairs = 3;
    cfg.convert_arkit_to_opencv = true;
    auto_calib.reset(cfg);

    // Simulate 30 Hz ARKit camera poses and 10 Hz LIO bundles over 4.0 seconds of multi-axis motion
    auto evalWorldLidar = [](double t) -> math::SE3d {
        math::Vec3d rot(
            0.35 * std::sin(1.4 * t),
            0.30 * std::cos(1.1 * t),
            0.28 * std::sin(0.8 * t + 0.5));
        math::Vec3d trans(
            0.20 * std::sin(0.9 * t),
            0.15 * std::cos(1.2 * t),
            0.08 * std::sin(1.7 * t));
        return math::SE3d(math::SO3d::exp(rot), trans);
    };

    bool ready_triggered = false;
    const double dt_cam = 1.0 / 30.0;
    const double dt_lidar = 0.1;
    double next_lidar_t = 0.1;

    for (int step = 0; step <= 120; ++step) {
        const double t_host = step * dt_cam;
        const double t_lidar_domain = t_host + kClockOffsetSec;

        const math::SE3d T_w_l = evalWorldLidar(t_lidar_domain);
        const math::SE3d T_w_c_cv = T_w_l * T_lidar_cam_gt;
        const math::SE3d T_w_c_arkit = openCvToArkitCameraPose(T_w_c_cv);
        auto_calib.pushCameraPose(t_host, T_w_c_arkit);

        if (t_lidar_domain >= next_lidar_t) {
            sync::SyncBundle bundle;
            const auto rgb = makeSyntheticRgbFrame(160, 120, next_lidar_t);
            bundle.lidar_frame = makeSyntheticLidarFrame(T_cam_lidar_gt, rgb, next_lidar_t);
            bundle.rgb_frame = rgb;
            bundle.estimated_cam_to_lidar_offset_sec = kClockOffsetSec;
            bundle.has_valid_imu_coverage = true;

            core::ImuMeasurement imu;
            imu.timestamp_sec = next_lidar_t;
            imu.gyro_rads = math::Vec3d(0.4, 0.3, 0.2);
            bundle.imu_measurements.push_back(imu);

            const math::SE3d T_w_l_at_scan = evalWorldLidar(next_lidar_t);
            if (auto_calib.pushLidarBundleAndPose(bundle, T_w_l_at_scan)) {
                ready_triggered = true;
                break;
            }
            next_lidar_t += dt_lidar;
        }
    }

    if (!ready_triggered) {
        auto st = auto_calib.getStatus();
        std::printf("[FAIL] OnlineAutoCalibrator never became ready (pairs=%zu, eig=%.4f, msg=%s)\n",
                    st.motion_pairs_collected, st.second_axis_excitation, st.status_message.c_str());
        return 1;
    }

    std::vector<calibration::HandEyeMotionPair> motions;
    std::vector<calibration::DirectVLCalibPair> vl_pairs;
    std::vector<std::pair<double, double>> imu_norms;
    std::vector<std::pair<double, double>> cam_norms;
    if (!auto_calib.beginSolve(motions, vl_pairs, imu_norms, cam_norms)) {
        std::printf("[FAIL] beginSolve returned false\n");
        return 1;
    }

    core::ExtrinsicCalibrationProfile solved;
    if (!auto_calib.completeSolve(motions, vl_pairs, imu_norms, cam_norms,
                                  "iPad13,18", "universal_clamp_auto", solved)) {
        std::printf("[FAIL] completeSolve failed (conf=%.3f, rot_res=%.3f)\n",
                    solved.confidence_score, solved.hand_eye_rot_residual_deg);
        return 1;
    }

    if (!auto_calib.isLocked() || !solved.verified) {
        std::printf("[FAIL] Expected LOCKED and verified profile\n");
        return 1;
    }

    const double rot_err_deg = (solved.T_cam_lidar.so3.inverse() * T_cam_lidar_gt.so3).log().norm() * math::kRadToDeg;
    const double trans_err_m = (solved.T_cam_lidar.t - T_cam_lidar_gt.t).norm();
    std::printf("[PASS] OnlineAutoCalibrator locked: rot_err=%.3f deg, trans_err=%.4f m, conf=%.3f\n",
                rot_err_deg, trans_err_m, solved.confidence_score);

    if (rot_err_deg > 1.0 || trans_err_m > 0.01) {
        std::printf("[FAIL] Solved extrinsics exceeded tolerance\n");
        return 1;
    }

    // Verify popup color transitions: GREEN -> YELLOW (featureless) -> GREEN (feature-rich)
    if (auto_calib.getStatus().popup_color != calibration::AutoCalibPopupColor::GREEN) {
        std::printf("[FAIL] Expected GREEN after initial lock\n");
        return 1;
    }

    {
        sync::SyncBundle blank_bundle;
        blank_bundle.lidar_frame = makeSyntheticLidarFrame(T_cam_lidar_gt, makeFeaturelessRgbFrame(160, 120, 5.0), 5.0);
        blank_bundle.rgb_frame = makeFeaturelessRgbFrame(160, 120, 5.0);
        blank_bundle.estimated_cam_to_lidar_offset_sec = kClockOffsetSec;
        auto_calib.pushLidarBundleAndPose(blank_bundle, evalWorldLidar(5.0));

        auto st = auto_calib.getStatus();
        if (st.popup_color != calibration::AutoCalibPopupColor::YELLOW || !st.has_initial_lock) {
            std::printf("[FAIL] Expected YELLOW fallback with initial lock preserved in featureless scene\n");
            return 1;
        }
    }

    // Also verify RED on initial featureless bundle
    {
        calibration::OnlineAutoCalibrator red_test;
        red_test.reset(cfg);
        sync::SyncBundle blank_bundle;
        blank_bundle.lidar_frame = makeSyntheticLidarFrame(T_cam_lidar_gt, makeFeaturelessRgbFrame(160, 120, 1.0), 1.0);
        blank_bundle.rgb_frame = makeFeaturelessRgbFrame(160, 120, 1.0);
        red_test.pushLidarBundleAndPose(blank_bundle, math::SE3d::Identity());
        auto st = red_test.getStatus();
        if (st.popup_color != calibration::AutoCalibPopupColor::RED || st.popup_title != "LOCKING FAILURE") {
            std::printf("[FAIL] Expected RED LOCKING FAILURE on initial featureless scene\n");
            return 1;
        }
    }
    return 0;
}
