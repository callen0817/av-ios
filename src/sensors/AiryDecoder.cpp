#include "avscan/sensors/AiryDecoder.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace avscan {
namespace sensors {

namespace {

inline uint16_t readUint16BE(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t readUint32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t readUint48BE(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 40) |
           (static_cast<uint64_t>(p[1]) << 32) |
           (static_cast<uint64_t>(p[2]) << 24) |
           (static_cast<uint64_t>(p[3]) << 16) |
           (static_cast<uint64_t>(p[4]) << 8) |
           static_cast<uint64_t>(p[5]);
}

inline void writeUint16BE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

inline void writeUint32BE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

inline void writeUint48BE(uint8_t* p, uint64_t v) {
    p[0] = static_cast<uint8_t>((v >> 40) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 32) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[4] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[5] = static_cast<uint8_t>(v & 0xFF);
}

inline float readFloatLE(const uint8_t* p) {
    uint32_t u = static_cast<uint32_t>(p[0]) |
                 (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) |
                 (static_cast<uint32_t>(p[3]) << 24);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(float));
    return f;
}

inline float readFloatBE(const uint8_t* p) {
    uint32_t u = (static_cast<uint32_t>(p[0]) << 24) |
                 (static_cast<uint32_t>(p[1]) << 16) |
                 (static_cast<uint32_t>(p[2]) << 8) |
                 static_cast<uint32_t>(p[3]);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(float));
    return f;
}

inline int32_t readInt32BE(const uint8_t* p) {
    return static_cast<int32_t>(readUint32BE(p));
}

