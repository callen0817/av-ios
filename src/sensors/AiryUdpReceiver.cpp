#include "avscan/sensors/AiryUdpReceiver.hpp"
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace avscan {
namespace sensors {

RawSensorRecorder::~RawSensorRecorder() {
    close();
}

uint32_t RawSensorRecorder::computeCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool RawSensorRecorder::openForWrite(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_stream_.is_open()) {
        out_stream_.close();
    }
    out_stream_.open(file_path, std::ios::binary | std::ios::trunc);
    if (!out_stream_.is_open()) {
        return false;
    }
    uint32_t magic = kFileMagic;
    uint32_t version = kFileVersion;
    out_stream_.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out_stream_.write(reinterpret_cast<const char*>(&version), sizeof(version));
    records_written_ = 0;
    bytes_written_ = sizeof(magic) + sizeof(version);
    return out_stream_.good();
}

void RawSensorRecorder::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_stream_.is_open()) {
        out_stream_.flush();
        out_stream_.close();
    }
}

bool RawSensorRecorder::writeRecord(RawRecordType type, double host_time_sec, const uint8_t* payload, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_stream_.is_open() || !payload || len == 0) {
        return false;
    }
    uint8_t t = static_cast<uint8_t>(type);
    uint32_t length32 = static_cast<uint32_t>(len);
    uint32_t crc = computeCrc32(payload, len);

    out_stream_.write(reinterpret_cast<const char*>(&t), sizeof(t));
    out_stream_.write(reinterpret_cast<const char*>(&host_time_sec), sizeof(host_time_sec));
    out_stream_.write(reinterpret_cast<const char*>(&length32), sizeof(length32));
    out_stream_.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
    out_stream_.write(reinterpret_cast<const char*>(payload), static_cast<std::streamsize>(len));

    if (!out_stream_.good()) {
        return false;
    }
    records_written_++;
    bytes_written_ += sizeof(t) + sizeof(host_time_sec) + sizeof(length32) + sizeof(crc) + len;
    return true;
}

bool RawSensorRecorder::readAllRecords(const std::string& file_path, std::vector<RawRecordChunk>& out_records) {
    out_records.clear();
    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) return false;

    uint32_t magic = 0, version = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!in.good() || magic != kFileMagic || version != kFileVersion) {
        return false;
    }

    while (in.peek() != EOF) {
        uint8_t t = 0;
        double host_time = 0.0;
        uint32_t length32 = 0;
        uint32_t expected_crc = 0;

        in.read(reinterpret_cast<char*>(&t), sizeof(t));
        if (in.eof()) break;
        in.read(reinterpret_cast<char*>(&host_time), sizeof(host_time));
        in.read(reinterpret_cast<char*>(&length32), sizeof(length32));
        in.read(reinterpret_cast<char*>(&expected_crc), sizeof(expected_crc));
        if (!in.good() || length32 == 0 || length32 > 16 * 1024 * 1024) {
            return false;
        }

        RawRecordChunk chunk;
        chunk.type = static_cast<RawRecordType>(t);
        chunk.host_receive_time_sec = host_time;
        chunk.payload.resize(length32);
        in.read(reinterpret_cast<char*>(chunk.payload.data()), static_cast<std::streamsize>(length32));
        if (!in.good()) return false;

        uint32_t actual_crc = computeCrc32(chunk.payload.data(), chunk.payload.size());
        if (actual_crc != expected_crc) {
            return false;
        }
        out_records.push_back(std::move(chunk));
    }
    return true;
}

AiryUdpReceiver::AiryUdpReceiver(const AiryNetworkConfig& config) : config_(config) {}

AiryUdpReceiver::~AiryUdpReceiver() {
    stop();
}

void AiryUdpReceiver::setCallbacks(FrameCallback on_frame, ImuCallback on_imu, DifopCallback on_difop) {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    on_frame_ = std::move(on_frame);
    on_imu_ = std::move(on_imu);
    on_difop_ = std::move(on_difop);
}

bool AiryUdpReceiver::start() {
    if (running_.load()) return true;
    running_.store(true);
    worker_thread_ = std::thread(&AiryUdpReceiver::receiveLoop, this);
    return true;
}

void AiryUdpReceiver::stop() {
    if (!running_.exchange(false)) return;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    recorder_.close();
}

bool AiryUdpReceiver::startRawRecording(const std::string& raw_file_path) {
    return recorder_.openForWrite(raw_file_path);
}

void AiryUdpReceiver::stopRawRecording() {
    recorder_.close();
}

AiryPacketStats AiryUdpReceiver::getPacketStats() const {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    return decoder_.getStats();
}

core::AiryFactoryCalibration AiryUdpReceiver::getCalibration() const {
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    return decoder_.getCalibration();
}

