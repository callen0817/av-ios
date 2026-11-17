#pragma once

#include "avscan/math/SophusLie.hpp"
#include <cmath>

namespace avscan {
namespace terrestrial {

struct TripodAxisCalibration {
    // Mechanical offset from Tripod Rotation Axis center to Device Mounting Screw
    math::Vec3d T_axis_body_offset_m{0.0, 0.0, 0.05}; // e.g. 5cm height above mount plate
    // Tilt of rotation axis relative to gravity vector (normally 0 for leveled tripod)
    math::Vec2d axis_tilt_leveling_rad{0.0, 0.0};
};

class RotationGeometry {
public:
    // Computes T_station_lidar for an arbitrary rotation angle theta around the tripod Z-axis:
    // P_station = T_station_axis * R_z(theta) * T_axis_body * T_body_lidar * P_lidar
    static math::SE3d computeStationLidarTransform(double rotation_angle_deg,
                                                  const math::SE3d& T_body_lidar,
                                                  const TripodAxisCalibration& axis_calib = TripodAxisCalibration()) {
        double theta_rad = rotation_angle_deg * math::kDegToRad;

        // 1. Rotation R_z(theta) around the vertical tripod rotation axis
        math::Quatd q_z = math::Quatd::FromEuler(0.0, 0.0, theta_rad);
        math::SE3d R_axis(math::SO3d(q_z), math::Vec3d(0.0, 0.0, 0.0));

        // 2. Translation offset of device body above the tripod mount center
        math::SE3d T_axis_body(math::SO3d(), axis_calib.T_axis_body_offset_m);

        // 3. Station leveling transform (if tripod has dual-axis bubble tilt calibration)
        math::Quatd q_tilt = math::Quatd::FromEuler(axis_calib.axis_tilt_leveling_rad.x,
                                                    axis_calib.axis_tilt_leveling_rad.y,
                                                    0.0);
        math::SE3d T_station_axis(math::SO3d(q_tilt), math::Vec3d(0.0, 0.0, 0.0));

        // Total forward kinematic chain:
        return T_station_axis * R_axis * T_axis_body * T_body_lidar;
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