inline void writeFloatLE(uint8_t* p, float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(float));
    p[0] = static_cast<uint8_t>(u & 0xFF);
    p[1] = static_cast<uint8_t>((u >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((u >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((u >> 24) & 0xFF);
}

inline double parseTimestamp10Bytes(const uint8_t* p) {
    uint64_t sec = readUint48BE(p);
    uint32_t subsec = readUint32BE(p + 6);
    // RoboSense 10-byte UTC timestamp stores microseconds when < 1e6, or nanoseconds in PTP mode
    double frac = (subsec >= 1000000u) ? (static_cast<double>(subsec) * 1e-9)
                                       : (static_cast<double>(subsec) * 1e-6);
    return static_cast<double>(sec) + frac;
}

inline void writeTimestamp10Bytes(uint8_t* p, double timestamp_sec) {
    if (timestamp_sec < 0.0) timestamp_sec = 0.0;
    uint64_t sec = static_cast<uint64_t>(std::floor(timestamp_sec));
    uint32_t usec = static_cast<uint32_t>(std::round((timestamp_sec - static_cast<double>(sec)) * 1e6));
    if (usec >= 1000000u) {
        sec += 1;
        usec -= 1000000u;
    }
    writeUint48BE(p, sec);
    writeUint32BE(p + 6, usec);
}

}  // namespace

AiryDecoder::AiryDecoder() {
    // Initialize with real factory calibration extracted from RoboSense Airy unit (MAC 08:48:57:09:82:89)
    static const double kFactoryVertDeg[kTotalBeams] = {
        -0.48,  0.43,  1.35,  2.39,  3.34,  4.33,  5.35,  6.27,  7.33,  8.20,  9.23, 10.14,
        11.10, 12.07, 13.02, 13.95, 14.90, 15.82, 16.79, 17.76, 18.71, 19.64, 20.58, 21.52,
        22.42, 23.34, 24.29, 25.22, 26.13, 27.05, 27.98, 28.90, 29.85, 30.74, 31.67, 32.58,
        33.55, 34.49, 35.37, 36.33, 37.25, 38.17, 39.09, 40.02, 40.95, 41.86, 42.77, 43.69,
        44.53, 45.41, 46.33, 47.29, 48.20, 49.09, 50.01, 50.94, 51.93, 52.76, 53.65, 54.58,
        55.64, 56.51, 57.42, 58.34, 59.33, 60.20, 61.12, 62.02, 63.09, 63.86, 64.81, 65.73,
        66.62, 67.48, 68.45, 69.37, 70.40, 71.24, 72.19, 73.13, 74.20, 75.02, 76.01, 76.93,
        78.07, 78.88, 79.87, 80.76, 81.93, 82.72, 83.73, 84.64, 85.81, 86.60, 87.62, 88.58
    };
    static const double kFactoryHorDeg[kTotalBeams] = {
        -1.64, -1.67, -1.69, -1.72, -1.75, -1.78, -1.80, -1.82, -1.84, -1.87, -1.90, -1.92,
         1.30,  1.30,  1.32,  1.33,  1.33,  1.34,  1.35,  1.37,  1.37,  1.39,  1.40,  1.41,
        -1.82, -1.84, -1.86, -1.88, -1.90, -1.93, -1.94, -1.97, -1.99, -2.02, -2.03, -2.06,
         1.59,  1.60,  1.62,  1.64,  1.65,  1.67,  1.68,  1.70,  1.73,  1.75,  1.78,  1.80,
        -2.47, -2.50, -2.52, -2.56, -2.58, -2.60, -2.63, -2.65, -2.68, -2.81, -2.94, -2.97,
         2.14,  2.18,  2.22,  2.26,  2.31,  2.34,  2.40,  2.45,  2.51,  2.58,  2.64,  2.72,
        -5.05, -5.61, -5.79, -5.99, -6.21, -6.46, -6.72, -7.02, -7.36, -7.76, -8.18, -8.70,
         4.73,  5.10,  5.53,  6.05,  6.72,  7.58,  8.72, 10.38, 12.93, 15.49, 23.53, 35.04
    };
    for (size_t i = 0; i < kTotalBeams; ++i) {
        calib_.vertical_angles_deg[i] = kFactoryVertDeg[i];
        calib_.horizontal_angles_deg[i] = kFactoryHorDeg[i];
    }
    calib_.mac_address = "08:48:57:09:82:89";
    math::Quatd q_factory(-0.702718, 0.001879, 0.004094, 0.711454);
    q_factory.normalize();
    calib_.T_imu_lidar = math::SE3d(q_factory, math::Vec3d(0.00425, 0.00418, -0.00446));
    rebuildTrigonometricTables();
}

void AiryDecoder::setCalibration(const core::AiryFactoryCalibration& calib) {
    calib_ = calib;
    rebuildTrigonometricTables();
}

void AiryDecoder::rebuildTrigonometricTables() {
    for (size_t i = 0; i < kTotalBeams; ++i) {
        double v_rad = calib_.vertical_angles_deg[i] * math::kDegToRad;
        cos_vert_rad_[i] = std::cos(v_rad);
        sin_vert_rad_[i] = std::sin(v_rad);
        hor_offset_deg_[i] = calib_.horizontal_angles_deg[i];
    }
}

bool AiryDecoder::decodeMsopPacket(const uint8_t* data, size_t len,
                                   std::vector<core::LidarPointXYZIRT>& out_packet_points,
                                   bool& out_sweep_completed,
                                   core::LidarFrame& out_completed_frame) {
    out_packet_points.clear();
    out_sweep_completed = false;
    stats_.total_bytes_received += len;

    if (!data || len != kMsopPacketSize) {
        stats_.msop_packets_malformed++;
        return false;
    }

    // Verify 4-byte MSOP Header ID: 0x55 0xAA 0x05 0x5A
    if (data[0] != 0x55 || data[1] != 0xAA || data[2] != 0x05 || data[3] != 0x5A) {
        stats_.msop_packets_malformed++;
        return false;
    }

    // Verify 2-byte MSOP Tail: 0x00 0xFF at bytes 1230..1231 (real Airy firmware) or 1246..1247
    bool valid_msop_tail = (data[1230] == 0x00 && data[1231] == 0xFF) ||
                           (data[1246] == 0x00 && data[1247] == 0xFF);
    if (!valid_msop_tail) {
        stats_.msop_packets_malformed++;
        return false;
    }

    uint32_t pkt_seq = readUint32BE(data + 12);
    if (stats_.has_first_msop) {
        uint32_t expected = (stats_.last_msop_sequence + 1) & 0xFFFFu;
        uint32_t curr16 = pkt_seq & 0xFFFFu;
        if (curr16 != expected) {
            uint32_t diff = (curr16 + 0x10000u - expected) & 0xFFFFu;
            if (diff < 32768u) {
                stats_.msop_sequence_gaps += diff;
            } else {
                stats_.msop_out_of_order++;
            }
        }
    }
    stats_.last_msop_sequence = pkt_seq & 0xFFFFu;
    stats_.has_first_msop = true;

    double pkt_timestamp_sec = parseTimestamp10Bytes(data + 20);
    if (accumulating_frame_.points.empty()) {
        accumulating_frame_.frame_id = next_frame_id_;
        accumulating_frame_.timestamp_start_sec = pkt_timestamp_sec;
    }

    out_packet_points.reserve(kMsopBlocksPerPacket * kChannelsPerBlock);

    // Parse 8 Data Blocks (148 bytes each, starting at byte 42)
    for (size_t b = 0; b < kMsopBlocksPerPacket; ++b) {
        const uint8_t* block_ptr = data + kMsopHeaderSize + b * kMsopBlockSize;

        // Validate 2-byte Block Flag: 0xFF 0xEE
        if (block_ptr[0] != 0xFF || block_ptr[1] != 0xEE) {
            stats_.msop_packets_malformed++;
            return false;
        }

        uint16_t raw_azimuth = readUint16BE(block_ptr + 2);
        if (raw_azimuth >= 36000u) {
            continue;
        }
        double block_azimuth_deg = static_cast<double>(raw_azimuth) * kAzimuthResolutionDeg;

        // Detect 360-degree sweep wrap-around on even block boundaries
        if ((b % 2 == 0) && last_azimuth_deg_ >= 0.0) {
            if (block_azimuth_deg < last_azimuth_deg_ && (last_azimuth_deg_ - block_azimuth_deg) > 180.0) {
                if (!accumulating_frame_.points.empty()) {
                    accumulating_frame_.timestamp_end_sec = pkt_timestamp_sec + (b / 2) * kBlockIntervalSec;
                    out_completed_frame = std::move(accumulating_frame_);
                    out_sweep_completed = true;
                    next_frame_id_++;
                    accumulating_frame_ = core::LidarFrame{};
                    accumulating_frame_.frame_id = next_frame_id_;
                    accumulating_frame_.timestamp_start_sec = out_completed_frame.timestamp_end_sec;
                }
            }
        }
        if (b % 2 == 0) {
            last_azimuth_deg_ = block_azimuth_deg;
        }

        size_t channel_group_offset = (b % 2 == 0) ? 0 : kChannelsPerBlock;
        double block_time_offset = static_cast<double>(b / 2) * kBlockIntervalSec;

        const uint8_t* channels_ptr = block_ptr + 4;
        for (size_t ch = 0; ch < kChannelsPerBlock; ++ch) {
            const uint8_t* ch_ptr = channels_ptr + ch * 3;
            uint16_t raw_dist_word = readUint16BE(ch_ptr);
            uint8_t reflectivity = ch_ptr[2];

            uint16_t dist_counts = raw_dist_word & 0x3FFFu;
            uint8_t status_flags = static_cast<uint8_t>((raw_dist_word >> 14) & 0x03u);
            if (dist_counts == 0) continue;

            double range_m = static_cast<double>(dist_counts) * kDistanceResolutionMeters;
            if (range_m < kMinValidRangeMeters || range_m > kMaxValidRangeMeters) {
                continue;
            }

            size_t ring_idx = channel_group_offset + ch;
            double az_deg = block_azimuth_deg + hor_offset_deg_[ring_idx];
            if (az_deg >= 360.0) az_deg -= 360.0;
            if (az_deg < 0.0) az_deg += 360.0;
            double az_rad = az_deg * math::kDegToRad;

            double xy_proj = range_m * cos_vert_rad_[ring_idx];
            core::LidarPointXYZIRT pt;
            // RoboSense right-handed coordinate frame: X forward/right, Y forward, Z up
            pt.x = xy_proj * std::cos(az_rad);
            pt.y = xy_proj * std::sin(az_rad);
            pt.z = range_m * sin_vert_rad_[ring_idx];
            pt.intensity = static_cast<float>(reflectivity);
            pt.ring = static_cast<uint16_t>(ring_idx);
            pt.timestamp_sec = pkt_timestamp_sec + block_time_offset + static_cast<double>(ch) * kChannelIntervalSec;
            pt.range_m = static_cast<float>(range_m);
            pt.azimuth_deg = static_cast<float>(az_deg);
            pt.elevation_deg = static_cast<float>(calib_.vertical_angles_deg[ring_idx]);
            pt.status_flags = status_flags;

            out_packet_points.push_back(pt);
            accumulating_frame_.points.push_back(pt);
        }
    }

    accumulating_frame_.timestamp_end_sec = pkt_timestamp_sec + 4.0 * kBlockIntervalSec;
    stats_.msop_packets_valid++;
    return true;
}

bool AiryDecoder::decodeDifopPacket(const uint8_t* data, size_t len,
                                    core::AiryFactoryCalibration& out_calib) {
    stats_.total_bytes_received += len;
    if (!data || len != kDifopPacketSize) {
        stats_.difop_packets_malformed++;
        return false;
    }

    // Verify DIFOP header magic bytes (0xA5 0xFF 0x00 0x5A or 0x5A 0xFF 0x55 0xAA)
    bool valid_header =
        (data[0] == 0xA5 && data[1] == 0xFF && data[2] == 0x00 && data[3] == 0x5A) ||
        (data[0] == 0x5A && data[1] == 0xFF && data[2] == 0x55 && data[3] == 0xAA);
    bool valid_tail = (data[1246] == 0x0F && data[1247] == 0xF0);

    if (!valid_header || !valid_tail) {
        stats_.difop_packets_malformed++;
        return false;
    }

    char buf[64]{};
    // Detect real Airy hardware DIFOP layout (bytes 6..7 == 0x55 0x55 and motor RPM at 8..9) vs synthetic layout
    const bool is_hw_difop = (data[6] == 0x55 && data[7] == 0x55 && readUint16BE(data + 24) >= 1024);

    if (is_hw_difop) {
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[38], data[39], data[40], data[41]);
        out_calib.mainboard_firmware = buf;
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[44], data[45], data[46], data[47]);
        out_calib.baseboard_firmware = buf;
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[52], data[53], data[54], data[55]);
        out_calib.motor_firmware = buf;

        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[10], data[11], data[12], data[13]);
        out_calib.sensor_ip = buf;
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[14], data[15], data[16], data[17]);
        out_calib.dest_ip = buf;
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      data[18], data[19], data[20], data[21], data[22], data[23]);
        out_calib.mac_address = buf;

        out_calib.msop_port = readUint16BE(data + 24);
        out_calib.difop_port = readUint16BE(data + 28);
        uint16_t hw_imu_port = readUint16BE(data + 359);
        out_calib.imu_port = (hw_imu_port >= 1024) ? hw_imu_port : 6688;
        out_calib.return_mode = data[300];

        // Parse COR_VERT_ANG (288 bytes at offset 468: 96 channels * 3 bytes)
        for (size_t i = 0; i < kTotalBeams; ++i) {
            const uint8_t* vp = data + 468 + i * 3;
            double sign = (vp[0] != 0x00) ? -1.0 : 1.0;
            uint16_t mag = readUint16BE(vp + 1);
            out_calib.vertical_angles_deg[i] = sign * static_cast<double>(mag) * 0.01;
        }
        // Parse COR_HOR_ANG (288 bytes at offset 756: 96 channels * 3 bytes)
        for (size_t i = 0; i < kTotalBeams; ++i) {
            const uint8_t* hp = data + 756 + i * 3;
            double sign = (hp[0] != 0x00) ? -1.0 : 1.0;
            uint16_t mag = readUint16BE(hp + 1);
            out_calib.horizontal_angles_deg[i] = sign * static_cast<double>(mag) * 0.01;
        }
        // Parse IMU_CALIB_DATA (28 bytes at offset 1092: qw, qx, qy, qz, tx, ty, tz as big-endian floats)
        const uint8_t* imu_cal_ptr = data + 1092;
        float qw = readFloatBE(imu_cal_ptr + 0);
        float qx = readFloatBE(imu_cal_ptr + 4);
        float qy = readFloatBE(imu_cal_ptr + 8);
        float qz = readFloatBE(imu_cal_ptr + 12);
        float tx = readFloatBE(imu_cal_ptr + 16);
        float ty = readFloatBE(imu_cal_ptr + 20);
        float tz = readFloatBE(imu_cal_ptr + 24);
        double qnorm = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (std::isfinite(qnorm) && qnorm > 0.1 && qnorm < 10.0) {
            math::Quatd q(qx, qy, qz, qw);
            q.normalize();
            out_calib.T_imu_lidar = math::SE3d(q, math::Vec3d(tx, ty, tz));
        } else {
            out_calib.T_imu_lidar = math::SE3d::Identity();
        }
    } else {
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[6], data[7], data[8], data[9]);
        out_calib.mainboard_firmware = buf;

        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[10], data[11], data[12], data[13]);
        out_calib.baseboard_firmware = buf;

        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[14], data[15], data[16], data[17]);
        out_calib.motor_firmware = buf;

        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[18], data[19], data[20], data[21]);
        out_calib.sensor_ip = buf;

        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", data[22], data[23], data[24], data[25]);
        out_calib.dest_ip = buf;

        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      data[26], data[27], data[28], data[29], data[30], data[31]);
        out_calib.mac_address = buf;

        out_calib.msop_port = readUint16BE(data + 32);
        out_calib.difop_port = readUint16BE(data + 34);
        out_calib.imu_port = readUint16BE(data + 36);
        out_calib.return_mode = data[300];

        // Parse COR_VERT_ANG (288 bytes at offset 304: 96 channels * 3 bytes)
        bool has_nonzero_vert = false;
        for (size_t i = 0; i < kTotalBeams; ++i) {
            const uint8_t* vp = data + 304 + i * 3;
            double sign = (vp[0] == 0x01) ? -1.0 : 1.0;
            uint16_t mag = readUint16BE(vp + 1);
            if (mag > 0) has_nonzero_vert = true;
            out_calib.vertical_angles_deg[i] = sign * static_cast<double>(mag) * 0.01;
        }
        if (!has_nonzero_vert) {
            for (size_t i = 0; i < kTotalBeams; ++i) {
                out_calib.vertical_angles_deg[i] = static_cast<double>(i) * (90.0 / static_cast<double>(kTotalBeams));
            }
        }

        // Parse COR_HOR_ANG (288 bytes at offset 592: 96 channels * 3 bytes)
        for (size_t i = 0; i < kTotalBeams; ++i) {
            const uint8_t* hp = data + 592 + i * 3;
            double sign = (hp[0] == 0x01) ? -1.0 : 1.0;
            uint16_t mag = readUint16BE(hp + 1);
            out_calib.horizontal_angles_deg[i] = sign * static_cast<double>(mag) * 0.01;
        }

        // Parse IMU_CALIB_DATA (28 bytes at offset 880: qx, qy, qz, qw, x, y, z as 32-bit IEEE floats)
        const uint8_t* imu_cal_ptr = data + 880;
        float qx = readFloatLE(imu_cal_ptr + 0);
        float qy = readFloatLE(imu_cal_ptr + 4);
        float qz = readFloatLE(imu_cal_ptr + 8);
        float qw = readFloatLE(imu_cal_ptr + 12);
        float tx = readFloatLE(imu_cal_ptr + 16);
        float ty = readFloatLE(imu_cal_ptr + 20);
        float tz = readFloatLE(imu_cal_ptr + 24);

        double qnorm = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (std::isfinite(qnorm) && qnorm > 0.1 && qnorm < 10.0) {
            math::Quatd q(qx, qy, qz, qw);
            q.normalize();
            out_calib.T_imu_lidar = math::SE3d(q, math::Vec3d(tx, ty, tz));
        } else {
            out_calib.T_imu_lidar = math::SE3d::Identity();
        }
    }

    out_calib.valid = true;
    setCalibration(out_calib);
    stats_.difop_packets_valid++;
    return true;
}

