#pragma once

#include "avscan/sensors/AiryDecoder.hpp"
#include "avscan/sensors/AiryDiscovery.hpp"
#include <atomic>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace avscan {
namespace sensors {

enum class RawRecordType : uint8_t {
    MSOP_PACKET = 1,
    DIFOP_PACKET = 2,
    IMU_PACKET = 3,
    RGB_FRAME_META = 4
};

struct RawRecordChunk {
    RawRecordType type{RawRecordType::MSOP_PACKET};
    double host_receive_time_sec{0.0};
    std::vector<uint8_t> payload;
};

class RawSensorRecorder {
public:
    static constexpr uint32_t kFileMagic = 0x41565257u;  // "AVRW"
    static constexpr uint32_t kFileVersion = 1u;

    RawSensorRecorder() = default;
    ~RawSensorRecorder();

    bool openForWrite(const std::string& file_path);
    void close();
    bool isOpen() const { return out_stream_.is_open(); }

    bool writeRecord(RawRecordType type, double host_time_sec, const uint8_t* payload, size_t len);
    uint64_t getRecordsWritten() const { return records_written_; }
    uint64_t getBytesWritten() const { return bytes_written_; }

    static uint32_t computeCrc32(const uint8_t* data, size_t len);
    static bool readAllRecords(const std::string& file_path, std::vector<RawRecordChunk>& out_records);

private:
    std::mutex mutex_;
    std::ofstream out_stream_;
    uint64_t records_written_{0};
    uint64_t bytes_written_{0};
};

class AiryUdpReceiver {
public:
    using FrameCallback = std::function<void(const core::LidarFrame&)>;
    using ImuCallback = std::function<void(const core::ImuMeasurement&)>;
    using DifopCallback = std::function<void(const core::AiryFactoryCalibration&)>;

    explicit AiryUdpReceiver(const AiryNetworkConfig& config = AiryNetworkConfig{});
    ~AiryUdpReceiver();

    void setCallbacks(FrameCallback on_frame, ImuCallback on_imu, DifopCallback on_difop);

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    bool startRawRecording(const std::string& raw_file_path);
    void stopRawRecording();

    // Replays a recorded .avraw dataset through the exact same packet decoder and callbacks
    bool replayRawFile(const std::string& raw_file_path, size_t& out_frames_decoded, size_t& out_imus_decoded);

    AiryPacketStats getPacketStats() const;
    core::AiryFactoryCalibration getCalibration() const;

private:
    void receiveLoop();

    AiryNetworkConfig config_;
    mutable std::mutex decoder_mutex_;
    AiryDecoder decoder_;
    RawSensorRecorder recorder_;

    FrameCallback on_frame_;
    ImuCallback on_imu_;
    DifopCallback on_difop_;

    std::atomic<bool> running_{false};
    std::thread worker_thread_;
};

}  // namespace sensors
}  // namespace avscan
