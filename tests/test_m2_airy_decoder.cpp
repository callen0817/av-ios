#include "avscan/sensors/AiryDecoder.hpp"
#include "avscan/sensors/AiryUdpReceiver.hpp"
#include <array>
#include <cmath>
#include <cstring>
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

    // 3. Test MSOP 1248-byte packet decoding (official decoder_RSAIRY.hpp Y-negation + lens center offset) & malformed rejection
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
        uint16_t az = static_cast<uint16_t>(9000 + (b / 2) * 100);  // 90.0 deg -> +sin(90)=+1 -> y MUST be negative!
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
    // Verify clockwise azimuth Y-negation: at az ~= 90 deg, y must be strongly negative (~ -5.0 m)
    if (pts[0].y > -4.0) return false;

    // 4. Test real hardware Airy DIFOP offset 1092 (qx,qy,qz,qw,tx,ty,tz) = T_lidar_imu -> inverted to T_imu_lidar
    {
        std::array<uint8_t, sensors::AiryDecoder::kDifopPacketSize> hw_difop{};
        hw_difop.fill(0);
        hw_difop[0] = 0xA5; hw_difop[1] = 0xFF; hw_difop[2] = 0x00; hw_difop[3] = 0x5A;
        hw_difop[4] = 0x11; hw_difop[5] = 0x11; hw_difop[6] = 0x55; hw_difop[7] = 0x55;
        // Hardware MSOP port 6699 (0x1A2B) at offset 24 & DIFOP tail 0x0F 0xF0
        hw_difop[24] = 0x1A; hw_difop[25] = 0x2B;
        hw_difop[28] = 0x1E; hw_difop[29] = 0x6C;
        hw_difop[1246] = 0x0F; hw_difop[1247] = 0xF0;
        // Hardware IMU_CALIB_DATA at offset 1092: (qx, qy, qz, qw, tx, ty, tz) big-endian floats matching real Airy DIFOP
        auto writeFloatBE = [&](size_t off, float v) {
            uint32_t u = 0;
            std::memcpy(&u, &v, 4);
            hw_difop[off + 0] = static_cast<uint8_t>((u >> 24) & 0xFF);
            hw_difop[off + 1] = static_cast<uint8_t>((u >> 16) & 0xFF);
            hw_difop[off + 2] = static_cast<uint8_t>((u >> 8) & 0xFF);
            hw_difop[off + 3] = static_cast<uint8_t>(u & 0xFF);
        };
        const float qx = 0.711454f, qy = -0.702718f, qz = 0.001879f, qw = 0.004094f;
        const float tx = 0.00425f, ty = 0.00418f, tz = -0.00446f;
        writeFloatBE(1092, qx);
        writeFloatBE(1096, qy);
        writeFloatBE(1100, qz);
        writeFloatBE(1104, qw);
        writeFloatBE(1108, tx);
        writeFloatBE(1112, ty);
        writeFloatBE(1116, tz);

        sensors::AiryDecoder hw_dec;
        core::AiryFactoryCalibration hw_cal;
        if (!hw_dec.decodeDifopPacket(hw_difop.data(), hw_difop.size(), hw_cal)) return false;

        // Expected T_imu_lidar = T_lidar_imu.inverse()
        math::Quatd q_li(qx, qy, qz, qw);
        q_li.normalize();
        const math::SE3d T_lidar_imu(math::SO3d(q_li), math::Vec3d(tx, ty, tz));
        const math::SE3d expected_T_imu_lidar = T_lidar_imu.inverse();

        const double rot_err = (hw_cal.T_imu_lidar.so3.inverse() * expected_T_imu_lidar.so3).log().norm();
        const double trans_err = (hw_cal.T_imu_lidar.t - expected_T_imu_lidar.t).norm();
        if (rot_err > 1e-4 || trans_err > 1e-4) return false;
    }

    // 5. Test hardware 51-byte Airy IMU dynamic FSR scaling (acceIFsr=1 -> +-4g, gyroIFsr=1 -> +-500dps)
    {
        std::array<uint8_t, sensors::AiryDecoder::kImuPacketSize> hw_imu{};
        hw_imu.fill(0);
        hw_imu[0] = 0xAA; hw_imu[1] = 0x55; hw_imu[2] = 0x5A; hw_imu[3] = 0x05;
        hw_imu[49] = 0xF0; hw_imu[50] = 0x0F;
        hw_imu[9] = 200;  // timestamp 200s at bytes 4..9
        auto writeI32BE = [&](size_t off, int32_t v) {
            uint32_t u = static_cast<uint32_t>(v);
            hw_imu[off + 0] = static_cast<uint8_t>((u >> 24) & 0xFF);
            hw_imu[off + 1] = static_cast<uint8_t>((u >> 16) & 0xFF);
            hw_imu[off + 2] = static_cast<uint8_t>((u >> 8) & 0xFF);
            hw_imu[off + 3] = static_cast<uint8_t>(u & 0xFF);
        };
        writeI32BE(14, 0);
        writeI32BE(18, 8192);  // +1.0 g at acceIFsr=1 (8192 LSB/g)
        writeI32BE(22, 0);
        writeI32BE(26, 0);
        writeI32BE(30, 0);
        writeI32BE(34, 66);    // ~1.007 deg/s at gyroIFsr=1 (65.536 LSB/dps)
        hw_imu[43] = 1;  // +-4g
        hw_imu[44] = 1;  // +-500dps

        core::ImuMeasurement out_imu;
        if (!decoder.decodeImuPacket(hw_imu.data(), hw_imu.size(), out_imu)) return false;
        if (std::abs(out_imu.accel_mps2.y - 9.80665) > 0.02) return false;
        const double expected_gz = (66.0 / 65.536) * math::kDegToRad;
        if (std::abs(out_imu.gyro_rads.z - expected_gz) > 1e-4) return false;
    }

    // Malformed packet rejection test
    msop_bytes[0] = 0x00;
    if (decoder.decodeMsopPacket(msop_bytes.data(), msop_bytes.size(), pts, sweep_done, completed_frame)) return false;
    if (decoder.getStats().msop_packets_malformed != 1) return false;

    return true;
}
