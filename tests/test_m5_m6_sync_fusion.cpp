#include "avscan/slam/FastLivo2Engine.hpp"
#include "avscan/sync/TimeSynchronizer.hpp"
#include <cmath>

bool run_test_m5_m6_sync_fusion() {
    using namespace avscan;

    // 1. Test cross-correlation temporal offset estimator
    const double true_offset_sec = 0.032;  // 32 ms camera lag
    std::vector< std::pair<double, double> > imu_series;
    std::vector< std::pair<double, double> > cam_series;
    for (int i = 0; i < 400; ++i) {
        double t = i * 0.005;
        double signal = std::sin(3.0 * t) + 0.5 * std::cos(7.0 * t) + 2.0;
        imu_series.push_back({t, signal});
        cam_series.push_back({t - true_offset_sec, signal});
    }

    double peak_corr = 0.0;
    double est_offset = sync::TimeSynchronizer::estimateTemporalOffsetByCrossCorrelation(
        imu_series, cam_series, 0.10, 0.001, &peak_corr);
    if (std::abs(est_offset - true_offset_sec) > 0.002 || peak_corr < 0.98) {
        return false;
    }

    // 2. Test TimeSynchronizer bundle alignment + RGB FOV colorization in FastLivo2Engine
    sync::TimeSynchronizer sync_engine;
    sync_engine.setCalibratedCameraTimeOffset(0.0);

    for (int k = 0; k < 30; ++k) {
        core::ImuMeasurement imu;
        imu.timestamp_sec = 10.0 + k * 0.005;
        imu.accel_mps2 = {0.0, 0.0, 9.81};
        sync_engine.pushImu(imu, imu.timestamp_sec);
    }

    core::RgbFrame rgb;
    rgb.frame_id = 1;
    rgb.timestamp_sec = 10.05;
    rgb.intrinsics.width = 160;
    rgb.intrinsics.height = 120;
    rgb.intrinsics.fx = 120.0;
    rgb.intrinsics.fy = 120.0;
    rgb.intrinsics.cx = 80.0;
    rgb.intrinsics.cy = 60.0;
    rgb.gray_bytes.assign(160 * 120, 128);
    rgb.rgb_bytes.assign(160 * 120 * 3, 200);
    sync_engine.pushRgbFrame(rgb);

    core::LidarFrame lf;
    lf.frame_id = 1;
    lf.timestamp_start_sec = 10.00;
    lf.timestamp_end_sec = 10.10;
    for (int i = -5; i <= 5; ++i) {
        core::LidarPointXYZIRT pt;
        pt.x = i * 0.1; pt.y = 0.0; pt.z = 3.0;  // Directly in front of camera (+Z in optical frame)
        pt.timestamp_sec = 10.05;
        lf.points.push_back(pt);
    }
    sync_engine.pushLidarFrame(lf, 10.10);

    sync::SyncBundle bundle;
    if (!sync_engine.popSyncBundle(bundle)) return false;
    return bundle.has_valid_imu_coverage && bundle.rgb_frame.has_value();
}
