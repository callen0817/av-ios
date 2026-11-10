#pragma once

#include "avscan/core/Types.hpp"
#include <string>
#include <utility>
#include <vector>

namespace avscan {
namespace calibration {

struct HandEyeMotionPair {
    math::SE3d A_lidar_motion{math::SE3d::Identity()};  // T_L(k-1)^-1 * T_L(k)
    math::SE3d B_camera_motion{math::SE3d::Identity()}; // T_C(k-1)^-1 * T_C(k)
    double timestamp_sec{0.0};
};

struct DirectVLCalibPair {
    core::LidarFrame lidar_frame{};
    core::RgbFrame rgb_frame{};
};

class TargetlessCalibrator {
public:
    TargetlessCalibrator() = default;

    // Method 2 (Initialization / Cross-check): Motion-based Hand-Eye Calibration (A * X = X * B)
    // Solves T_L_C (X) and inverts to return T_cam_lidar, checking rotational excitation across >= 2 axes
    static bool solveHandEyeAXeqXB(const std::vector<HandEyeMotionPair>& motions,
                                   math::SE3d& out_T_cam_lidar,
                                   double& out_rot_residual_deg,
                                   double& out_trans_residual_m);

    // Evaluates DirectVLCalib Normalized Information Distance (NID) and edge-gradient alignment score
    // Lower NID (closer to 0) and higher edge correlation (closer to 1) indicate better alignment
    static double evaluateDirectVLObjective(const std::vector<DirectVLCalibPair>& pairs,
                                            const math::SE3d& candidate_T_cam_lidar,
                                            double* out_nid = nullptr,
                                            double* out_edge_corr = nullptr);

    // Method 1 (Primary): Direct Visual-LiDAR Calibration (DirectVLCalib) over SE(3)
    static bool optimizeDirectVLCalib(const std::vector<DirectVLCalibPair>& pairs,
                                      const math::SE3d& initial_T_cam_lidar,
                                      math::SE3d& out_refined_T_cam_lidar,
                                      double& out_nid_metric,
                                      double& out_confidence);

    // Jointly runs Hand-Eye initialization, temporal offset estimation, and DirectVLCalib refinement
    static bool runFullTargetlessCalibration(
        const std::vector<HandEyeMotionPair>& motion_pairs,
        const std::vector<DirectVLCalibPair>& visual_lidar_pairs,
        const std::vector< std::pair<double, double> >& imu_gyro_norms,
        const std::vector< std::pair<double, double> >& cam_rot_norms,
        const std::string& device_model,
        const std::string& mount_id,
        core::ExtrinsicCalibrationProfile& out_profile);

    // Persists and loads ExtrinsicCalibrationProfile to/from disk
    static bool saveProfileToFile(const core::ExtrinsicCalibrationProfile& profile, const std::string& file_path);
    static bool loadProfileFromFile(const std::string& file_path, core::ExtrinsicCalibrationProfile& out_profile);
};

}  // namespace calibration
}  // namespace avscan
