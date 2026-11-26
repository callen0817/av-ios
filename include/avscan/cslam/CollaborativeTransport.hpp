#pragma once

#include "avscan/cslam/CollaborativeSlamEngine.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace avscan {
namespace cslam {

// Portable cross-platform LAN Wi-Fi UDP discovery + TCP keyframe sync transport.
// Runs identically on iOS/iPadOS, NVIDIA Jetson (Linux), Windows, and macOS so
// AV Scan iOS, AV SLAM, AV Scanar, and AV Scan nodes interoperate seamlessly.
class CollaborativeLanTransport {
public:
    using PacketCallback = std::function<void(const uint8_t* data, size_t len, const std::string& sender_ip)>;

    CollaborativeLanTransport(uint16_t discovery_port = kCslamDiscoveryPort,
                              uint16_t data_port = kCslamDataPort);
    ~CollaborativeLanTransport();

    void setPacketCallback(PacketCallback cb);
    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    // Manually add a peer IP (e.g., Jetson / Windows PC / iPad on a hotspot where UDP broadcast is filtered)
    void addDirectPeerIp(const std::string& ip_address);
    void clearDirectPeers();

    // Broadcast a lightweight UDP beacon or pose heartbeat to LAN broadcast + all known peer IPs
    void broadcastUdpPacket(const std::vector<uint8_t>& payload);

    // Send a length-prefixed TCP payload (e.g., full KEYFRAME_SYNC) to all discovered + direct peer IPs
    void broadcastTcpPacket(const std::vector<uint8_t>& payload);

    uint16_t discoveryPort() const { return discovery_port_; }
    uint16_t dataPort() const { return data_port_; }

private:
    void udpReceiveLoop();
    void tcpAcceptLoop();
    void handleTcpClient(intptr_t client_sock, const std::string& sender_ip);
    void sendTcpToIp(const std::string& ip, uint16_t port, const std::vector<uint8_t>& payload);

    uint16_t discovery_port_;
    uint16_t data_port_;
    std::atomic<bool> running_{false};
    intptr_t udp_sock_{-1};
    intptr_t tcp_listen_sock_{-1};

    std::thread udp_thread_;
    std::thread tcp_thread_;

    mutable std::mutex mutex_;
    PacketCallback on_packet_;
    std::unordered_set<std::string> known_peer_ips_;
};

}  // namespace cslam
}  // namespace avscan
