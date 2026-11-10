#pragma once

#include "avscan/core/Types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace avscan {
namespace sensors {

enum class AiryConnectionState : uint8_t {
    DISCONNECTED = 0,
    ETHERNET_ADAPTER_DETECTED = 1,
    SUBNET_MISMATCH = 2,
    SENSOR_REACHABLE_HTTP = 3,
    DIFOP_DETECTED = 4,
    STREAMING_ACTIVE = 5,
    ERROR_STATE = 6
};

struct NetworkInterfaceInfo {
    std::string interface_name{};
    std::string friendly_name{};
    std::string ipv4_address{};
    std::string subnet_mask{};
    bool is_up{false};
    bool is_loopback{false};
    bool is_wired_ethernet{false};
};

struct AiryNetworkConfig {
    std::string sensor_ip{"192.168.1.200"};
    std::string sensor_mac{"08:48:57:09:82:89"};
    std::string host_expected_ip{"192.168.1.102"};
    std::string subnet_mask{"255.255.255.0"};
    std::string gateway_ip{"192.168.1.1"};       // Required on iOS manual Ethernet config for ICMP/HTTP routing
    std::string adapter_mac{"A0:CE:C8:0C:88:4A"}; // Belkin / Realtek USB GbE Family Controller
    uint16_t msop_port{6699};                    // Airy source port 49609 -> host 6699 (1248B)
    uint16_t difop_port{7788};                   // Airy source port 49608 -> host 7788 (1248B)
    uint16_t imu_port{6688};                     // Airy source port 49607 -> host 6688 (51B @ 200Hz)
    uint16_t http_port{80};
    uint16_t motor_rpm{600};                     // 10 Hz rotation, High-Performance, Strongest return
};

struct AiryDiscoveryReport {
    AiryConnectionState state{AiryConnectionState::DISCONNECTED};
    std::string active_interface_name{};
    std::string active_host_ip{};
    bool adapter_detected{false};
    bool subnet_matched{false};
    bool http_reachable{false};
    bool difop_received{false};
    bool msop_received{false};
    bool imu_received{false};
    double probe_latency_ms{0.0};
    core::AiryFactoryCalibration device_info{};
    std::string diagnostic_message{};
    std::vector<NetworkInterfaceInfo> enumerated_interfaces;
};

class AiryDiscovery {
public:
    explicit AiryDiscovery(const AiryNetworkConfig& config = AiryNetworkConfig{});

    void setConfig(const AiryNetworkConfig& config);
    const AiryNetworkConfig& getConfig() const { return config_; }

    // Enumerates real OS network interfaces (BSD getifaddrs on iOS/POSIX, GetAdaptersAddresses on Windows)
    std::vector<NetworkInterfaceInfo> enumerateNetworkInterfaces() const;

    // Checks if two IPv4 addresses belong to the same subnet given a subnet mask
    static bool isSameSubnet(const std::string& ip1, const std::string& ip2, const std::string& mask);

    // Parses an IPv4 dotted string to host-order uint32_t; returns false if invalid
    static bool parseIpv4(const std::string& ip_str, uint32_t& out_ip_host_order);

    // Probes TCP HTTP port (default 192.168.1.200:80) with non-blocking connect timeout
    bool probeSensorHttpInterface(int timeout_ms, double& out_latency_ms) const;

    // Runs a complete Gate 1 diagnostic check (interface check + HTTP reachability + passive UDP check)
    AiryDiscoveryReport runDiagnostics(int udp_listen_timeout_ms = 250, int http_timeout_ms = 250) const;

private:
    AiryNetworkConfig config_;
};

}  // namespace sensors
}  // namespace avscan
