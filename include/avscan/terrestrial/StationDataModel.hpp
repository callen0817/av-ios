#pragma once

#include "avscan/core/Types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace avscan {
namespace terrestrial {

enum class RotationMode : uint8_t {
    MANUAL_INDEX = 0,    // Operator manually turns mount to detent / nominal mark
    PRESET_INTERVAL = 1, // Turntable runs programmed manufacturer preset stops
    AUTOMATIC_MOTOR = 2  // Software commands motor via verified API (T5)
};

enum class MotionState : uint8_t {
    STATIONARY = 0,      // Scanner is stable and motionless; safe to accumulate
    SETTLING = 1,        // Motion recently ceased; waiting for tripod vibrations to damp
    ROTATING = 2,        // Motor or operator is currently rotating scanner
    DISTURBED = 3        // Excessive shock, wind buffeting, or tripod bump detected
};

struct StopObservation {
    uint32_t stop_index{0};
    double nominal_angle_deg{0.0};
    double measured_angle_deg{0.0};
    bool has_measured_angle{false};
    
    double timestamp_start_sec{0.0};
    double timestamp_end_sec{0.0};
    
    std::string raw_lidar_relpath{};
    std::string rgb_image_relpath{};
    
    std::vector<core::LidarPointXYZIRT> stationary_points;
    math::SE3d T_station_lidar{math::SE3d::Identity()};
    
    double registration_residual_rmse_m{0.0};
    bool registration_converged{false};
};

struct TerrestrialStationConfig {
    std::string project_id{"PROJ_001"};
    std::string station_id{"st_001"};
    std::string station_name{"Station 01"};
    
    RotationMode rotation_mode{RotationMode::MANUAL_INDEX};
    double nominal_step_deg{60.0};       // Default 6-stop 360 degree scan (60 deg at 5 sec)
    size_t total_expected_stops{6};      // (360 / 60) = 6
    double accumulation_sec_per_stop{5.0};
    double settling_delay_sec{1.0};
    
    double max_motion_angular_vel_rads{0.05}; // IMU stillness threshold
    bool enable_overlap_refinement{true};
    bool enable_camera_capture{true};
};

struct TerrestrialStationMetadata {
    std::string schema_version{"1.0"};
    std::string project_id{};
    std::string station_id{};
    std::string station_name{};
    std::string created_utc{};
    
    std::string host_app{"AV Scan"};
    std::string host_device{"iPad/iPhone"};
    std::string sensor_serial{"08:48:57:09:82:89"};
    
    RotationMode rotation_mode{RotationMode::MANUAL_INDEX};
    double nominal_step_deg{60.0};
    size_t total_stops{0};
    size_t total_merged_points{0};
    
    double overall_rmse_m{0.0};
    bool is_closed_360{false};
    double azimuth_coverage_ratio{1.0};
    double elevation_coverage_ratio{0.85};
    std::string coverage_warning{};
    
    std::string pointcloud_ply_relpath{"pointcloud.ply"};
    std::string pointcloud_lod0_relpath{"cache/lod0.bin"};
    std::string station_e57_relpath{"station.e57"};
    std::string panorama_rgb_relpath{"pano/pano_rgb.jpg"};
    std::string coverage_mask_relpath{"pano/coverage_mask.png"};
};

} // namespace terrestrial
} // namespace avscan
