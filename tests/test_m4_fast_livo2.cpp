#include "avscan/slam/FastLivo2Engine.hpp"
#include <cmath>

bool run_test_m4_fast_livo2() {
    using namespace avscan;

    // 1. Verify SO(3) and SE(3) exponential/logarithm round-trip
    math::Vec3d omega(0.15, -0.25, 0.35);
    math::SO3d R = math::SO3d::exp(omega);
    math::Vec3d omega_rec = R.log();
    if ((omega - omega_rec).norm() > 1e-7) return false;

    math::Vec6d xi{1.2, -0.5, 0.3, 0.1, -0.2, 0.15};
    math::SE3d T = math::SE3d::exp(xi);
    math::Vec6d xi_rec = T.log();
    for (size_t i = 0; i < 6; ++i) {
        if (std::abs(xi[i] - xi_rec[i]) > 1e-6) return false;
    }

    // 2. Verify FastLivo2Engine LIO tracking on a 3-plane orthogonal room corner with IMU propagation
    slam::FastLivo2Engine engine;
    for (int f = 0; f < 5; ++f) {
        sync::SyncBundle bundle;
        double t0 = f * 0.10;
        double t1 = t0 + 0.10;
        bundle.lidar_frame.frame_id = static_cast<uint64_t>(f + 1);
        bundle.lidar_frame.timestamp_start_sec = t0;
        bundle.lidar_frame.timestamp_end_sec = t1;

        for (int k = 0; k < 20; ++k) {
            core::ImuMeasurement imu;
            imu.timestamp_sec = t0 + k * 0.005;
            imu.accel_mps2 = {0.0, 0.0, 9.81};
            imu.gyro_rads = {0.0, 0.0, 0.0};
            bundle.imu_measurements.push_back(imu);
        }

        // Generate points on floor (z = -1.0), front wall (x = 4.0), side wall (y = 3.0)
        for (int u = -10; u <= 10; ++u) {
            for (int v = -10; v <= 10; ++v) {
                core::LidarPointXYZIRT p1;
                p1.x = 2.0 + u * 0.08; p1.y = v * 0.08; p1.z = -1.0; p1.timestamp_sec = t0 + 0.02;
                bundle.lidar_frame.points.push_back(p1);

                core::LidarPointXYZIRT p2;
                p2.x = 4.0; p2.y = u * 0.08; p2.z = v * 0.08; p2.timestamp_sec = t0 + 0.05;
                bundle.lidar_frame.points.push_back(p2);

                core::LidarPointXYZIRT p3;
                p3.x = 2.0 + u * 0.08; p3.y = 3.0; p3.z = v * 0.08; p3.timestamp_sec = t0 + 0.08;
                bundle.lidar_frame.points.push_back(p3);
            }
        }

        core::LidarFrame body_f, world_f;
        slam::EstimatorMetrics metrics;
        if (!engine.processBundle(bundle, body_f, world_f, metrics)) return false;
    }

    auto st = engine.getCurrentState();
    return (st.T_world_body.t.norm() < 0.05 && engine.getLocalMap().numValidPlanes() >= 10);
}
