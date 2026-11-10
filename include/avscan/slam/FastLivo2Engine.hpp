#pragma once

#include "avscan/slam/ISlamEstimator.hpp"
#include "avscan/slam/VoxelHashMap.hpp"
#include <array>
#include <vector>

namespace avscan {
namespace slam {

struct FastLivo2Config {
    double voxel_size_m{0.25};
    double max_plane_corr_dist_m{0.15};
    int max_iekf_iterations{4};
    int point_downsample_stride{2};
    double gyro_noise_sigma{0.005};
    double accel_noise_sigma{0.05};
    double bias_gyro_rw_sigma{0.0005};
    double bias_accel_rw_sigma{0.002};
    double keyframe_translation_thresh_m{0.35};
    double keyframe_rotation_thresh_deg{12.0};
    double degeneracy_eigenvalue_thresh{8.0};
    bool enable_visual_fusion{true};
    bool enable_rgb_colorization{true};
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
    bool initializeImuGravity(const std::vector<core::ImuMeasurement>& imus);

    void propagateImuAndDeskew(const sync::SyncBundle& bundle,
                               std::vector<ImuPropagatedPose>& out_imu_traj,
                               core::LidarFrame& out_deskewed_body);

    void runGeometricIekfUpdate(const core::LidarFrame& deskewed_body,
                                EstimatorMetrics& io_metrics);

    void runDirectVisualUpdate(const core::RgbFrame& rgb_frame,
                               const core::LidarFrame& deskewed_body,
                               EstimatorMetrics& io_metrics);

    void colorizeAndExtractVisualPatches(const core::RgbFrame& rgb_frame,
                                         core::LidarFrame& io_deskewed_body,
                                         core::LidarFrame& io_world_frame);

    static float sampleBilinearGray(const core::RgbFrame& img, double u, double v);
    static void sampleBilinearRgb(const core::RgbFrame& img, double u, double v,
                                  uint8_t& r, uint8_t& g, uint8_t& b);

    FastLivo2Config config_;
    core::AiryFactoryCalibration factory_calib_{};
    core::ExtrinsicCalibrationProfile extrinsic_profile_{};

    core::SlamState state_{};
    // 18x18 diagonal/block covariance representation for [rot(3), pos(3), vel(3), bg(3), ba(3), grav(3)]
    std::array<double, 18> P_diag_{};

    VoxelHashMap local_map_;
    std::vector<core::ImuMeasurement> init_imu_accum_;
    std::vector<core::SlamState> trajectory_;
    math::SE3d last_keyframe_pose_{math::SE3d::Identity()};
    bool has_first_keyframe_{false};
};

}  // namespace slam
}  // namespace avscan
