#pragma once

#include "avscan/math/SophusLie.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace avscan {
namespace core {

struct LidarPointXYZIRT {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float intensity{0.0f};      // Calibrated reflectivity [0, 255]
    uint16_t ring{0};           // Laser channel index [0, 95] for 96-beam Airy
    double timestamp_sec{0.0};  // Exact per-point acquisition time (seconds)
    float range_m{0.0f};        // Raw spherical range (meters)
    float azimuth_deg{0.0f};    // Horizontal firing azimuth (degrees)
    float elevation_deg{0.0f};  // Vertical firing elevation (degrees)
    uint8_t status_flags{0};    // Upper 2-bit status/confidence flags from MSOP channel
    uint8_t r{200}, g{200}, b{200};
    bool colorized{false};
};

struct LidarFrame {
    uint64_t frame_id{0};
    double timestamp_start_sec{0.0};
    double timestamp_end_sec{0.0};
    std::vector<LidarPointXYZIRT> points;
};

struct ImuMeasurement {
    double timestamp_sec{0.0};
    math::Vec3d accel_mps2{0.0, 0.0, 0.0};  // Linear acceleration (m/s^2)
    math::Vec3d gyro_rads{0.0, 0.0, 0.0};   // Angular velocity (rad/s)
    float temperature_c{25.0f};
    uint32_t sequence_id{0};
    uint8_t status_flags{0};
};

struct CameraIntrinsics {
    int width{1920};
    int height{1440};
    double fx{1420.0};
    double fy{1420.0};
    double cx{960.0};
    double cy{720.0};
    // Brown-Conrady distortion coefficients [k1, k2, p1, p2, k3]
    std::array<double, 5> dist_coeffs{0.0, 0.0, 0.0, 0.0, 0.0};

    math::Vec2d project(const math::Vec3d& p_cam, bool apply_distortion = true) const {
        if (p_cam.z <= 1e-4) return {-1.0, -1.0};
        double xn = p_cam.x / p_cam.z;
        double yn = p_cam.y / p_cam.z;
        if (apply_distortion) {
            double r2 = xn * xn + yn * yn;
            double r4 = r2 * r2;
            double r6 = r4 * r2;
            double radial = 1.0 + dist_coeffs[0] * r2 + dist_coeffs[1] * r4 + dist_coeffs[4] * r6;
            double dx = 2.0 * dist_coeffs[2] * xn * yn + dist_coeffs[3] * (r2 + 2.0 * xn * xn);
            double dy = dist_coeffs[2] * (r2 + 2.0 * yn * yn) + 2.0 * dist_coeffs[3] * xn * yn;
            xn = xn * radial + dx;
            yn = yn * radial + dy;
        }
        return {fx * xn + cx, fy * yn + cy};
    }

    bool isInFieldOfView(const math::Vec3d& p_cam, double margin_px = 4.0) const {
        if (p_cam.z <= 0.1) return false;
        math::Vec2d uv = project(p_cam, true);
        return (uv.x >= margin_px && uv.x < (width - margin_px) &&
                uv.y >= margin_px && uv.y < (height - margin_px));
    }
};

struct RgbFrame {
    uint64_t frame_id{0};
    double timestamp_sec{0.0};
    double exposure_duration_sec{0.008};
    float iso{100.0f};
    CameraIntrinsics intrinsics{};
    std::vector<uint8_t> rgb_bytes;   // Row-major RGB24 (width * height * 3)
    std::vector<uint8_t> gray_bytes;  // Row-major Grayscale8 (width * height)
};

struct AiryFactoryCalibration {
    bool valid{false};
    math::SE3d T_imu_lidar{math::SE3d::Identity()};  // P_imu = R * P_lidar + t
    std::array<double, 96> vertical_angles_deg{};
    std::array<double, 96> horizontal_angles_deg{};
    std::string mainboard_firmware{};
    std::string baseboard_firmware{};
    std::string motor_firmware{};
    std::string sensor_ip{"192.168.1.200"};
    std::string dest_ip{"192.168.1.102"};
    std::string mac_address{"00:00:00:00:00:00"};
    uint16_t msop_port{6699};
    uint16_t difop_port{7788};
    uint16_t imu_port{6688};
    uint8_t return_mode{1};
};

struct ExtrinsicCalibrationProfile {
    std::string profile_id{"default_iphone_airy"};
    std::string device_model{"iPhone_USB_C"};
    std::string mount_id{"rigid_mount_v1"};
    math::SE3d T_cam_lidar{math::SE3d::Identity()};  // Transforms LiDAR points into Camera frame
    double camera_time_offset_sec{0.0};              // t_lidar = t_camera + camera_time_offset_sec
    double confidence_score{0.0};                    // [0.0, 1.0], requires >= 0.65 for visual fusion
    double nid_metric{1.0};
    double hand_eye_rot_residual_deg{0.0};
    bool verified{false};
};

enum class TrackingStatus : uint8_t {
    UNINITIALIZED = 0,
    INITIALIZING_IMU = 1,
    TRACKING_LIO = 2,
    TRACKING_LIVO = 3,
    DEGENERATE_GEOMETRY = 4,
    LOST_RELOCALIZING = 5
};

enum class OperatingMode : uint8_t {
    TRACKING_REALTIME = 0,
    MAPPING_DENSE = 1,
    FINAL_REFINEMENT = 2
};

struct SlamState {
    double timestamp_sec{0.0};
    math::SE3d T_world_body{math::SE3d::Identity()};
    math::Vec3d velocity_mps{0.0, 0.0, 0.0};
    math::Vec3d bias_gyro_rads{0.0, 0.0, 0.0};
    math::Vec3d bias_accel_mps2{0.0, 0.0, 0.0};
    math::Vec3d gravity_mps2{0.0, 0.0, -9.81};
    TrackingStatus status{TrackingStatus::UNINITIALIZED};
    double position_uncertainty_m{0.0};
    double orientation_uncertainty_deg{0.0};
    double condition_number{1.0};
};

struct KeyframeSubmap {
    uint64_t keyframe_id{0};
    double timestamp_sec{0.0};
    math::SE3d T_world_keyframe{math::SE3d::Identity()};
    std::vector<LidarPointXYZIRT> local_points;  // Stored in keyframe-local frame for global map correction
    bool archived_to_disk{false};
    std::string disk_chunk_path{};
};

}  // namespace core
}  // namespace avscan
