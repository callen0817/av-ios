#include "avscan/sensors/AiryDiscovery.hpp"
#include <cmath>

bool run_test_m1_airy_discovery() {
    using namespace avscan::sensors;

    uint32_t ip = 0;
    if (!AiryDiscovery::parseIpv4("192.168.1.200", ip)) return false;
    if (ip != ((192u << 24) | (168u << 16) | (1u << 8) | 200u)) return false;
    if (AiryDiscovery::parseIpv4("999.168.1.1", ip)) return false;

    if (!AiryDiscovery::isSameSubnet("192.168.1.102", "192.168.1.200", "255.255.255.0")) return false;
    if (AiryDiscovery::isSameSubnet("192.168.2.102", "192.168.1.200", "255.255.255.0")) return false;

    AiryNetworkConfig cfg;
    cfg.sensor_ip = "192.168.1.200";
    cfg.host_expected_ip = "192.168.1.102";
    AiryDiscovery disc(cfg);
    auto ifaces = disc.enumerateNetworkInterfaces();
    // Should enumerate host network interfaces without crashing
    auto rep = disc.runDiagnostics(20, 20);
    return !rep.diagnostic_message.empty();
}
