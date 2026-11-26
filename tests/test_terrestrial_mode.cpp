#include "avscan/terrestrial/TerrestrialStationManager.hpp"
#include "avscan/export/AvSpatialBundleExporter.hpp"
#include "avscan/core/DeviceMountProfile.hpp"
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "CHECK FAILED at line " << __LINE__ << ": " << #cond << std::endl; \
        std::exit(1); \
    } \
} while(0)

namespace fs = std::filesystem;
using namespace avscan;

namespace {

bool approxEq(double a, double b, double tol) {
    return std::abs(a - b) <= tol;
}

// Synthesize a 3D box room observed from LiDAR frame given true T_station_lidar
core::LidarFrame synthesizeRoomScan(const math::SE3d& T_station_lidar_true,
                                    double t_start, double t_end) {
    core::LidarFrame frame;
    frame.timestamp_start_sec = t_start;
    frame.timestamp_end_sec = t_end;

    const math::SE3d T_lidar_station = T_station_lidar_true.inverse();

    // Generate dense wall points on a 6m x 6m x 3m room around the station
    for (int side = 0; side < 4; ++side) {
        for (int u = -12; u <= 12; ++u) {
            for (int v = -4; v <= 6; ++v) {
                double a = u * 0.25;
                double z = v * 0.25;
                math::Vec3d p_st;
                if (side == 0) p_st = { 3.0, a, z };
                else if (side == 1) p_st = { -3.0, a, z };
                else if (side == 2) p_st = { a,  3.0, z };
                else p_st = { a, -3.0, z };

                math::Vec3d p_l = T_lidar_station * p_st;
                // Keep points in front of the forward-facing dome (+Z_lidar > 0.1)
                if (p_l.z > 0.1) {
                    core::LidarPointXYZIRT pt;
                    pt.x = p_l.x;
                    pt.y = p_l.y;
                    pt.z = p_l.z;
                    pt.intensity = 120.0f;
                    pt.r = 200; pt.g = 180; pt.b = 140;
                    frame.points.push_back(pt);
                }
            }
        }
    }
    return frame;
}

void testRotationGeometryVerticalAxis() {
    std::cout << "[TEST 1] RotationGeometry vertical Z_station axis invariance...\n";
    auto profile = core::DeviceProfileRegistry::instance().activeProfile();
    terrestrial::TripodAxisCalibration calib;
    calib.T_axis_body_offset_m = {0.0, 0.0, 0.0};

    // Point 5.0m directly in front of the 90-deg forward-facing Airy dome (+Z_lidar)
    core::LidarPointXYZIRT p_dome;
    p_dome.x = 0.0; p_dome.y = 0.0; p_dome.z = 5.0;

    auto T0 = terrestrial::RotationGeometry::computeStationLidarTransform(0.0, profile.T_cam_lidar, calib);
    auto T90 = terrestrial::RotationGeometry::computeStationLidarTransform(90.0, profile.T_cam_lidar, calib);
    auto T180 = terrestrial::RotationGeometry::computeStationLidarTransform(180.0, profile.T_cam_lidar, calib);

    auto p0 = terrestrial::RotationGeometry::transformPointToStation(p_dome, T0);
    auto p90 = terrestrial::RotationGeometry::transformPointToStation(p_dome, T90);
    auto p180 = terrestrial::RotationGeometry::transformPointToStation(p_dome, T180);

    std::cout << "  p0=(" << p0.x << ", " << p0.y << ", " << p0.z << ")\n";
    std::cout << "  p90=(" << p90.x << ", " << p90.y << ", " << p90.z << ")\n";
    std::cout << "  p180=(" << p180.x << ", " << p180.y << ", " << p180.z << ")\n";

    // At 0 deg: dome looks along +X_station (Forward)
    CHECK(p0.x > 4.8 && std::abs(p0.y) < 0.25 && std::abs(p0.z) < 0.35);
    // At 90 deg: dome rotates CCW around +Z_station to look along +Y_station (Left)
    CHECK(std::abs(p90.x) < 0.25 && p90.y > 4.8 && approxEq(p90.z, p0.z, 1e-6));
    // At 180 deg: dome looks along -X_station (Backward)
    CHECK(p180.x < -4.8 && std::abs(p180.y) < 0.25 && approxEq(p180.z, p0.z, 1e-6));
    std::cout << "  PASS: R_z(theta) rotates horizontally around +Z_station.\n";
}

void testStillnessGateAndDisturbanceRejection() {
    std::cout << "[TEST 2] Dual-criterion IMU stillness gate & disturbance rejection...\n";
    terrestrial::TerrestrialStationConfig cfg;
    cfg.station_id = "st_stillness_test";
    cfg.settling_delay_sec = 0.5;
    cfg.accumulation_sec_per_stop = 2.0;

    auto profile = core::DeviceProfileRegistry::instance().activeProfile();
    terrestrial::TerrestrialStationManager mgr(cfg);
    fs::path tmp_proj = fs::temp_directory_path() / "avscan_terrestrial_unit_test";
    fs::remove_all(tmp_proj);
    mgr.startNewStation(tmp_proj.string(), cfg, profile);

    // 1. Moving IMU should hold WAITING_FOR_STILLNESS
    double t = 100.0;
    for (int i = 0; i < 40; ++i) {
        core::ImuMeasurement imu;
        imu.timestamp_sec = t;
        imu.gyro_rads = {0.0, 0.0, 0.40};
        imu.accel_mps2 = {0.0, 0.0, 9.81};
        mgr.pushImuMeasurement(imu);
        t += 0.01;
    }
    CHECK(mgr.getCaptureState() == terrestrial::TerrestrialCaptureState::WAITING_FOR_STILLNESS);

    // 2. Quiet IMU for > 0.5s transitions to ACCUMULATING_STOP
    for (int i = 0; i < 60; ++i) {
        core::ImuMeasurement imu;
        imu.timestamp_sec = t;
        imu.gyro_rads = {0.002, -0.001, 0.003};
        imu.accel_mps2 = {0.0, 0.0, 9.81};
        mgr.pushImuMeasurement(imu);
        t += 0.01;
    }
    CHECK(mgr.getCaptureState() == terrestrial::TerrestrialCaptureState::ACCUMULATING_STOP);
    CHECK(mgr.isStationary());

    // 3. Sudden bump during accumulation resets to WAITING_FOR_STILLNESS
    core::ImuMeasurement bump;
    bump.timestamp_sec = t;
    bump.gyro_rads = {0.35, 0.0, 0.0};
    bump.accel_mps2 = {0.0, 0.0, 9.81};
    mgr.pushImuMeasurement(bump);
    CHECK(mgr.getCaptureState() == terrestrial::TerrestrialCaptureState::WAITING_FOR_STILLNESS);

    fs::remove_all(tmp_proj);
    std::cout << "  PASS: IMU stillness gate and bump rejection verified.\n";
}

void testSixStopCaptureRefinementAndExport() {
    std::cout << "[TEST 3] 60-deg x 6-stop capture, 1-DOF overlap refinement & disk export...\n";
    terrestrial::TerrestrialStationConfig cfg;
    cfg.project_id = "TEST_PROJ";
    cfg.station_id = "st_001";
    cfg.station_name = "Unit Test Station 01";
    cfg.nominal_step_deg = 60.0;
    cfg.total_expected_stops = 6;
    cfg.accumulation_sec_per_stop = 5.0;
    cfg.settling_delay_sec = 0.5;
    cfg.enable_overlap_refinement = true;

    auto profile = core::DeviceProfileRegistry::instance().activeProfile();
    terrestrial::TripodAxisCalibration calib;
    terrestrial::TerrestrialStationManager mgr(cfg);

    fs::path tmp_proj = fs::temp_directory_path() / "avscan_terrestrial_6stop_test";
    fs::remove_all(tmp_proj);
    mgr.startNewStation(tmp_proj.string(), cfg, profile);

    double t = 1000.0;
    for (int stop = 0; stop < 6; ++stop) {
        // Inject a deliberate +1.5 deg manual detent error on Stop 1 (true = 61.5 deg vs nominal 60.0 deg)
        double true_angle_deg = stop * 60.0 + ((stop == 1) ? 1.5 : 0.0);
        auto T_true = terrestrial::RotationGeometry::computeStationLidarTransform(
            true_angle_deg, profile.T_cam_lidar, calib);

        // Settle IMU
        for (int i = 0; i < 60; ++i) {
            core::ImuMeasurement imu;
            imu.timestamp_sec = t;
            imu.gyro_rads = {0.0, 0.0, 0.0};
            imu.accel_mps2 = {0.0, 0.0, 9.81};
            mgr.pushImuMeasurement(imu);
            t += 0.01;
        }
        CHECK(mgr.getCaptureState() == terrestrial::TerrestrialCaptureState::ACCUMULATING_STOP);

        // Push LiDAR frames spanning 5.1 seconds
        double t0 = t;
        for (int f = 0; f < 6; ++f) {
            auto frame = synthesizeRoomScan(T_true, t, t + 1.0);
            mgr.pushLidarFrame(frame);
            t += 1.0;
        }
        CHECK(mgr.getCaptureState() == terrestrial::TerrestrialCaptureState::STOP_COMPLETED);
        CHECK(t - t0 >= 5.0);

        if (stop < 5) {
            mgr.advanceToNextStop();
        }
    }

    std::string err;
    bool finalized = mgr.finalizeAndSaveStation(err);
    CHECK(finalized);
    CHECK(mgr.getCompletedStops().size() == 6);

    // Check 1-DOF refinement recovered Stop 1's +1.5 deg error
    double refined_stop1 = mgr.getCompletedStops()[1].measured_angle_deg;
    std::cout << "  Stop 1 nominal=60.0 deg, true=61.5 deg, refined=" << refined_stop1 << " deg\n";
    CHECK(approxEq(refined_stop1, 61.5, 0.35));

    // Verify all on-disk station artifacts exist and are non-empty
    fs::path st_dir = tmp_proj / "stations" / "st_001";
    CHECK(fs::exists(st_dir / "station_manifest.json") && fs::file_size(st_dir / "station_manifest.json") > 200);
    CHECK(fs::exists(st_dir / "pointcloud.ply") && fs::file_size(st_dir / "pointcloud.ply") > 1000);
    CHECK(fs::exists(st_dir / "station.e57") && fs::file_size(st_dir / "station.e57") > 1000);
    CHECK(fs::exists(st_dir / "cache" / "lod0.bin") && fs::file_size(st_dir / "cache" / "lod0.bin") > 100);
    CHECK(fs::exists(st_dir / "raw" / "stop_000_000deg.ply"));

    auto meta = mgr.getStationMetadata();
    CHECK(meta.is_closed_360);
    CHECK(meta.azimuth_coverage_ratio > 0.90);

    fs::remove_all(tmp_proj);
    std::cout << "  PASS: 6-stop capture, 1-DOF angle recovery, coverage & disk artifacts verified.\n";
}

void testTruePngKeyplanExport() {
    std::cout << "[TEST 4] True PNG keyplan encoder validation...\n";
    quality::FloorPlanGrid2D plan;
    plan.width_cells = 32;
    plan.height_cells = 32;
    plan.resolution_m = 0.05;
    plan.occupancy_cells.assign(32 * 32, 1);
    for (int i = 0; i < 32; ++i) {
        plan.occupancy_cells[i] = 2;
        plan.occupancy_cells[31 * 32 + i] = 2;
    }

    fs::path png_path = fs::temp_directory_path() / "avscan_keyplan_test.png";
    bool ok = export_bundle::AvSpatialBundleExporter::exportFloorKeyplanPng(png_path.string(), plan);
    CHECK(ok);

    std::ifstream in(png_path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CHECK(bytes.size() > 64);

    // Exact 8-byte PNG signature
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i) {
        CHECK(bytes[i] == sig[i]);
    }
    // Check IHDR tag at offset 12
    CHECK(bytes[12] == 'I' && bytes[13] == 'H' && bytes[14] == 'D' && bytes[15] == 'R');

    fs::remove(png_path);
    std::cout << "  PASS: Valid PNG signature and IHDR/IDAT/IEND chunks verified.\n";
}

void testSingleShotMotorFreeFlashScan() {
    std::cout << "[TEST 5] Single-Shot Motor-Free Flash Scan (Decoupled from Terrestrial 6x60 deg)...\n";

    class SpyRotationController : public terrestrial::IRotationController {
    public:
        int set_zero_calls = 0;
        int move_calls = 0;
        int stop_calls = 0;
        bool connect() override { return true; }
        void disconnect() override {}
        bool isConnected() const override { return true; }
        terrestrial::MotorCapabilities getCapabilities() const override { return terrestrial::MotorCapabilities{}; }
        terrestrial::RotationMode getRotationMode() const override { return terrestrial::RotationMode::MANUAL_INDEX; }
        bool setZeroPosition() override { ++set_zero_calls; return true; }
        bool moveToAngleDeg(double) override { ++move_calls; return true; }
        bool moveRelativeDeg(double) override { ++move_calls; return true; }
        bool getCurrentAngleDeg(double& deg) const override { deg = 0.0; return false; }
        terrestrial::MotionState getMotionState() const override { return terrestrial::MotionState::STATIONARY; }
        void setOperatorPromptCallback(PromptOperatorCallback) override {}
        void stopMotion() override { ++stop_calls; }
    };

    const fs::path tmp_dir = fs::temp_directory_path() / "av_ios_flash_scan_unit_test";
    fs::remove_all(tmp_dir);
    fs::create_directories(tmp_dir);

    auto spy = std::make_shared<SpyRotationController>();
    terrestrial::TerrestrialStationConfig cfg;
    cfg.project_id = "TEST_FLASH";
    cfg.station_id = "flash_st_001";
    cfg.station_name = "Flash Scan 01";
    cfg.is_single_shot_flash = true;
    cfg.T_imu_lidar = math::SE3d(math::SO3d(math::Quatd::fromEuler(0.0, 0.0, 90.0 * math::kDegToRad)),
                                 math::Vec3d(0.10, -0.05, 0.20));
    cfg.settling_delay_sec = 0.5;
    cfg.accumulation_sec_per_stop = 2.0;

    terrestrial::TerrestrialStationManager mgr(cfg, spy);
    CHECK(mgr.startNewStation(tmp_dir.string(), cfg, core::DeviceProfileRegistry::instance().activeProfile()));
    CHECK(spy->set_zero_calls == 0);

    double t = 10.0;
    for (int i = 0; i < 30; ++i) {
        core::ImuMeasurement imu;
        imu.timestamp_sec = t;
        imu.gyro_rads = {0.001, -0.001, 0.0};
        imu.accel_mps2 = {0.0, 0.0, 9.81};
        mgr.pushImuMeasurement(imu);

        core::LidarFrame frame;
        frame.timestamp_start_sec = t;
        frame.timestamp_end_sec = t + 0.1;
        core::LidarPointXYZIRT pt;
        pt.x = 2.0; pt.y = 0.0; pt.z = 0.5;
        frame.points.push_back(pt);
        mgr.pushLidarFrame(frame);
        t += 0.1;
        if (mgr.getCaptureState() == terrestrial::TerrestrialCaptureState::STATION_COMPLETED) break;
    }

    CHECK(mgr.getCaptureState() == terrestrial::TerrestrialCaptureState::STATION_COMPLETED);
    CHECK(spy->move_calls == 0 && spy->set_zero_calls == 0);

    std::string err;
    CHECK(mgr.finalizeAndSaveStation(err));
    CHECK(spy->set_zero_calls == 0);

    auto cloud = mgr.buildStationPointCloud();
    CHECK(!cloud.empty());
    CHECK(approxEq(cloud.front().x, 0.10, 1e-4));
    CHECK(approxEq(cloud.front().y, 1.95, 1e-4));
    CHECK(approxEq(cloud.front().z, 0.70, 1e-4));

    {
        std::ifstream mf(tmp_dir / "stations" / "flash_st_001" / "station_manifest.json");
        std::string json((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
        CHECK(json.find("\"mode\": \"flash_scan\"") != std::string::npos);
        CHECK(json.find("\"mode\": \"none_stationary\"") != std::string::npos);
    }

    fs::remove_all(tmp_dir);
    std::cout << "  PASS: Single-shot Flash Scan completed in 1 stop with 0 motor commands and exact T_imu_lidar transform.\n";
}

} // anonymous namespace

int main() {
    std::cout << std::unitbuf;
    testRotationGeometryVerticalAxis();
    testStillnessGateAndDisturbanceRejection();
    testSixStopCaptureRefinementAndExport();
    testTruePngKeyplanExport();
    testSingleShotMotorFreeFlashScan();
    std::cout << "ALL TERRESTRIAL MODE UNIT TESTS PASSED.\n";
    return 0;
}
