#pragma once

#include "avscan/slam/ISlamEstimator.hpp"
#include "avscan/slam/VoxelHashMap.hpp"
#include <array>
#include <deque>
#include <vector>

namespace avscan {
namespace slam {

struct FastLivo2Config {
    double voxel_size_m{0.50};                // Matches av-scanar robosense.yaml voxel_size: 0.5
    double surf_downsample_voxel_m{0.08};     // Spatial voxel downsampling of incoming scan before IEKF
    double max_plane_corr_dist_m{0.25};
    int max_iekf_iterations{8};               // Matches av-scanar robosense.yaml max_iterations: 8
    int point_downsample_stride{2};
    double gyro_noise_sigma{0.10};            // Continuous-time gyro noise density (rad/s/sqrt(Hz))
    double accel_noise_sigma{0.316};          // Continuous-time accel noise density (m/s^2/sqrt(Hz)) -> cov 0.10
    double bias_gyro_rw_sigma{0.01};          // Gyro bias random walk -> cov 1e-4
    double bias_accel_rw_sigma{0.01};         // Accel bias random walk -> cov 1e-4
    double keyframe_translation_thresh_m{0.50};
    double keyframe_rotation_thresh_deg{22.0};
    double degeneracy_eigenvalue_thresh{8.0};
    bool enable_visual_fusion{false};         // Pure LIO default matching av-scanar robosense.yaml img_en: 0
    bool enable_rgb_colorization{true};       // SLERP-interpolated depth-buffered RGB projection
};

struct ImuPropagatedPose {
    double timestamp_sec{0.0};
    math::SE3d T_world_body{math::SE3d::Identity()};
    math::Vec3d velocity_world{0.0, 0.0, 0.0};
    math::Vec3d gyro_unbiased{0.0, 0.0, 0.0};
    math::Vec3d accel_world{0.0, 0.0, 0.0};
};

class FastLivo2Engine final : public ISlamEstimator {
public:
    explicit FastLivo2Engine(const FastLivo2Config& config = FastLivo2Config{});
    ~FastLivo2Engine() override = default;

    void setFactoryCalibration(const core::AiryFactoryCalibration& calib) override;
    void setExtrinsicCalibration(const core::ExtrinsicCalibrationProfile& profile) override;
    void setVisualFusionEnabled(bool enabled) override { config_.enable_visual_fusion = enabled; }

    // Processes a synchronized LiDAR + IMU (+ optional RGB) bundle.
    // Outputs:
    //   out_deskewed_body_frame: Motion-compensated points in sweep-end IMU BODY frame (T_world_body * p_body = p_world)
    //   out_world_frame:         Points transformed into world frame (+Z gravity-aligned)
    bool processBundle(const sync::SyncBundle& bundle,
                       core::LidarFrame& out_deskewed_body_frame,
                       core::LidarFrame& out_world_frame,
                       EstimatorMetrics& out_metrics) override;

    core::SlamState getCurrentState() const override { return state_; }
    void resetPose(const math::SE3d& new_T_world_body) override;
    void reset() override;
    std::string getLicenseIdentifier() const override {
        return "FAST-LIVO2 Native C++ Engine (Isolated Estimator Module - Commercial Authorization Gate Tracked)";
    }

    const VoxelHashMap& getLocalMap() const { return local_map_; }
    const std::vector<core::SlamState>& getTrajectory() const { return trajectory_; }

private:
    struct Mat18 {
        double m[18][18]{};
        static Mat18 Zero() { return Mat18{}; }
        static Mat18 Identity() {
            Mat18 r;
            for (int i = 0; i < 18; ++i) r.m[i][i] = 1.0;
            return r;
        }
        double& operator()(int r, int c) { return m[r][c]; }
        double operator()(int r, int c) const { return m[r][c]; }
    };

    bool initializeImuGravity(const std::vector<core::ImuMeasurement>& imus);

    void propagateImuAndDeskew(const sync::SyncBundle& bundle,
                               std::vector<ImuPropagatedPose>& out_imu_traj,
                               core::LidarFrame& out_deskewed_body);

    void runGeometricIekfUpdate(const std::vector<math::Vec3d>& feats_body,
                                EstimatorMetrics& io_metrics);

    void runDirectVisualUpdate(const core::RgbFrame& rgb_frame,
                               const core::LidarFrame& deskewed_body,
                               EstimatorMetrics& io_metrics);

    void colorizeAndExtractVisualPatches(const core::RgbFrame& rgb_frame,
                                         const std::vector<ImuPropagatedPose>& corrected_traj,
                                         core::LidarFrame& io_deskewed_body,
                                         core::LidarFrame& io_world_frame);

    math::SE3d interpolateBodyPoseAtTime(double target_time_sec,
                                         const std::vector<ImuPropagatedPose>& sweep_traj) const;

    static float sampleBilinearGray(const core::RgbFrame& img, double u, double v);
    static void sampleBilinearRgb(const core::RgbFrame& img, double u, double v,
                                  uint8_t& r, uint8_t& g, uint8_t& b);
    static bool invert18x18(const Mat18& A, Mat18& out_inv);
    static Mat18 mul18x18(const Mat18& A, const Mat18& B);
    static Mat18 mul18x18Transpose(const Mat18& A, const Mat18& B);

    FastLivo2Config config_;
    core::AiryFactoryCalibration factory_calib_{};
    core::ExtrinsicCalibrationProfile extrinsic_profile_{};

    core::SlamState state_{};
    // Full 18x18 coupled error-state covariance: [rot(0..2), pos(3..5), vel(6..8), bg(9..11), ba(12..14), grav(15..17)]
    Mat18 P_{};

    VoxelHashMap local_map_;
    std::vector<core::ImuMeasurement> init_imu_accum_;
    core::ImuMeasurement last_imu_{};
    bool has_last_imu_{false};
    double acc_scale_{1.0};

    std::vector<core::SlamState> trajectory_;
    std::deque<std::pair<double, math::SE3d>> recent_pose_history_;
    math::SE3d last_keyframe_pose_{math::SE3d::Identity()};
    bool has_first_keyframe_{false};
};

}  // namespace slam
}  // namespace avscan
