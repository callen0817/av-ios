#pragma once

#include "avscan/math/SophusLie.hpp"
#include "avscan/core/Types.hpp"
#include <cmath>

namespace avscan {
namespace terrestrial {

struct TripodAxisCalibration {
    // Mechanical offset from Tripod Rotation Axis center to Device Mounting Screw (in Station/Body frame)
    // Station frame: +X Forward at theta=0, +Y Left (CCW 90 deg), +Z Up (along tripod vertical axis)
    math::Vec3d T_axis_body_offset_m{0.0, 0.0, 0.05}; // 5cm height above mount plate
    // Tilt of rotation axis relative to gravity vector (roll, pitch in rad; 0 for leveled tripod)
    math::Vec2d axis_tilt_leveling_rad{0.0, 0.0};
};

class RotationGeometry {
public:
    // Canonical Mapping from Camera Optical Frame (+X Right, +Y Down, +Z Forward)
    // to Station Reference Frame at theta = 0 (+X Forward, +Y Left, +Z Up):
    //   X_station (Forward) = +Z_cam
    //   Y_station (Left)    = -X_cam
    //   Z_station (Up)      = -Y_cam
    static math::SE3d cameraOpticalToStationZero() {
        math::Mat3d R_sc = math::Mat3d::Zero();
        R_sc(0, 2) =  1.0; // X_station = +Z_cam (Forward)
        R_sc(1, 0) = -1.0; // Y_station = -X_cam (Left)
        R_sc(2, 1) = -1.0; // Z_station = -Y_cam (Up)
        return math::SE3d(math::SO3d(R_sc), math::Vec3d(0.0, 0.0, 0.0));
    }

    // Computes T_station_lidar for an arbitrary rotation angle theta around the tripod +Z_station axis:
    // P_station = T_station_axis * R_z(theta) * T_axis_body * T_body_cam * T_cam_lidar * P_lidar
    // Where T_cam_lidar is the calibrated extrinsic from DeviceMountProfile (incorporating 90 deg pitch on iOS or 45 deg on scanAR).
    static math::SE3d computeStationLidarTransform(double rotation_angle_deg,
                                                  const math::SE3d& T_cam_lidar,
                                                  const TripodAxisCalibration& axis_calib = TripodAxisCalibration()) {
        double theta_rad = rotation_angle_deg * math::kDegToRad;

        // 1. Rotation R_z(theta) around the vertical tripod rotation axis (+Z_station)
        math::Quatd q_z = math::Quatd::FromEuler(0.0, 0.0, theta_rad);
        math::SE3d R_axis(math::SO3d(q_z), math::Vec3d(0.0, 0.0, 0.0));

        // 2. Translation offset of device body above the tripod mount center
        math::SE3d T_axis_body(math::SO3d(), axis_calib.T_axis_body_offset_m);

        // 3. Transform from Camera Optical Frame to Station Reference Frame at theta=0
        math::SE3d T_body_cam = cameraOpticalToStationZero();

        // 4. Station leveling transform (if tripod has dual-axis bubble tilt calibration)
        math::Quatd q_tilt = math::Quatd::FromEuler(axis_calib.axis_tilt_leveling_rad.x,
                                                    axis_calib.axis_tilt_leveling_rad.y,
                                                    0.0);
        math::SE3d T_station_axis(math::SO3d(q_tilt), math::Vec3d(0.0, 0.0, 0.0));

        // Total forward kinematic chain:
        return T_station_axis * R_axis * T_axis_body * T_body_cam * T_cam_lidar;
    }

    // Rotates a point from LiDAR sensor frame into station frame at angle theta_deg
    static core::LidarPointXYZIRT transformPointToStation(const core::LidarPointXYZIRT& p_lidar,
                                                          const math::SE3d& T_station_lidar) {
        core::LidarPointXYZIRT p_stat = p_lidar;
        math::Vec3d v_in(p_lidar.x, p_lidar.y, p_lidar.z);
        math::Vec3d v_out = T_station_lidar * v_in;
        p_stat.x = v_out.x;
        p_stat.y = v_out.y;
        p_stat.z = v_out.z;
        return p_stat;
    }
};

} // namespace terrestrial
} // namespace avscan
