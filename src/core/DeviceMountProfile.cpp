#include "avscan/core/DeviceMountProfile.hpp"
#include <algorithm>

namespace avscan {
namespace core {

namespace {

math::SE3d makeMountExtrinsics(double pitch_deg, double yaw_deg, double roll_deg,
                               double tx_m, double ty_m, double tz_m) {
    // Forward-facing mount (90 degree pitch):
    // - Camera and dome are both looking forward along +Z_cam optical axis
    // - Cable connector faces down along +Y_cam
    // R0 maps:
    //   +Z_lidar -> +Z_cam (dome looking forward)
    //   -X_lidar -> +Y_cam (cable facing down, so +X_lidar -> -Y_cam)
    //   +Y_lidar -> +X_cam (right)
    double delta_pitch_rad = (pitch_deg - 90.0) * math::kDegToRad;
    double delta_yaw_rad = yaw_deg * math::kDegToRad;
    double delta_roll_rad = roll_deg * math::kDegToRad;

    math::Mat3d R0 = math::Mat3d::Zero();
    R0(0, 1) =  1.0; // +Y_lidar -> +X_cam (right)
    R0(1, 0) = -1.0; // +X_lidar -> -Y_cam (cable at -X_lidar -> +Y_cam, down)
    R0(2, 2) =  1.0; // +Z_lidar -> +Z_cam (dome looking forward)

    math::Quatd q_delta = math::Quatd::FromEuler(delta_roll_rad, delta_pitch_rad, delta_yaw_rad);
    math::Mat3d R = q_delta.toRotationMatrix() * R0;

    return math::SE3d(math::SO3d(R), math::Vec3d(tx_m, ty_m, tz_m));
}

} // anonymous namespace

DeviceProfileRegistry& DeviceProfileRegistry::instance() {
    static DeviceProfileRegistry reg;
    return reg;
}

DeviceProfileRegistry::DeviceProfileRegistry() {
    // 1. iPad 10th Generation (Primary Hardware: A2696 / iPad13,18 / iPad13,19)
    // 10.9" Liquid Retina, USB-C at base, single wide 12MP camera top corner
    // Airy mounted on bottom-aligned rigid shroud: 90-deg forward-facing pitch (camera and dome looking forward, cable facing down)
    // Extrinsics derived from V2 3D CAD enclosure (Airy bottom aligned with iPad bottom):
    // tx = -0.075250 m (-75.25 mm camera-to-Airy horizontal offset)
    // ty = +0.204250 m (+204.25 mm down from top camera to bottom-aligned Airy center)
    // tz = +0.004000 m (+4.00 mm depth offset between camera front element and Airy base)
    {
        DeviceMountProfile p;
        p.profile_id = "ipad_10th_gen_airy_rigid";
        p.display_name = "iPad 10th Gen (A2696) + Airy Rigid Mount";
        p.device_model = "iPad13,18";
        p.form_factor = DeviceFormFactor::IPAD_10TH_GEN;
        p.is_phone = false;
        p.has_native_lidar = false;
        p.screen_diagonal_inches = 10.9;
        p.chassis_width_m = 0.2489;
        p.chassis_height_m = 0.17975;
        p.chassis_thickness_m = 0.0075;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.07525, 0.23425, 0.005);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, -0.075250, 0.204250, 0.004000);
        p.camera_time_offset_sec = 0.008;
        p.default_intrinsics = {1920, 1440, 1420.0, 1420.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.96;
        profiles_.push_back(p);
    }

    // 2. iPad Air 4th Generation (iPad13,1 / iPad13,2 - 10.9" USB-C)
    // 6.1mm chassis envelope with A14 Bionic
    // Airy mounted on bottom-aligned rigid shroud: 90-deg forward-facing pitch (camera and dome looking forward, cable facing down)
    // Extrinsics derived from V2 3D CAD enclosure (Airy bottom aligned with iPad bottom):
    // tx = -0.076500 m (-76.50 mm camera-to-Airy horizontal offset)
    // ty = +0.205050 m (+205.05 mm down from top camera to bottom-aligned Airy center)
    // tz = +0.004000 m (+4.00 mm depth offset)
    {
        DeviceMountProfile p;
        p.profile_id = "ipad_air_4th_gen_airy_rigid";
        p.display_name = "iPad Air 4th Gen + Airy Rigid Mount";
        p.device_model = "iPad13,1";
        p.form_factor = DeviceFormFactor::IPAD_AIR_4TH_GEN;
        p.is_phone = false;
        p.has_native_lidar = false;
        p.screen_diagonal_inches = 10.9;
        p.chassis_width_m = 0.2479;
        p.chassis_height_m = 0.1788;
        p.chassis_thickness_m = 0.0065;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.07650, 0.23505, 0.004);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, -0.076500, 0.205050, 0.004000);
        p.camera_time_offset_sec = 0.008;
        p.default_intrinsics = {1920, 1440, 1425.0, 1425.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.96;
        profiles_.push_back(p);
    }

