#include "avscan/sensors/AiryDiscovery.hpp"
#include "avscan/sensors/AiryDecoder.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace avscan {
namespace sensors {

namespace {
#if defined(_WIN32)
struct WinsockInitializer {
    WinsockInitializer() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
};
static WinsockInitializer g_wsa_init;
#endif
}  // namespace

AiryDiscovery::AiryDiscovery(const AiryNetworkConfig& config) : config_(config) {}

void AiryDiscovery::setConfig(const AiryNetworkConfig& config) {
    config_ = config;
}

bool AiryDiscovery::parseIpv4(const std::string& ip_str, uint32_t& out_ip_host_order) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (std::sscanf(ip_str.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    out_ip_host_order = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

bool AiryDiscovery::isSameSubnet(const std::string& ip1, const std::string& ip2, const std::string& mask) {
    uint32_t u1 = 0, u2 = 0, um = 0;
    if (!parseIpv4(ip1, u1) || !parseIpv4(ip2, u2) || !parseIpv4(mask, um)) {
        return false;
    }
    return (u1 & um) == (u2 & um);
}

std::vector<NetworkInterfaceInfo> AiryDiscovery::enumerateNetworkInterfaces() const {
    std::vector<NetworkInterfaceInfo> list;

#if defined(_WIN32)
    ULONG bufLen = 15000;
    std::vector<uint8_t> buffer(bufLen);
    PIP_ADAPTER_ADDRESSES addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    ULONG ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufLen);
        addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &bufLen);
    }
    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES curr = addresses; curr != nullptr; curr = curr->Next) {
            for (PIP_ADAPTER_UNICAST_ADDRESS ua = curr->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                    sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                    char ipStr[INET_ADDRSTRLEN]{};
                    inet_ntop(AF_INET, &(sin->sin_addr), ipStr, sizeof(ipStr));

                    uint32_t maskVal = (ua->OnLinkPrefixLength == 0)
                        ? 0
                        : (0xFFFFFFFFu << (32 - ua->OnLinkPrefixLength));
                    in_addr maskAddr{};
                    maskAddr.s_addr = htonl(maskVal);
                    char maskStr[INET_ADDRSTRLEN]{};
                    inet_ntop(AF_INET, &maskAddr, maskStr, sizeof(maskStr));

                    NetworkInterfaceInfo info;
                    info.interface_name = curr->AdapterName ? curr->AdapterName : "eth0";
                    if (curr->FriendlyName) {
                        char friendly[256]{};
                        std::wcstombs(friendly, curr->FriendlyName, sizeof(friendly) - 1);
                        info.friendly_name = friendly;
                    }
                    info.ipv4_address = ipStr;
                    info.subnet_mask = maskStr;
                    info.is_up = (curr->OperStatus == IfOperStatusUp);
                    info.is_loopback = (curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
                    info.is_wired_ethernet = (curr->IfType == IF_TYPE_ETHERNET_CSMACD);
                    list.push_back(info);
                }
            }
        }
    }
#else
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0 && ifaddr != nullptr) {
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            NetworkInterfaceInfo info;
            info.interface_name = ifa->ifa_name ? ifa->ifa_name : "";
            info.friendly_name = info.interface_name;

            char ipStr[INET_ADDRSTRLEN]{};
            sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            inet_ntop(AF_INET, &(sin->sin_addr), ipStr, sizeof(ipStr));
            info.ipv4_address = ipStr;

            if (ifa->ifa_netmask) {
                char maskStr[INET_ADDRSTRLEN]{};
                sockaddr_in* smask = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask);
                inet_ntop(AF_INET, &(smask->sin_addr), maskStr, sizeof(maskStr));
                info.subnet_mask = maskStr;
            } else {
                info.subnet_mask = "255.255.255.0";
            }

            info.is_up = (ifa->ifa_flags & IFF_UP) != 0;
            info.is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
            // On iOS/iPadOS, USB-C Ethernet adapters appear as enX (typically en2..en8)
            info.is_wired_ethernet = !info.is_loopback &&
                (info.interface_name.rfind("en", 0) == 0 || info.interface_name.rfind("eth", 0) == 0);
            list.push_back(info);
        }
        freeifaddrs(ifaddr);
    }
#endif
    return list;
}