bool AiryDecoder::decodeImuPacket(const uint8_t* data, size_t len,
                                  core::ImuMeasurement& out_imu) {
    stats_.total_bytes_received += len;
    if (!data || len != kImuPacketSize) {
        stats_.imu_packets_malformed++;
        return false;
    }

    // Support both real Airy hardware IMU header (0xAA 0x55 0x5A 0x05, tail 0xF0 0x0F)
    // and synthetic IEEE float IMU header (0x5A 0xFF 0x55 0xAA, tail 0x0F 0xF0)
    const bool is_hw_imu = (data[0] == 0xAA && data[1] == 0x55 && data[2] == 0x5A && data[3] == 0x05 &&
                            data[49] == 0xF0 && data[50] == 0x0F);
    const bool is_synth_imu = (data[0] == 0x5A && data[1] == 0xFF && data[2] == 0x55 && data[3] == 0xAA);

    if (!is_hw_imu && !is_synth_imu) {
        stats_.imu_packets_malformed++;
        return false;
    }

    if (is_hw_imu) {
        out_imu.timestamp_sec = parseTimestamp10Bytes(data + 4);
        // Hardware registers: [-4g, +4g] -> 8192 LSB/g; [-500, +500] dps -> 65.536 LSB/dps
        constexpr double kAccelScaleMps2 = 9.80665 / 8192.0;
        constexpr double kGyroScaleRads = (1.0 / 65.536) * math::kDegToRad;
        out_imu.accel_mps2.x = static_cast<double>(readInt32BE(data + 14)) * kAccelScaleMps2;
        out_imu.accel_mps2.y = static_cast<double>(readInt32BE(data + 18)) * kAccelScaleMps2;
        out_imu.accel_mps2.z = static_cast<double>(readInt32BE(data + 22)) * kAccelScaleMps2;
        out_imu.gyro_rads.x = static_cast<double>(readInt32BE(data + 26)) * kGyroScaleRads;
        out_imu.gyro_rads.y = static_cast<double>(readInt32BE(data + 30)) * kGyroScaleRads;
        out_imu.gyro_rads.z = static_cast<double>(readInt32BE(data + 34)) * kGyroScaleRads;
        out_imu.temperature_c = static_cast<float>(readInt32BE(data + 38)) / 256.0f;
        out_imu.status_flags = data[42];
        out_imu.sequence_id = readUint32BE(data + 45);
    } else {
        out_imu.timestamp_sec = parseTimestamp10Bytes(data + 6);
        out_imu.accel_mps2.x = static_cast<double>(readFloatLE(data + 16));
        out_imu.accel_mps2.y = static_cast<double>(readFloatLE(data + 20));
        out_imu.accel_mps2.z = static_cast<double>(readFloatLE(data + 24));
        out_imu.gyro_rads.x = static_cast<double>(readFloatLE(data + 28));
        out_imu.gyro_rads.y = static_cast<double>(readFloatLE(data + 32));
        out_imu.gyro_rads.z = static_cast<double>(readFloatLE(data + 36));
        out_imu.temperature_c = readFloatLE(data + 40);
        out_imu.sequence_id = readUint32BE(data + 44);
        out_imu.status_flags = data[48];
    }

    if (!std::isfinite(out_imu.accel_mps2.x) || !std::isfinite(out_imu.gyro_rads.x)) {
        stats_.imu_packets_malformed++;
        return false;
    }

    if (stats_.has_first_imu) {
        if (out_imu.sequence_id > stats_.last_imu_sequence + 1) {
            stats_.imu_sequence_gaps += (out_imu.sequence_id - stats_.last_imu_sequence - 1);
        }
    }
    stats_.last_imu_sequence = out_imu.sequence_id;
    stats_.has_first_imu = true;
    stats_.imu_packets_valid++;
    return true;
}

