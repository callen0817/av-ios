#include "avscan/cslam/CollaborativeTransport.hpp"
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SockLenT = int;
#define CLOSESOCK closesocket
#define INVALID_SOCK_VAL INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLenT = socklen_t;
#define CLOSESOCK ::close
#define INVALID_SOCK_VAL (-1)
#endif

namespace avscan {
namespace cslam {

namespace {

bool ensureSocketsInitialized() {
#ifdef _WIN32
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
            initialized = true;
        }
    }
    return initialized;
#else
    return true;
#endif
}

bool recvExact(intptr_t sock, uint8_t* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = static_cast<int>(::recv(sock, reinterpret_cast<char*>(dst + got),
                                        static_cast<int>(n - got), 0));
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

}  // namespace

CollaborativeLanTransport::CollaborativeLanTransport(uint16_t discovery_port, uint16_t data_port)
    : discovery_port_(discovery_port), data_port_(data_port) {}

CollaborativeLanTransport::~CollaborativeLanTransport() {
    stop();
}

void CollaborativeLanTransport::setPacketCallback(PacketCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_packet_ = std::move(cb);
}

void CollaborativeLanTransport::addDirectPeerIp(const std::string& ip_address) {
    if (ip_address.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    known_peer_ips_.insert(ip_address);
}

void CollaborativeLanTransport::clearDirectPeers() {
    std::lock_guard<std::mutex> lock(mutex_);
    known_peer_ips_.clear();
}

bool CollaborativeLanTransport::start() {
    if (running_.load()) return true;
    if (!ensureSocketsInitialized()) return false;

    // 1. UDP Discovery & Heartbeat socket
    udp_sock_ = static_cast<intptr_t>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (udp_sock_ == static_cast<intptr_t>(INVALID_SOCK_VAL)) {
        udp_sock_ = -1;
        return false;
    }
    int yes = 1;
    ::setsockopt(udp_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    ::setsockopt(udp_sock_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in udp_addr{};
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    udp_addr.sin_port = htons(discovery_port_);
    if (::bind(udp_sock_, reinterpret_cast<sockaddr*>(&udp_addr), sizeof(udp_addr)) != 0) {
        // Fallback: allow ephemeral bind so sending still works even if port is shared
    }

    // 2. TCP Keyframe Sync listener socket
    tcp_listen_sock_ = static_cast<intptr_t>(::socket(AF_INET, SOCK_STREAM, 0));
    if (tcp_listen_sock_ != static_cast<intptr_t>(INVALID_SOCK_VAL)) {
        ::setsockopt(tcp_listen_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in tcp_addr{};
        tcp_addr.sin_family = AF_INET;
        tcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        tcp_addr.sin_port = htons(data_port_);
        if (::bind(tcp_listen_sock_, reinterpret_cast<sockaddr*>(&tcp_addr), sizeof(tcp_addr)) == 0) {
            ::listen(tcp_listen_sock_, 8);
        } else {
            CLOSESOCK(tcp_listen_sock_);
            tcp_listen_sock_ = -1;
        }
    } else {
        tcp_listen_sock_ = -1;
    }

    running_.store(true);
    udp_thread_ = std::thread(&CollaborativeLanTransport::udpReceiveLoop, this);
    if (tcp_listen_sock_ != -1) {
        tcp_thread_ = std::thread(&CollaborativeLanTransport::tcpAcceptLoop, this);
    }
    return true;
}

void CollaborativeLanTransport::stop() {
    if (!running_.exchange(false)) return;
    if (udp_sock_ != -1) {
        CLOSESOCK(udp_sock_);
        udp_sock_ = -1;
    }
    if (tcp_listen_sock_ != -1) {
        CLOSESOCK(tcp_listen_sock_);
        tcp_listen_sock_ = -1;
    }
    if (udp_thread_.joinable()) udp_thread_.join();
    if (tcp_thread_.joinable()) tcp_thread_.join();
}

void CollaborativeLanTransport::udpReceiveLoop() {
    std::vector<uint8_t> buf(4096);
    while (running_.load() && udp_sock_ != -1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp_sock_, &rfds);
        timeval tv{0, 200000};  // 200 ms poll timeout for clean shutdown
        int sel = ::select(static_cast<int>(udp_sock_ + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in from{};
        SockLenT from_len = sizeof(from);
        int n = static_cast<int>(::recvfrom(udp_sock_, reinterpret_cast<char*>(buf.data()),
                                            static_cast<int>(buf.size()), 0,
                                            reinterpret_cast<sockaddr*>(&from), &from_len));
        if (n <= 0) continue;

        char ip_str[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
        std::string sender_ip(ip_str);

        PacketCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!sender_ip.empty() && sender_ip != "127.0.0.1") {
                known_peer_ips_.insert(sender_ip);
            }
            cb = on_packet_;
        }
        if (cb) {
            cb(buf.data(), static_cast<size_t>(n), sender_ip);
        }
    }
}

void CollaborativeLanTransport::tcpAcceptLoop() {
    while (running_.load() && tcp_listen_sock_ != -1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(tcp_listen_sock_, &rfds);
        timeval tv{0, 250000};
        int sel = ::select(static_cast<int>(tcp_listen_sock_ + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in from{};
        SockLenT from_len = sizeof(from);
        intptr_t client = static_cast<intptr_t>(::accept(tcp_listen_sock_, reinterpret_cast<sockaddr*>(&from), &from_len));
        if (client == static_cast<intptr_t>(INVALID_SOCK_VAL)) continue;

        char ip_str[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
        handleTcpClient(client, std::string(ip_str));
        CLOSESOCK(client);
    }
}

void CollaborativeLanTransport::handleTcpClient(intptr_t client_sock, const std::string& sender_ip) {
    uint32_t net_len = 0;
    if (!recvExact(client_sock, reinterpret_cast<uint8_t*>(&net_len), sizeof(net_len))) return;
    uint32_t payload_len = ntohl(net_len);
    if (payload_len == 0 || payload_len > 8 * 1024 * 1024u) return;

    std::vector<uint8_t> payload(payload_len);
    if (!recvExact(client_sock, payload.data(), payload_len)) return;

    PacketCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sender_ip.empty()) known_peer_ips_.insert(sender_ip);
        cb = on_packet_;
    }
    if (cb) {
        cb(payload.data(), payload.size(), sender_ip);
    }
}

void CollaborativeLanTransport::broadcastUdpPacket(const std::vector<uint8_t>& payload) {
    if (!running_.load() || udp_sock_ == -1 || payload.empty()) return;

    // 1. Subnet broadcast (255.255.255.255)
    sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(discovery_port_);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    ::sendto(udp_sock_, reinterpret_cast<const char*>(payload.data()),
             static_cast<int>(payload.size()), 0,
             reinterpret_cast<sockaddr*>(&bcast), sizeof(bcast));

    // 2. Direct unicast to any manually added / previously discovered peers
    std::vector<std::string> peers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peers.assign(known_peer_ips_.begin(), known_peer_ips_.end());
    }
    for (const auto& ip : peers) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(discovery_port_);
        if (::inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) == 1) {
            ::sendto(udp_sock_, reinterpret_cast<const char*>(payload.data()),
                     static_cast<int>(payload.size()), 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
    }
}

void CollaborativeLanTransport::sendTcpToIp(const std::string& ip, uint16_t port, const std::vector<uint8_t>& payload) {
    intptr_t s = static_cast<intptr_t>(::socket(AF_INET, SOCK_STREAM, 0));
    if (s == static_cast<intptr_t>(INVALID_SOCK_VAL)) return;

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1) {
        CLOSESOCK(s);
        return;
    }

    // Short send/recv timeout so unreachable peers never stall SLAM worker
#ifdef _WIN32
    DWORD timeout_ms = 500;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{0, 500000};
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    if (::connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
        uint32_t net_len = htonl(static_cast<uint32_t>(payload.size()));
        ::send(s, reinterpret_cast<const char*>(&net_len), sizeof(net_len), 0);
        ::send(s, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()), 0);
    }
    CLOSESOCK(s);
}

void CollaborativeLanTransport::broadcastTcpPacket(const std::vector<uint8_t>& payload) {
    if (!running_.load() || payload.empty()) return;
    std::vector<std::string> peers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peers.assign(known_peer_ips_.begin(), known_peer_ips_.end());
    }
    for (const auto& ip : peers) {
        sendTcpToIp(ip, data_port_, payload);
    }
}

}  // namespace cslam
}  // namespace avscan
