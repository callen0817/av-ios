#include "avscan/calibration/TargetlessCalibrator.hpp"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace avscan;

bool run_test_m7_calibration() {
    // 1. Ground-truth Camera-to-LiDAR extrinsic transform T_cam_lidar
    const math::Vec3d gt_rot_vec(0.035, -0.042, 0.028); // ~2.0, -2.4, 1.6 deg
    const math::Vec3d gt_trans(0.055, -0.082, 0.030);
    const math::SE3d T_cam_lidar_gt(math::SO3d::exp(gt_rot_vec), gt_trans);
    const math::SE3d T_lidar_cam_gt = T_cam_lidar_gt.inverse(); // X in A * X = X * B

    // 2. Test degenerate single-axis motion rejection in Hand-Eye AX = XB
    {
        std::vector<calibration::HandEyeMotionPair> single_axis_motions;
        for (int i = 0; i < 8; ++i) {
            math::SE3d A(math::SO3d::exp(math::Vec3d(0.0, 0.0, 0.15 * (i + 1))), math::Vec3d(0.1, 0.0, 0.0));
            math::SE3d B = T_lidar_cam_gt.inverse() * A * T_lidar_cam_gt;
            calibration::HandEyeMotionPair pair;
            pair.A_lidar_motion = A;
            pair.B_camera_motion = B;
            pair.timestamp_sec = 1.0 * i;
            single_axis_motions.push_back(pair);
        }
        math::SE3d est_T;
        double rot_res = 0.0, trans_res = 0.0;
        bool solved_degenerate = calibration::TargetlessCalibrator::solveHandEyeAXeqXB(
            single_axis_motions, est_T, rot_res, trans_res);
        if (solved_degenerate) {
            std::printf("  [M7 FAIL] solveHandEyeAXeqXB failed to reject degenerate single-axis motion\n");
            return false;
        }
    }

    // 3. Test multi-axis Hand-Eye AX = XB solver accuracy
    std::vector<calibration::HandEyeMotionPair> multi_axis_motions;
    for (int i = 0; i < 16; ++i) {
        const double t = 0.4 * i;
        math::Vec3d omega(0.25 * std::sin(t), 0.28 * std::cos(1.3 * t), 0.22 * std::sin(0.7 * t + 0.4));
        math::Vec3d step_t(0.12 * std::cos(t), 0.09 * std::sin(t), 0.05 * std::sin(2.0 * t));
        math::SE3d A(math::SO3d::exp(omega), step_t);
        math::SE3d B = T_lidar_cam_gt.inverse() * A * T_lidar_cam_gt;
        calibration::HandEyeMotionPair pair;
        pair.A_lidar_motion = A;
        pair.B_camera_motion = B;
        pair.timestamp_sec = t;
        multi_axis_motions.push_back(pair);
    }

    math::SE3d hand_eye_T_cam_lidar;
    double rot_res_deg = 0.0;
    double trans_res_m = 0.0;
    if (!calibration::TargetlessCalibrator::solveHandEyeAXeqXB(
            multi_axis_motions, hand_eye_T_cam_lidar, rot_res_deg, trans_res_m)) {
        std::printf("  [M7 FAIL] solveHandEyeAXeqXB failed on well-conditioned multi-axis motions\n");
        return false;
    }

    const double he_rot_err_deg = (hand_eye_T_cam_lidar.so3().inverse() * T_cam_lidar_gt.so3()).log().norm() * 180.0 / math::kPi;
    const double he_trans_err_m = (hand_eye_T_cam_lidar.translation() - T_cam_lidar_gt.translation()).norm();
    if (he_rot_err_deg > 0.25 || he_trans_err_m > 0.005) {
        std::printf("  [M7 FAIL] Hand-Eye error too high: rot=%.4f deg, trans=%.5f m\n", he_rot_err_deg, he_trans_err_m);
        return false;
    }

    // 4. Test Direct Visual-LiDAR Calibration (DirectVLCalib) refinement from perturbed initial guess
    std::vector<calibration::DirectVLCalibPair> vl_pairs;
    for (int f = 0; f < 3; ++f) {
        calibration::DirectVLCalibPair vlp;
        vlp.rgb_frame.timestamp_sec = 10.0 + f;
        vlp.rgb_frame.width = 160;
        vlp.rgb_frame.height = 120;
        vlp.rgb_frame.intrinsics.width = 160;
        vlp.rgb_frame.intrinsics.height = 120;
        vlp.rgb_frame.intrinsics.fx = 130.0;
        vlp.rgb_frame.intrinsics.fy = 130.0;
        vlp.rgb_frame.intrinsics.cx = 80.0;
        vlp.rgb_frame.intrinsics.cy = 60.0;
        vlp.rgb_frame.gray_pixels.assign(160 * 120, 40);
        vlp.rgb_frame.rgb_pixels.assign(160 * 120 * 3, 40);

        // Render synthetic high-contrast rectangular target in camera frame and corresponding 3D LiDAR points
        for (int v = 30; v < 90; ++v) {
            for (int u = 45; u < 115; ++u) {
                vlp.rgb_frame.gray_pixels[v * 160 + u] = 220;
            }
        }

        for (double y_c = -0.8; y_c <= 0.8; y_c += 0.05) {
            for (double x_c = -1.1; x_c <= 1.1; x_c += 0.05) {
                const double z_c = 3.0 + 0.1 * f;
                const math::Vec3d p_cam(x_c, y_c, z_c);
                const double u = vlp.rgb_frame.intrinsics.fx * (x_c / z_c) + vlp.rgb_frame.intrinsics.cx;
                const double v = vlp.rgb_frame.intrinsics.fy * (y_c / z_c) + vlp.rgb_frame.intrinsics.cy;
                const bool inside_rect = (u >= 45.0 && u < 115.0 && v >= 30.0 && v < 90.0);

                // Transform into LiDAR frame using inverse of ground-truth T_cam_lidar
                const math::Vec3d p_lidar = T_lidar_cam_gt. transform(p_cam);
                core::LidarPointXYZIRT pt;
                pt.x = static_cast<float>(p_lidar.x());
                pt.y = static_cast<float>(p_lidar.y());
                pt.z = static_cast<float>(p_lidar.z());
                pt.intensity = inside_rect ? 220.0f : 40.0f;
                vlp.lidar_frame.points.push_back(pt);
            }
        }
        vl_pairs.push_back(vlp);
    }

    // Perturb initial guess by ~1.5 degrees and compare objective
    const math::SE3d perturbed_guess = T_cam_lidar_gt * math::SE3d(math::SO3d::exp(math::Vec3d(0.02, -0.02, 0.015)), math::Vec3d(0.01, -0.01, 0.005));
    const double initial_cost = calibration::TargetlessCalibrator::evaluateDirectVLObjective(vl_pairs, perturbed_guess);
    const double gt_cost = calibration::TargetlessCalibrator::evaluateDirectVLObjective(vl_pairs, T_cam_lidar_gt);
    if (gt_cost >= initial_cost) {
        std::printf("  [M7 FAIL] DirectVLCalib objective did not favor ground-truth alignment over perturbation\n");
        return false;
    }

    // 5. Run full joint calibration and profile persistence round-trip
    std::vector< std::pair<double, double> > imu_norms, cam_norms;
    for (int i = 0; i < 200; ++i) {
        double t = i * 0.01;
        double signal = std::sin(3.0 * t) + 0.5 * std::cos(7.0 * t);
        imu_norms.push_back({t, std::abs(signal)});
        cam_norms.push_back({t - 0.01, std::abs(signal)});
    }

    core::ExtrinsicCalibrationProfile profile{};
    if (!calibration::TargetlessCalibrator::runFullTargetlessCalibration(
            multi_axis_motions, vl_pairs, imu_norms, cam_norms, "iPad_A2696", "Mount_Rev1", profile)) {
        std::printf("  [M7 FAIL] runFullTargetlessCalibration returned false\n");
        return false;
    }

    const std::string calib_file = "test_calib_profile.cfg";
    if (!calibration::TargetlessCalibrator::saveProfileToFile(profile, calib_file)) {
        std::printf("  [M7 FAIL] saveProfileToFile failed\n");
        return false;
    }

    core::ExtrinsicCalibrationProfile loaded_profile{};
    if (!calibration::TargetlessCalibrator::loadProfileFromFile(calib_file, loaded_profile)) {
        std::printf("  [M7 FAIL] loadProfileFromFile failed\n");
        return false;
    }
    std::filesystem::remove(calib_file);

    if (loaded_profile.device_model != "iPad_A2696" ||
        (loaded_profile.T_cam_lidar.translation() - profile.T_cam_lidar.translation()).norm() > 1e-6) {
        std::printf("  [M7 FAIL] Profile persistence round-trip mismatch\n");
        return false;
    }

    return true;
}