void AiryDecoder::buildDifopPacket(const core::AiryFactoryCalibration& calib,
                                   std::array<uint8_t, kDifopPacketSize>& out_pkt) {
    out_pkt.fill(0);
    // Sync Header: 0xA5 0xFF 0x00 0x5A
    out_pkt[0] = 0xA5; out_pkt[1] = 0xFF; out_pkt[2] = 0x00; out_pkt[3] = 0x5A;
    out_pkt[4] = 0x11; out_pkt[5] = 0x11;

    // Firmware versions
    out_pkt[6] = 1; out_pkt[7] = 4; out_pkt[8] = 2; out_pkt[9] = 0;
    out_pkt[10] = 1; out_pkt[11] = 2; out_pkt[12] = 0; out_pkt[13] = 0;
    out_pkt[14] = 1; out_pkt[15] = 0; out_pkt[16] = 3; out_pkt[17] = 0;

    // Source IP 192.168.1.200 & Dest IP 192.168.1.102
    unsigned int s0 = 192, s1 = 168, s2 = 1, s3 = 200;
    std::sscanf(calib.sensor_ip.c_str(), "%u.%u.%u.%u", &s0, &s1, &s2, &s3);
    out_pkt[18] = static_cast<uint8_t>(s0);
    out_pkt[19] = static_cast<uint8_t>(s1);
    out_pkt[20] = static_cast<uint8_t>(s2);
    out_pkt[21] = static_cast<uint8_t>(s3);

    unsigned int d0 = 192, d1 = 168, d2 = 1, d3 = 102;
    std::sscanf(calib.dest_ip.c_str(), "%u.%u.%u.%u", &d0, &d1, &d2, &d3);
    out_pkt[22] = static_cast<uint8_t>(d0);
    out_pkt[23] = static_cast<uint8_t>(d1);
    out_pkt[24] = static_cast<uint8_t>(d2);
    out_pkt[25] = static_cast<uint8_t>(d3);

    out_pkt[26] = 0x00; out_pkt[27] = 0x1C; out_pkt[28] = 0x23;
    out_pkt[29] = 0xAA; out_pkt[30] = 0xBB; out_pkt[31] = 0xCC;

    writeUint16BE(out_pkt.data() + 32, calib.msop_port);
    writeUint16BE(out_pkt.data() + 34, calib.difop_port);
    writeUint16BE(out_pkt.data() + 36, calib.imu_port);
    out_pkt[300] = calib.return_mode;

    // COR_VERT_ANG (offset 304) & COR_HOR_ANG (offset 592)
    for (size_t i = 0; i < kTotalBeams; ++i) {
        double v = calib.vertical_angles_deg[i];
        uint8_t* vp = out_pkt.data() + 304 + i * 3;
        vp[0] = (v < 0.0) ? 0x01 : 0x00;
        writeUint16BE(vp + 1, static_cast<uint16_t>(std::round(std::abs(v) * 100.0)));

        double h = calib.horizontal_angles_deg[i];
        uint8_t* hp = out_pkt.data() + 592 + i * 3;
        hp[0] = (h < 0.0) ? 0x01 : 0x00;
        writeUint16BE(hp + 1, static_cast<uint16_t>(std::round(std::abs(h) * 100.0)));
    }

    // IMU_CALIB_DATA (offset 880)
    math::Quatd q = calib.T_imu_lidar.so3.unit_quaternion();
    const math::Vec3d& t = calib.T_imu_lidar.t;
    uint8_t* imu_cal = out_pkt.data() + 880;
    writeFloatLE(imu_cal + 0, static_cast<float>(q.x));
    writeFloatLE(imu_cal + 4, static_cast<float>(q.y));
    writeFloatLE(imu_cal + 8, static_cast<float>(q.z));
    writeFloatLE(imu_cal + 12, static_cast<float>(q.w));
    writeFloatLE(imu_cal + 16, static_cast<float>(t.x));
    writeFloatLE(imu_cal + 20, static_cast<float>(t.y));
    writeFloatLE(imu_cal + 24, static_cast<float>(t.z));

    // Tail: 0x0F 0xF0
    out_pkt[1246] = 0x0F;
    out_pkt[1247] = 0xF0;
}