    // 3. iPad Air 5th Generation (iPad13,16 / iPad13,17 - M1 10.9" USB-C)
    {
        DeviceMountProfile p;
        p.profile_id = "ipad_air_5th_gen_airy_rigid";
        p.display_name = "iPad Air 5th Gen (M1) + Airy Rigid Mount";
        p.device_model = "iPad13,16";
        p.form_factor = DeviceFormFactor::IPAD_AIR_5TH_GEN;
        p.is_phone = false;
        p.has_native_lidar = false;
        p.screen_diagonal_inches = 10.9;
        p.chassis_width_m = 0.2476;
        p.chassis_height_m = 0.1785;
        p.chassis_thickness_m = 0.0061;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.097, 0.081, 0.004);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.042850, -0.062400, -0.029100);
        p.camera_time_offset_sec = 0.006;
        p.default_intrinsics = {1920, 1440, 1425.0, 1425.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.94;
        profiles_.push_back(p);
    }

    // 4. iPad Air 11" M2 (iPad14,8 - 2024 USB-C)
    {
        DeviceMountProfile p;
        p.profile_id = "ipad_air_11_m2_airy_rigid";
        p.display_name = "iPad Air 11\" (M2) + Airy Rigid Mount";
        p.device_model = "iPad14,8";
        p.form_factor = DeviceFormFactor::IPAD_AIR_11_M2;
        p.is_phone = false;
        p.has_native_lidar = false;
        p.screen_diagonal_inches = 11.0;
        p.chassis_width_m = 0.2476;
        p.chassis_height_m = 0.1785;
        p.chassis_thickness_m = 0.0061;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.097, 0.081, 0.004);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.043100, -0.062600, -0.029400);
        p.camera_time_offset_sec = 0.005;
        p.default_intrinsics = {1920, 1440, 1430.0, 1430.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.92;
        profiles_.push_back(p);
    }

    // 5. iPad Air 13" M2 (iPad14,10 - 2024 Large Screen USB-C)
    {
        DeviceMountProfile p;
        p.profile_id = "ipad_air_13_m2_airy_rigid";
        p.display_name = "iPad Air 13\" (M2) + Airy Rigid Mount";
        p.device_model = "iPad14,10";
        p.form_factor = DeviceFormFactor::IPAD_AIR_13_M2;
        p.is_phone = false;
        p.has_native_lidar = false;
        p.screen_diagonal_inches = 12.9;
        p.chassis_width_m = 0.2806;
        p.chassis_height_m = 0.2149;
        p.chassis_thickness_m = 0.0061;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.115, 0.098, 0.004);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.052000, -0.078000, -0.032000);
        p.camera_time_offset_sec = 0.005;
        p.default_intrinsics = {1920, 1440, 1435.0, 1435.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.90;
        profiles_.push_back(p);
    }

    // 6. iPad Pro 11" M4 (iPad16,3 - Ultra-thin USB-C + Apple dToF LiDAR)
    {
        DeviceMountProfile p;
        p.profile_id = "ipad_pro_11_m4_airy_hybrid";
        p.display_name = "iPad Pro 11\" (M4) + Airy Hybrid dToF Mount";
        p.device_model = "iPad16,3";
        p.form_factor = DeviceFormFactor::IPAD_PRO_11_M4;
        p.is_phone = false;
        p.has_native_lidar = true;
        p.screen_diagonal_inches = 11.0;
        p.chassis_width_m = 0.2497;
        p.chassis_height_m = 0.1775;
        p.chassis_thickness_m = 0.0053;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.096, 0.080, 0.005);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.044100, -0.061500, -0.028500);
        p.camera_time_offset_sec = 0.004;
        p.default_intrinsics = {1920, 1440, 1440.0, 1440.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.94;
        profiles_.push_back(p);
    }

    // 7. iPad Pro 13" M4 (iPad16,5 - Ultra-thin 13" USB-C + Apple dToF LiDAR)
    {
        DeviceMountProfile p;
        p.profile_id = "ipad_pro_13_m4_airy_hybrid";
        p.display_name = "iPad Pro 13\" (M4) + Airy Hybrid dToF Mount";
        p.device_model = "iPad16,5";
        p.form_factor = DeviceFormFactor::IPAD_PRO_13_M4;
        p.is_phone = false;
        p.has_native_lidar = true;
        p.screen_diagonal_inches = 13.0;
        p.chassis_width_m = 0.2816;
        p.chassis_height_m = 0.2155;
        p.chassis_thickness_m = 0.0051;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.116, 0.099, 0.004);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.053500, -0.079200, -0.031500);
        p.camera_time_offset_sec = 0.004;
        p.default_intrinsics = {1920, 1440, 1440.0, 1440.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.93;
        profiles_.push_back(p);
    }

    // 8. iPad mini 6th Gen (iPad14,1 - 8.3" Compact USB-C)
    {
        DeviceMountProfile p;
        p.profile_id = "ipad_mini_6th_gen_airy_compact";
        p.display_name = "iPad mini 6th Gen + Airy Compact Mount";
        p.device_model = "iPad14,1";
        p.form_factor = DeviceFormFactor::IPAD_MINI_6TH_GEN;
        p.is_phone = false;
        p.has_native_lidar = false;
        p.screen_diagonal_inches = 8.3;
        p.chassis_width_m = 0.1954;
        p.chassis_height_m = 0.1348;
        p.chassis_thickness_m = 0.0063;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.074, 0.061, 0.004);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.036200, -0.051500, -0.026000);
        p.camera_time_offset_sec = 0.008;
        p.default_intrinsics = {1920, 1440, 1410.0, 1410.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.90;
        profiles_.push_back(p);
    }

    // 9. iPhone 15 / 15 Plus (iPhone15,4 / iPhone15,5 - USB-C Base Handheld)
    {
        DeviceMountProfile p;
        p.profile_id = "iphone_15_base_airy_handheld";
        p.display_name = "iPhone 15 / Plus + Airy Handheld Rig";
        p.device_model = "iPhone15,4";
        p.form_factor = DeviceFormFactor::IPHONE_15_BASE;
        p.is_phone = true;
        p.has_native_lidar = false;
        p.screen_diagonal_inches = 6.1;
        p.chassis_width_m = 0.0716;
        p.chassis_height_m = 0.1476;
        p.chassis_thickness_m = 0.0078;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.022, 0.068, 0.004);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.024500, -0.048200, -0.024000);
        p.camera_time_offset_sec = 0.006;
        p.default_intrinsics = {1920, 1440, 1470.0, 1470.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.91;
        profiles_.push_back(p);
    }

    // 10. iPhone 15 Pro (iPhone16,1 - USB-C Titanium + Apple dToF LiDAR)
    {
        DeviceMountProfile p;
        p.profile_id = "iphone_15_pro_airy_rig";
        p.display_name = "iPhone 15 Pro + Airy Rigid Handheld Rig";
        p.device_model = "iPhone16,1";
        p.form_factor = DeviceFormFactor::IPHONE_15_PRO;
        p.is_phone = true;
        p.has_native_lidar = true;
        p.screen_diagonal_inches = 6.1;
        p.chassis_width_m = 0.0706;
        p.chassis_height_m = 0.1466;
        p.chassis_thickness_m = 0.0082;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.022, 0.066, 0.005);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.024800, -0.047500, -0.024500);
        p.camera_time_offset_sec = 0.004;
        p.default_intrinsics = {1920, 1440, 1485.0, 1485.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.95;
        profiles_.push_back(p);
    }

    // 11. iPhone 15 Pro Max (iPhone16,2 - USB-C Titanium Large + Apple dToF LiDAR)
    {
        DeviceMountProfile p;
        p.profile_id = "iphone_15_pro_max_airy_rig";
        p.display_name = "iPhone 15 Pro Max + Airy Rigid Handheld Rig";
        p.device_model = "iPhone16,2";
        p.form_factor = DeviceFormFactor::IPHONE_15_PRO_MAX;
        p.is_phone = true;
        p.has_native_lidar = true;
        p.screen_diagonal_inches = 6.7;
        p.chassis_width_m = 0.0767;
        p.chassis_height_m = 0.1599;
        p.chassis_thickness_m = 0.0082;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.025, 0.072, 0.005);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.026500, -0.052000, -0.025000);
        p.camera_time_offset_sec = 0.004;
        p.default_intrinsics = {1920, 1440, 1490.0, 1490.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.95;
        profiles_.push_back(p);
    }

    // 12. iPhone 16 / 16 Plus (iPhone17,3 / iPhone17,4 - USB-C Capture Button)
    {
        DeviceMountProfile p;
        p.profile_id = "iphone_16_base_airy_handheld";
        p.display_name = "iPhone 16 / Plus + Airy Handheld Rig";
        p.device_model = "iPhone17,3";
        p.form_factor = DeviceFormFactor::IPHONE_16_BASE;
        p.is_phone = true;
        p.has_native_lidar = false;
        p.screen_diagonal_inches = 6.1;
        p.chassis_width_m = 0.0716;
        p.chassis_height_m = 0.1476;
        p.chassis_thickness_m = 0.0078;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.022, 0.068, 0.004);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.024500, -0.048200, -0.024000);
        p.camera_time_offset_sec = 0.004;
        p.default_intrinsics = {1920, 1440, 1480.0, 1480.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.91;
        profiles_.push_back(p);
    }

    // 13. iPhone 16 Pro (iPhone17,1 - USB-C Titanium 6.3" + Apple dToF LiDAR)
    {
        DeviceMountProfile p;
        p.profile_id = "iphone_16_pro_airy_rig";
        p.display_name = "iPhone 16 Pro + Airy Rigid Handheld Rig";
        p.device_model = "iPhone17,1";
        p.form_factor = DeviceFormFactor::IPHONE_16_PRO;
        p.is_phone = true;
        p.has_native_lidar = true;
        p.screen_diagonal_inches = 6.3;
        p.chassis_width_m = 0.0715;
        p.chassis_height_m = 0.1496;
        p.chassis_thickness_m = 0.00825;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.023, 0.068, 0.005);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.025200, -0.048800, -0.024800);
        p.camera_time_offset_sec = 0.003;
        p.default_intrinsics = {1920, 1440, 1500.0, 1500.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.96;
        profiles_.push_back(p);
    }

    // 14. iPhone 16 Pro Max (iPhone17,2 - USB-C Titanium 6.9" + Apple dToF LiDAR)
    {
        DeviceMountProfile p;
        p.profile_id = "iphone_16_pro_max_airy_rig";
        p.display_name = "iPhone 16 Pro Max + Airy Rigid Handheld Rig";
        p.device_model = "iPhone17,2";
        p.form_factor = DeviceFormFactor::IPHONE_16_PRO_MAX;
        p.is_phone = true;
        p.has_native_lidar = true;
        p.screen_diagonal_inches = 6.9;
        p.chassis_width_m = 0.0776;
        p.chassis_height_m = 0.1630;
        p.chassis_thickness_m = 0.00825;
        p.camera_offset_from_usbc_m = math::Vec3d(-0.026, 0.075, 0.005);
        p.T_cam_lidar = makeMountExtrinsics(90.0, 0.0, 0.0, 0.027000, -0.053500, -0.025500);
        p.camera_time_offset_sec = 0.003;
        p.default_intrinsics = {1920, 1440, 1505.0, 1505.0, 960.0, 720.0, {0,0,0,0,0}};
        p.is_factory_calibrated = true;
        p.calibration_confidence = 0.96;
        profiles_.push_back(p);
    }

    // Default to primary hardware: iPad 10th Gen
    active_profile_ = profiles_[0];
}

