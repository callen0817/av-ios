#pragma once

#include "avscan/core/Types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace avscan {
namespace core {

enum class DeviceFormFactor {
    IPAD_10TH_GEN,       // iPad 10th Gen (A2696 / 10.9" USB-C)
    IPAD_AIR_4TH_GEN,    // iPad Air 4th Gen (10.9" USB-C)
    IPAD_AIR_5TH_GEN,    // iPad Air 5th Gen (M1 / 10.9" USB-C)
    IPAD_AIR_11_M2,      // iPad Air 11" (M2 / USB-C)
    IPAD_AIR_13_M2,      // iPad Air 13" (M2 / USB-C)
    IPAD_PRO_11_M4,      // iPad Pro 11" (M4 / USB-C LiDAR)
    IPAD_PRO_13_M4,      // iPad Pro 13" (M4 / USB-C LiDAR)
    IPAD_MINI_6TH_GEN,   // iPad mini 6th Gen (8.3" USB-C)
    IPHONE_15_BASE,      // iPhone 15 / 15 Plus (USB-C)
    IPHONE_15_PRO,       // iPhone 15 Pro (USB-C LiDAR)
    IPHONE_15_PRO_MAX,   // iPhone 15 Pro Max (USB-C LiDAR)
    IPHONE_16_BASE,      // iPhone 16 / 16 Plus (USB-C)
    IPHONE_16_PRO,       // iPhone 16 Pro (USB-C LiDAR)
    IPHONE_16_PRO_MAX,   // iPhone 16 Pro Max (USB-C LiDAR)
    CUSTOM_MOUNT
};

struct DeviceMountProfile {
    std::string profile_id{"ipad_10th_gen_airy_rigid"};
    std::string display_name{"iPad 10th Gen + RoboSense Airy"};
    std::string device_model{"iPad13,18"}; // iPad 10th Gen A2696
    DeviceFormFactor form_factor{DeviceFormFactor::IPAD_10TH_GEN};
    bool is_phone{false};
    bool has_native_lidar{false};

    // Physical dimensions & offsets
    double screen_diagonal_inches{10.9};
    double chassis_width_m{0.2486};
    double chassis_height_m{0.1795};
    double chassis_thickness_m{0.0070};

    // Camera placement relative to USB-C port center [x: right, y: up, z: out from screen]
    math::Vec3d camera_offset_from_usbc_m{-0.098, 0.082, 0.005};

    // Mechanical CAD Rigid Extrinsic: Camera Optical Frame <- RoboSense Airy LiDAR Base
    // T_cam_lidar transforms points from Airy LiDAR frame into Camera optical frame
    math::SE3d T_cam_lidar{math::SE3d::Identity()};

    // Fine temporal offset (sec): t_lidar = t_camera + camera_time_offset_sec
    double camera_time_offset_sec{0.008};

    // Default camera intrinsics for this device model
    CameraIntrinsics default_intrinsics{};

    // Factory / calibration status
    bool is_factory_calibrated{true};
    double calibration_confidence{0.92};
};

class DeviceProfileRegistry {
public:
    static DeviceProfileRegistry& instance();

    const std::vector<DeviceMountProfile>& allProfiles() const { return profiles_; }

    std::optional<DeviceMountProfile> getProfile(const std::string& profile_id) const;
    std::optional<DeviceMountProfile> getProfileForModel(const std::string& model_identifier) const;

    const DeviceMountProfile& activeProfile() const { return active_profile_; }
    bool setActiveProfile(const std::string& profile_id);

    // Creates an ExtrinsicCalibrationProfile from active device profile
    ExtrinsicCalibrationProfile activeExtrinsics() const;

private:
    DeviceProfileRegistry();
    std::vector<DeviceMountProfile> profiles_;
    DeviceMountProfile active_profile_;
};

}  // namespace core
}  // namespace avscan