void AiryDecoder::buildImuPacket(const core::ImuMeasurement& imu,
                                 std::array<uint8_t, kImuPacketSize>& out_pkt) {
    out_pkt.fill(0);
    out_pkt[0] = 0x5A; out_pkt[1] = 0xFF; out_pkt[2] = 0x55; out_pkt[3] = 0xAA;
    out_pkt[4] = 0x01; out_pkt[5] = 0x00;
    writeTimestamp10Bytes(out_pkt.data() + 6, imu.timestamp_sec);
    writeFloatLE(out_pkt.data() + 16, static_cast<float>(imu.accel_mps2.x));
    writeFloatLE(out_pkt.data() + 20, static_cast<float>(imu.accel_mps2.y));
    writeFloatLE(out_pkt.data() + 24, static_cast<float>(imu.accel_mps2.z));
    writeFloatLE(out_pkt.data() + 28, static_cast<float>(imu.gyro_rads.x));
    writeFloatLE(out_pkt.data() + 32, static_cast<float>(imu.gyro_rads.y));
    writeFloatLE(out_pkt.data() + 36, static_cast<float>(imu.gyro_rads.z));
    writeFloatLE(out_pkt.data() + 40, imu.temperature_c);
    writeUint32BE(out_pkt.data() + 44, imu.sequence_id);
    out_pkt[48] = imu.status_flags;
    out_pkt[49] = 0x0F;
    out_pkt[50] = 0xF0;
}

}  // namespace sensors
}  // namespace avscan