bool AiryDiscovery::probeSensorHttpInterface(int timeout_ms, double& out_latency_ms) const {
    out_latency_ms = 0.0;
    auto t0 = std::chrono::steady_clock::now();

#if defined(_WIN32)
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;
    u_long nonblock = 1;
    ioctlsocket(sock, FIONBIO, &nonblock);
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(config_.http_port);
    if (inet_pton(AF_INET, config_.sensor_ip.c_str(), &target.sin_addr) != 1) {
#if defined(_WIN32)
        closesocket(sock);
#else
        close(sock);
#endif
        return false;
    }

    int rc = connect(sock, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    bool connected = false;
    if (rc == 0) {
        connected = true;
    } else {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = select(static_cast<int>(sock + 1), nullptr, &wfds, nullptr, &tv);
        if (sel > 0) {
            int so_error = 0;
#if defined(_WIN32)
            int len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
#else
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
#endif
            connected = (so_error == 0);
        }
    }

#if defined(_WIN32)
    closesocket(sock);
#else
    close(sock);
#endif

    auto t1 = std::chrono::steady_clock::now();
    out_latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return connected;
}

AiryDiscoveryReport AiryDiscovery::runDiagnostics(int udp_listen_timeout_ms, int http_timeout_ms) const {
    AiryDiscoveryReport report;
    report.enumerated_interfaces = enumerateNetworkInterfaces();

    for (const auto& iface : report.enumerated_interfaces) {
        if (!iface.is_up || iface.is_loopback) continue;
        if (iface.is_wired_ethernet) {
            report.adapter_detected = true;
            report.active_interface_name = iface.interface_name;
            report.active_host_ip = iface.ipv4_address;
        }
        if (isSameSubnet(iface.ipv4_address, config_.sensor_ip, config_.subnet_mask)) {
            report.adapter_detected = true;
            report.subnet_matched = true;
            report.active_interface_name = iface.interface_name;
            report.active_host_ip = iface.ipv4_address;
            break;
        }
    }

    if (http_timeout_ms > 0) {
        report.http_reachable = probeSensorHttpInterface(http_timeout_ms, report.probe_latency_ms);
    }

    // Listen briefly for incoming DIFOP (7788) or MSOP (6699) UDP datagrams
    if (udp_listen_timeout_ms > 0) {
#if defined(_WIN32)
        SOCKET difopSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        bool validSock = (difopSock != INVALID_SOCKET);
#else
        int difopSock = socket(AF_INET, SOCK_DGRAM, 0);
        bool validSock = (difopSock >= 0);
#endif
        if (validSock) {
            int reuse = 1;
            setsockopt(difopSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            sockaddr_in localAddr{};
            localAddr.sin_family = AF_INET;
            localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
            localAddr.sin_port = htons(config_.difop_port);
            if (bind(difopSock, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) == 0) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(difopSock, &rfds);
                timeval tv{};
                tv.tv_sec = udp_listen_timeout_ms / 1000;
                tv.tv_usec = (udp_listen_timeout_ms % 1000) * 1000;
                if (select(static_cast<int>(difopSock + 1), &rfds, nullptr, nullptr, &tv) > 0) {
                    uint8_t buf[1500]{};
                    sockaddr_in sender{};
#if defined(_WIN32)
                    int slen = sizeof(sender);
#else
                    socklen_t slen = sizeof(sender);
#endif
                    int n = recvfrom(difopSock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                     reinterpret_cast<sockaddr*>(&sender), &slen);
                    if (n == static_cast<int>(AiryDecoder::kDifopPacketSize)) {
                        AiryDecoder decoder;
                        if (decoder.decodeDifopPacket(buf, static_cast<size_t>(n), report.device_info)) {
                            report.difop_received = true;
                        }
                    }
                }
            }
#if defined(_WIN32)
            closesocket(difopSock);
#else
            close(difopSock);
#endif
        }
    }

    if (report.difop_received || report.msop_received) {
        report.state = AiryConnectionState::STREAMING_ACTIVE;
        report.diagnostic_message = "RoboSense Airy connected and streaming telemetry.";
    } else if (report.http_reachable) {
        report.state = AiryConnectionState::SENSOR_REACHABLE_HTTP;
        report.diagnostic_message = "RoboSense Airy reachable at " + config_.sensor_ip +
                                    " (awaiting UDP stream on ports " + std::to_string(config_.msop_port) + "/" +
                                    std::to_string(config_.difop_port) + ").";
    } else if (report.adapter_detected && !report.subnet_matched) {
        report.state = AiryConnectionState::SUBNET_MISMATCH;
        report.diagnostic_message = "Ethernet adapter detected (" + report.active_host_ip +
                                    "), but IP is not on Airy subnet (" + config_.sensor_ip +
                                    " / " + config_.subnet_mask + "). Configure manual IPv4 to " +
                                    config_.host_expected_ip + " (Router " + config_.gateway_ip + ").";
    } else if (report.adapter_detected) {
        report.state = AiryConnectionState::ETHERNET_ADAPTER_DETECTED;
        report.diagnostic_message = "Ethernet configured on " + report.active_host_ip +
                                    ", but Airy sensor at " + config_.sensor_ip +
                                    " did not respond. Set Router to " + config_.gateway_ip + " on iOS.";
    } else {
        report.state = AiryConnectionState::DISCONNECTED;
        report.diagnostic_message = "No wired Ethernet adapter detected. Connect Belkin USB-C Ethernet adapter and Airy sensor.";
    }

    return report;
}

}  // namespace sensors
}  // namespace avscan