bool AiryUdpReceiver::replayRawFile(const std::string& raw_file_path,
                                    size_t& out_frames_decoded,
                                    size_t& out_imus_decoded) {
    out_frames_decoded = 0;
    out_imus_decoded = 0;
    std::vector<RawRecordChunk> records;
    if (!RawSensorRecorder::readAllRecords(raw_file_path, records)) {
        return false;
    }

    for (const auto& rec : records) {
        if (rec.type == RawRecordType::DIFOP_PACKET) {
            core::AiryFactoryCalibration cal;
            bool ok = false;
            DifopCallback cb;
            {
                std::lock_guard<std::mutex> lock(decoder_mutex_);
                ok = decoder_.decodeDifopPacket(rec.payload.data(), rec.payload.size(), cal);
                cb = on_difop_;
            }
            if (ok && cb) cb(cal);
        } else if (rec.type == RawRecordType::IMU_PACKET) {
            core::ImuMeasurement imu;
            bool ok = false;
            ImuCallback cb;
            {
                std::lock_guard<std::mutex> lock(decoder_mutex_);
                ok = decoder_.decodeImuPacket(rec.payload.data(), rec.payload.size(), imu);
                cb = on_imu_;
            }
            if (ok) {
                out_imus_decoded++;
                if (cb) cb(imu);
            }
        } else if (rec.type == RawRecordType::MSOP_PACKET) {
            std::vector<core::LidarPointXYZIRT> pkt_pts;
            bool completed = false;
            core::LidarFrame frame;
            bool ok = false;
            FrameCallback cb;
            {
                std::lock_guard<std::mutex> lock(decoder_mutex_);
                ok = decoder_.decodeMsopPacket(rec.payload.data(), rec.payload.size(), pkt_pts, completed, frame);
                cb = on_frame_;
            }
            if (ok && completed) {
                out_frames_decoded++;
                if (cb) cb(frame);
            }
        }
    }
    return true;
}

void AiryUdpReceiver::receiveLoop() {
#if defined(_WIN32)
    using SockType = SOCKET;
    constexpr SockType kInvalidSock = INVALID_SOCKET;
#else
    using SockType = int;
    constexpr SockType kInvalidSock = -1;
#endif

    auto openUdpSocket = [](uint16_t port) -> SockType {
        SockType s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kInvalidSock) return kInvalidSock;
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        int rcvbuf = 4 * 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#if defined(_WIN32)
            closesocket(s);
#else
            close(s);
#endif
            return kInvalidSock;
        }
        return s;
    };

    SockType msop_sock = openUdpSocket(config_.msop_port);
    SockType difop_sock = openUdpSocket(config_.difop_port);
    SockType imu_sock = openUdpSocket(config_.imu_port);

    uint8_t buf[2048]{};

    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        SockType max_fd = 0;
        if (msop_sock != kInvalidSock) { FD_SET(msop_sock, &rfds); if (msop_sock > max_fd) max_fd = msop_sock; }
        if (difop_sock != kInvalidSock) { FD_SET(difop_sock, &rfds); if (difop_sock > max_fd) max_fd = difop_sock; }
        if (imu_sock != kInvalidSock) { FD_SET(imu_sock, &rfds); if (imu_sock > max_fd) max_fd = imu_sock; }

        if (max_fd == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 20000;  // 20 ms poll timeout
        int ready = select(static_cast<int>(max_fd + 1), &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        double now_sec = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (msop_sock != kInvalidSock && FD_ISSET(msop_sock, &rfds)) {
            int n = recvfrom(msop_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
            if (n > 0) {
                if (recorder_.isOpen()) {
                    recorder_.writeRecord(RawRecordType::MSOP_PACKET, now_sec, buf, static_cast<size_t>(n));
                }
                std::vector<core::LidarPointXYZIRT> pkt_pts;
                bool sweep_done = false;
                core::LidarFrame completed_frame;
                FrameCallback cb;
                {
                    std::lock_guard<std::mutex> lock(decoder_mutex_);
                    decoder_.decodeMsopPacket(buf, static_cast<size_t>(n), pkt_pts, sweep_done, completed_frame);
                    cb = on_frame_;
                }
                if (sweep_done && cb) {
                    cb(completed_frame);
                }
            }
        }

        if (difop_sock != kInvalidSock && FD_ISSET(difop_sock, &rfds)) {
            int n = recvfrom(difop_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
            if (n > 0) {
                if (recorder_.isOpen()) {
                    recorder_.writeRecord(RawRecordType::DIFOP_PACKET, now_sec, buf, static_cast<size_t>(n));
                }
                core::AiryFactoryCalibration cal;
                bool ok = false;
                DifopCallback cb;
                {
                    std::lock_guard<std::mutex> lock(decoder_mutex_);
                    ok = decoder_.decodeDifopPacket(buf, static_cast<size_t>(n), cal);
                    cb = on_difop_;
                }
                if (ok && cb) {
                    cb(cal);
                }
            }
        }

        if (imu_sock != kInvalidSock && FD_ISSET(imu_sock, &rfds)) {
            int n = recvfrom(imu_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
            if (n > 0) {
                if (recorder_.isOpen()) {
                    recorder_.writeRecord(RawRecordType::IMU_PACKET, now_sec, buf, static_cast<size_t>(n));
                }
                core::ImuMeasurement imu;
                bool ok = false;
                ImuCallback cb;
                {
                    std::lock_guard<std::mutex> lock(decoder_mutex_);
                    ok = decoder_.decodeImuPacket(buf, static_cast<size_t>(n), imu);
                    cb = on_imu_;
                }
                if (ok && cb) {
                    cb(imu);
                }
            }
        }
    }

#if defined(_WIN32)
    if (msop_sock != kInvalidSock) closesocket(msop_sock);
    if (difop_sock != kInvalidSock) closesocket(difop_sock);
    if (imu_sock != kInvalidSock) closesocket(imu_sock);
#else
    if (msop_sock != kInvalidSock) close(msop_sock);
    if (difop_sock != kInvalidSock) close(difop_sock);
    if (imu_sock != kInvalidSock) close(imu_sock);
#endif
}

}  // namespace sensors
}  // namespace avscan
