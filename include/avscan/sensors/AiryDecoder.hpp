#pragma once

#include "avscan/core/Types.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace avscan {
namespace sensors {

struct AiryPacketStats {
    uint64_t msop_packets_valid{0};
    uint64_t msop_packets_malformed{0};
    uint64_t msop_sequence_gaps{0};
    uint64_t msop_out_of_order{0};
    uint64_t difop_packets_valid{0};
    uint64_t difop_packets_malformed{0};
    uint64_t imu_packets_valid{0};
    uint64_t imu_packets_malformed{0};
    uint64_t imu_sequence_gaps{0};
    uint64_t total_bytes_received{0};
    uint32_t last_msop_sequence{0};
    uint32_t last_imu_sequence{0};
    bool has_first_msop{false};
    bool has_first_imu{false};

    double msopPacketLossRatio() const {
        uint64_t total_expected = msop_packets_valid + msop_sequence_gaps;
        return (total_expected > 0) ? (static_cast<double>(msop_sequence_gaps) / static_cast<double>(total_expected)) : 0.0;
    }
};

class AiryDecoder {
public:
    static constexpr size_t kMsopPacketSize = 1248;
    static constexpr size_t kDifopPacketSize = 1248;
    static constexpr size_t kImuPacketSize = 51;

    static constexpr size_t kMsopHeaderSize = 42;
    static constexpr size_t kMsopBlocksPerPacket = 8;
    static constexpr size_t kMsopBlockSize = 148;
    static constexpr size_t kChannelsPerBlock = 48;
    static constexpr size_t kTotalBeams = 96;

    static constexpr double kDistanceResolutionMeters = 0.005;  // 5 mm per LSB (lower 14 bits)
    static constexpr double kAzimuthResolutionDeg = 0.01;       // 0.01 deg per LSB
    static constexpr double kBlockIntervalSec = 55.555e-6;      // 55.55 us per firing group
    static constexpr double kChannelIntervalSec = 1.157e-6;     // 1.157 us per channel firing
    static constexpr double kMinValidRangeMeters = 0.10;
    static constexpr double kMaxValidRangeMeters = 60.0;

    AiryDecoder();

    // Sets or updates calibration lookup tables (from DIFOP or default hemispherical 0..90 deg profile)
    void setCalibration(const core::AiryFactoryCalibration& calib);
    const core::AiryFactoryCalibration& getCalibration() const { return calib_; }

    // Decodes a 1248-byte MSOP UDP packet and appends valid 3D points to current sweep or emits a completed 360-deg frame
    bool decodeMsopPacket(const uint8_t* data, size_t len,
                          std::vector<core::LidarPointXYZIRT>& out_packet_points,
                          bool& out_sweep_completed,
                          core::LidarFrame& out_completed_frame);

    // Decodes a 1248-byte DIFOP UDP packet into AiryFactoryCalibration and updates internal angle lookup tables
    bool decodeDifopPacket(const uint8_t* data, size_t len,
                           core::AiryFactoryCalibration& out_calib);

    // Decodes a 51-byte IMU UDP packet into ImuMeasurement
    bool decodeImuPacket(const uint8_t* data, size_t len,
                         core::ImuMeasurement& out_imu);

    const AiryPacketStats& getStats() const { return stats_; }
    void resetStats() { stats_ = AiryPacketStats{}; }

    // Deterministic packet builders for protocol verification & recorded dataset synthesis
    static void buildDifopPacket(const core::AiryFactoryCalibration& calib,
                                 std::array<uint8_t, kDifopPacketSize>& out_pkt);

    static void buildImuPacket(const core::ImuMeasurement& imu,
                               std::array<uint8_t, kImuPacketSize>& out_pkt);

private:
    void rebuildTrigonometricTables();

    core::AiryFactoryCalibration calib_{};
    AiryPacketStats stats_{};

    std::array<double, kTotalBeams> cos_vert_rad_{};
    std::array<double, kTotalBeams> sin_vert_rad_{};
    std::array<double, kTotalBeams> hor_offset_deg_{};

    double last_azimuth_deg_{-1.0};
    uint64_t next_frame_id_{1};
    core::LidarFrame accumulating_frame_{};
};

}  // namespace sensors
}  // namespace avscan