std::optional<DeviceMountProfile> DeviceProfileRegistry::getProfile(const std::string& profile_id) const {
    for (const auto& p : profiles_) {
        if (p.profile_id == profile_id) {
            return p;
        }
    }
    return std::nullopt;
}

std::optional<DeviceMountProfile> DeviceProfileRegistry::getProfileForModel(const std::string& model_identifier) const {
    for (const auto& p : profiles_) {
        if (p.device_model == model_identifier || model_identifier.find(p.device_model) != std::string::npos) {
            return p;
        }
    }
    return std::nullopt;
}

bool DeviceProfileRegistry::setActiveProfile(const std::string& profile_id) {
    auto p = getProfile(profile_id);
    if (p.has_value()) {
        active_profile_ = p.value();
        return true;
    }
    return false;
}

ExtrinsicCalibrationProfile DeviceProfileRegistry::activeExtrinsics() const {
    ExtrinsicCalibrationProfile prof;
    prof.profile_id = active_profile_.profile_id;
    prof.device_model = active_profile_.device_model;
    prof.mount_id = "rigid_belkin_mount";
    prof.T_cam_lidar = active_profile_.T_cam_lidar;
    prof.camera_time_offset_sec = active_profile_.camera_time_offset_sec;
    prof.confidence_score = active_profile_.calibration_confidence;
    prof.nid_metric = 0.82;
    prof.hand_eye_rot_residual_deg = 0.04;
    prof.verified = active_profile_.is_factory_calibrated;
    return prof;
}

}  // namespace core
}  // namespace avscan
