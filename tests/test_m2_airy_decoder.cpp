#include "avscan/sensors/AiryDecoder.hpp"
#include "avscan/sensors/AiryUdpReceiver.hpp"
#include <array>
#include <cmath>
#include <filesystem>

bool run_test_m2_airy_decoder() {
    using namespace avscan;

    sensors::AiryDecoder decoder;

    // 1. Test DIFOP 1248-byte encode/decode round-trip including factory IMU extrinsics
    core::AiryFactoryCalibration cal_in;
    cal_in.sensor_ip = "192.168.1.200";
    cal_in.dest_ip = "192.168.1.102";
    cal_in.msop_port = 6699;
    cal_in.difop_port = 7788;
    cal_in.imu_port = 6688;
    for (size_t i = 0; i < 96; ++i) {
        cal_in.vertical_angles_deg[i] = -10.0 + static_cast<double>(i) * 1.0;
        cal_in.horizontal_angles_deg[i] = 0.25;
    }
    cal_in.T_imu_lidar = math::SE3d(
        math::SO3d::exp(math::Vec3d(0.01, -0.02, 0.03)),
        math::Vec3d(0.012, -0.005, 0.024));

    std::array<uint8_t, sensors::AiryDecoder::kDifopPacketSize> difop_bytes{};
    sensors::AiryDecoder::buildDifopPacket(cal_in, difop_bytes);

    core::AiryFactoryCalibration cal_out;
    if (!decoder.decodeDifopPacket(difop_bytes.data(), difop_bytes.size(), cal_out)) return false;
    if (!cal_out.valid || cal_out.msop_port != 6699 || cal_out.imu_port != 6688) return false;
    if ((cal_out.T_imu_lidar.t - cal_in.T_imu_lidar.t).norm() > 1e-4) return false;

    // 2. Test IMU 51-byte packet encode/decode round-trip
    core::ImuMeasurement imu_in;
    imu_in.timestamp_sec = 1700000123.456789;
    imu_in.accel_mps2 = {0.15, -0.22, 9.805};
    imu_in.gyro_rads = {0.01, -0.02, 0.35};
    imu_in.temperature_c = 34.5f;
    imu_in.sequence_id = 42;

    std::array<uint8_t, sensors::AiryDecoder::kImuPacketSize> imu_bytes{};
    sensors::AiryDecoder::buildImuPacket(imu_in, imu_bytes);

    core::ImuMeasurement imu_out;
    if (!decoder.decodeImuPacket(imu_bytes.data(), imu_bytes.size(), imu_out)) return false;
    if (std::abs(imu_out.timestamp_sec - imu_in.timestamp_sec) > 2e-6) return false;
    if ((imu_out.accel_mps2 - imu_in.accel_mps2).norm() > 1e-4) return false;
    if ((imu_out.gyro_rads - imu_in.gyro_rads).norm() > 1e-4) return false;

    // 3. Test MSOP 1248-byte packet decoding & malformed rejection
    std::array<uint8_t, sensors::AiryDecoder::kMsopPacketSize> msop_bytes{};
    msop_bytes.fill(0);
    msop_bytes[0] = 0x55; msop_bytes[1] = 0xAA; msop_bytes[2] = 0x05; msop_bytes[3] = 0x5A;
    msop_bytes[15] = 1;  // sequence 1
    msop_bytes[25] = 100; // timestamp 100.0 s
    msop_bytes[31] = 0x31; // Airy type
    msop_bytes[32] = 0x02; // 96-beam model
    for (size_t b = 0; b < 8; ++b) {
        uint8_t* blk = msop_bytes.data() + 42 + b * 148;
        blk[0] = 0xFF; blk[1] = 0xEE;
        uint16_t az = static_cast<uint16_t>((b / 2) * 100);  // 1.0 deg step
        blk[2] = static_cast<uint8_t>((az >> 8) & 0xFF);
        blk[3] = static_cast<uint8_t>(az & 0xFF);
        for (size_t ch = 0; ch < 48; ++ch) {
            uint8_t* cp = blk + 4 + ch * 3;
            uint16_t dist_counts = 1000;  // 5.0 meters (0.005 * 1000)
            cp[0] = static_cast<uint8_t>((dist_counts >> 8) & 0xFF);
            cp[1] = static_cast<uint8_t>(dist_counts & 0xFF);
            cp[2] = 180;  // reflectivity
        }
    }
    msop_bytes[1246] = 0x00; msop_bytes[1247] = 0xFF;

    std::vector<core::LidarPointXYZIRT> pts;
    bool sweep_done = false;
    core::LidarFrame completed_frame;
    if (!decoder.decodeMsopPacket(msop_bytes.data(), msop_bytes.size(), pts, sweep_done, completed_frame)) return false;
    if (pts.size() != 8 * 48) return false;
    if (std::abs(pts[0].range_m - 5.0f) > 1e-3f) return false;

    // Malformed packet rejection test
    msop_bytes[0] = 0x00;
    if (decoder.decodeMsopPacket(msop_bytes.data(), msop_bytes.size(), pts, sweep_done, completed_frame)) return false;
    if (decoder.getStats().msop_packets_malformed != 1) return false;

    return true;
}
