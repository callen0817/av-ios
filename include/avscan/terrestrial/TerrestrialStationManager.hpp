#pragma once

#include "StationDataModel.hpp"
#include "RotationGeometry.hpp"
#include "IRotationController.hpp"
#include "avscan/core/DeviceMountProfile.hpp"
#include <mutex>
#include <memory>
#include <vector>

namespace avscan {
namespace terrestrial {

enum class TerrestrialCaptureState : uint8_t {
    IDLE = 0,
    WAITING_FOR_STILLNESS = 1,
    ACCUMULATING_STOP = 2,
    STOP_COMPLETED = 3,
    STATION_COMPLETED = 4,
    STATION_ERROR = 5
};

class TerrestrialStationManager {
public:
    TerrestrialStationManager(const TerrestrialStationConfig& config,
                              std::shared_ptr<IRotationController> rotation_ctrl = nullptr);

    bool startNewStation(const std::string& project_dir,
                         const TerrestrialStationConfig& config,
                         const core::DeviceMountProfile& mount_profile);

    // Ingestion of live sensor streams
    void pushLidarFrame(const core::LidarFrame& frame);
    void pushImuMeasurement(const core::ImuMeasurement& imu);
    void pushCameraImage(const std::string& image_file_path);

    // Operator control triggers (Manual or Automated)
    bool beginCurrentStopAccumulation();
    bool finishCurrentStop();
    bool advanceToNextStop();
    bool repeatCurrentStop();
    bool cancelStation();
    bool finalizeAndSaveStation(std::string& out_error);

    // Constrained 1-DOF rotation registration refinement around the tripod Z-axis
    bool refineStopAnglesViaOverlap(double& out_mean_rmse_m);

    // Coverage analyzer across azimuth [0..360) and elevation [-90..+90]
    void computeCoverageMetrics(double& out_az_cov, double& out_el_cov, std::string& out_warning) const;

    // State queries
    TerrestrialCaptureState getCaptureState() const;
    uint32_t getCurrentStopIndex() const;
    double getCurrentAccumulationProgress() const; // 0.0 to 1.0
    double getImuMotionEnergy() const;
    bool isStationary() const;

    const std::vector<StopObservation>& getCompletedStops() const;
    std::vector<core::LidarPointXYZIRT> buildStationPointCloud() const;
    TerrestrialStationMetadata getStationMetadata() const;

private:
    bool finishCurrentStopLocked();
    bool writeStationArtifactsLocked(std::string& out_error);

    TerrestrialStationConfig config_;
    core::DeviceMountProfile mount_profile_;
    TripodAxisCalibration axis_calib_;
    std::shared_ptr<IRotationController> rotation_ctrl_;

    std::string project_dir_;
    std::string station_dir_;

    mutable std::mutex mutex_;
    TerrestrialCaptureState state_{TerrestrialCaptureState::IDLE};
    uint32_t current_stop_idx_{0};
    double current_nominal_angle_deg_{0.0};
    double stop_start_time_sec_{0.0};
    double latest_frame_end_sec_{0.0};

    // IMU stillness detection (Gyro + Accel sliding variance)
    double latest_gyro_norm_{0.0};
    double latest_accel_dev_{0.0};
    double stillness_duration_sec_{0.0};
    double last_imu_time_sec_{0.0};
    bool has_received_imu_{false};

    // Temporary accumulation buffer for active stop
    std::vector<core::LidarPointXYZIRT> current_stop_points_;
    std::string current_stop_image_path_;

    // Completed stops
    std::vector<StopObservation> stops_;
    double refined_overall_rmse_m_{0.0};
};

} // namespace terrestrial
} // namespace avscan
