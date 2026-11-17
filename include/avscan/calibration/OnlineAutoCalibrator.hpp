#pragma once

#include "avscan/calibration/TargetlessCalibrator.hpp"
#include "avscan/core/Types.hpp"
#include "avscan/sync/TimeSynchronizer.hpp"
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace avscan {
namespace calibration {

enum class AutoCalibState : uint8_t {
    IDLE = 0,
    INSUFFICIENT_FEATURES = 1,   // Red:    LOCKING FAILURE - insufficient environment for initial lock
    COLLECTING_MOTION = 2,       // White:  LOCKING - sufficient environment, excite axes
    SOLVING = 3,                 // White:  LOCKING - solving initial extrinsics
    LOCKED_GREEN = 4,            // Green:  LOCKED - sufficient features, lock confirmed
    LOCKED_YELLOW_FALLBACK = 5   // Yellow: LOCKED - reverting to initial lock (insufficient features)
};

enum class AutoCalibPopupColor : uint8_t {
    NONE = 0,
    WHITE = 1,
    RED = 2,
    YELLOW = 3,
    GREEN = 4
};

struct AutoCalibConfig {
    size_t min_motion_pairs{12};
    size_t max_motion_pairs{40};
    size_t min_vl_pairs{3};
    size_t max_vl_pairs{6};
    size_t max_points_per_vl_frame{2000};
    double min_step_rot_rad{0.035};       // ~2.0 deg between Hand-Eye keyframes
    double min_step_trans_m{0.03};        // 3 cm fallback step
    double min_multi_axis_eig{0.05};      // Matches TargetlessCalibrator::solveHandEyeAXeqXB
    double min_vl_interval_sec{0.35};     // Spread DirectVLCalib keyframes over time
    double continuous_refine_interval_sec{2.0};
    double min_edge_pixel_ratio{0.035};   // Minimum fraction of high-gradient pixels for feature-rich check
    size_t min_lidar_points{150};
    bool convert_arkit_to_opencv{true};   // ARKit camera is +X right, +Y up, -Z forward
};

struct AutoCalibStatus {
    AutoCalibState state{AutoCalibState::IDLE};
    AutoCalibPopupColor popup_color{AutoCalibPopupColor::NONE};
    bool has_initial_lock{false};
    bool environment_feature_rich{false};
    size_t motion_pairs_collected{0};
    size_t motion_pairs_required{12};
    size_t vl_pairs_collected{0};
    double second_axis_excitation{0.0};
    double elapsed_sec{0.0};
    double confidence_score{0.0};
    std::string popup_title{"IDLE"};
    std::string status_message{"Idle"};
};

// Orchestrates automatic targetless Hand-Eye + DirectVLCalib calibration at scan startup
// and non-stalling continuous lock monitoring during the scan.
class OnlineAutoCalibrator {
public:
    OnlineAutoCalibrator() = default;

    void reset(const AutoCalibConfig& config = AutoCalibConfig{});

    // Ingest camera pose from ARKit / VIO (in host monotonic clock domain).
    void pushCameraPose(double host_timestamp_sec, const math::SE3d& T_world_cam);

    // Ingest deskewed LiDAR bundle + current LIO pose T_world_lidar (in LiDAR clock domain).
    // Before initial lock: returns true when enough multi-axis data is ready to solve.
    // After initial lock: monitors feature richness and returns true periodically when
    // enough textured keyframes are buffered for continuous DirectVLCalib verification.
    bool pushLidarBundleAndPose(const sync::SyncBundle& bundle, const math::SE3d& T_world_lidar);

    // Snapshots collected data for initial solve (Hand-Eye + DirectVLCalib).
    bool beginSolve(std::vector<HandEyeMotionPair>& out_motions,
                    std::vector<DirectVLCalibPair>& out_vl_pairs,
                    std::vector<std::pair<double, double>>& out_imu_norms,
                    std::vector<std::pair<double, double>>& out_cam_norms);

    // Completes initial solve and stores initial_locked_profile_ on success.
    bool completeSolve(const std::vector<HandEyeMotionPair>& motions,
                       const std::vector<DirectVLCalibPair>& vl_pairs,
                       const std::vector<std::pair<double, double>>& imu_norms,
                       const std::vector<std::pair<double, double>>& cam_norms,
                       const std::string& device_model,
                       const std::string& mount_id,
                       core::ExtrinsicCalibrationProfile& out_profile);

    // Snapshots keyframes for continuous DirectVLCalib check after initial lock.
    bool beginContinuousRefine(std::vector<DirectVLCalibPair>& out_vl_pairs,
                               core::ExtrinsicCalibrationProfile& out_initial_profile);

    // Completes continuous DirectVLCalib check. On low-feature rejection, reverts to initial_locked_profile_
    // with LOCKED_YELLOW_FALLBACK so colorization never stalls.
    bool completeContinuousRefine(const std::vector<DirectVLCalibPair>& vl_pairs,
                                  core::ExtrinsicCalibrationProfile& out_profile_to_apply);

    AutoCalibStatus getStatus() const;
    bool isLocked() const;

    // Evaluates whether a SyncBundle has sufficient visual gradient edges and LiDAR points for locking.
    static bool evaluateFeatureRichness(const sync::SyncBundle& bundle,
                                        double min_edge_ratio = 0.035,
                                        size_t min_lidar_points = 150);

    // Converts ARKit camera pose (+X right, +Y up, -Z forward) to OpenCV optical frame (+X right, +Y down, +Z forward).
    static math::SE3d arkitToOpenCvCameraPose(const math::SE3d& T_world_cam_arkit);

private:
    bool interpolateCameraPoseLocked(double lidar_time_sec,
                                     double cam_to_lidar_offset_sec,
                                     math::SE3d& out_T_world_cam) const;
    static core::LidarFrame downsampleLidarFrame(const core::LidarFrame& frame, size_t max_points);

    mutable std::mutex mutex_;
    AutoCalibConfig config_{};
    AutoCalibState state_{AutoCalibState::IDLE};

    struct StampedPose {
        double timestamp_sec{0.0};
        math::SE3d pose{math::SE3d::Identity()};
    };

    std::deque<StampedPose> cam_poses_;
    bool has_prev_key_pose_{false};
    bool has_initial_lock_{false};
    bool env_feature_rich_{false};
    bool continuous_refine_in_flight_{false};
    double start_time_sec_{-1.0};
    double last_lidar_time_sec_{-1.0};
    double last_vl_capture_time_sec_{-1.0};
    double last_continuous_refine_time_sec_{-1.0};
    math::SE3d prev_T_world_lidar_{math::SE3d::Identity()};
    math::SE3d prev_T_world_cam_{math::SE3d::Identity()};

    math::Mat3d axis_scatter_{math::Mat3d::Zero()};
    double second_axis_eig_{0.0};
    double confidence_score_{0.0};

    core::ExtrinsicCalibrationProfile initial_locked_profile_{};
    std::vector<HandEyeMotionPair> motion_pairs_;
    std::vector<DirectVLCalibPair> vl_pairs_;
    std::vector<DirectVLCalibPair> continuous_vl_pairs_;
    std::vector<std::pair<double, double>> imu_gyro_norms_;
    std::vector<std::pair<double, double>> cam_rot_norms_;
};

}  // namespace calibration
}  // namespace avscan
