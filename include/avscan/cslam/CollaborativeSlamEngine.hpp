#pragma once

#include "avscan/backend/GicpVerifier.hpp"
#include "avscan/backend/ScanContext.hpp"
#include "avscan/core/Types.hpp"
#include "avscan/math/SophusLie.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace avscan {
namespace cslam {

// Universal cross-platform C-SLAM wire protocol constants (AV iOS, AV SLAM, AV Scanar, AV Scan)
constexpr uint32_t kCslamMagic = 0x53435641u;  // "AVCS"
constexpr uint16_t kCslamProtocolVersion = 1;
constexpr uint16_t kCslamDiscoveryPort = 48765;
constexpr uint16_t kCslamDataPort = 48766;

enum class PeerPlatform : uint8_t {
    UNKNOWN = 0,
    IOS = 1,
    JETSON = 2,
    WINDOWS = 3,
    LINUX = 4,
    MACOS = 5
};

enum class PeerApp : uint8_t {
    UNKNOWN = 0,
    AV_IOS = 1,
    AV_SLAM = 2,
    AV_SCANAR = 3,
    AV_SCAN = 4
};

enum class CslamPacketType : uint8_t {
    PEER_BEACON = 1,
    POSE_HEARTBEAT = 2,
    KEYFRAME_SYNC = 3,
    SHARED_ORIGIN_SYNC = 4
};

struct PeerInfo {
    std::string agent_id{};
    std::string display_name{};
    std::string ip_address{};
    PeerPlatform platform{PeerPlatform::UNKNOWN};
    PeerApp app{PeerApp::UNKNOWN};
    uint16_t data_port{kCslamDataPort};
    bool connected{false};
    bool aligned{false};
    double last_seen_sec{0.0};
    uint32_t keyframe_count{0};
    math::SE3d latest_pose_in_peer_world{math::SE3d::Identity()};
    math::SE3d T_local_peer{math::SE3d::Identity()};  // Transforms peer-world -> local-world
    double alignment_rmse_m{0.0};
    std::array<uint8_t, 3> display_rgb{56, 189, 248};
};

struct RemoteKeyframeRecord {
    std::string agent_id{};
    uint64_t keyframe_id{0};
    double timestamp_sec{0.0};
    math::SE3d T_peer_keyframe{math::SE3d::Identity()};
    backend::ScanContextDescriptor sc_descriptor{};
    std::vector<core::LidarPointXYZIRT> local_points;
};

struct CslamStats {
    size_t discovered_peers{0};
    size_t aligned_peers{0};
    size_t remote_keyframes_received{0};
    size_t inter_agent_matches{0};
    size_t packets_sent{0};
    size_t packets_received{0};
};

class CollaborativeSlamEngine {
public:
    CollaborativeSlamEngine(std::string local_agent_id,
                            std::string local_display_name,
                            PeerPlatform local_platform,
                            PeerApp local_app);

    void setLocalIdentity(const std::string& agent_id, const std::string& display_name);
    const std::string& localAgentId() const { return local_agent_id_; }
    const std::string& localDisplayName() const { return local_display_name_; }
    PeerPlatform localPlatform() const { return local_platform_; }
    PeerApp localApp() const { return local_app_; }

    // Register a local keyframe for inter-agent Scan Context + GICP matching
    void addLocalKeyframe(const core::KeyframeSubmap& submap,
                          const backend::ScanContextDescriptor& descriptor);

    // Serialize outgoing packets (transport-agnostic: UDP, TCP, or Apple MultipeerConnectivity)
    std::vector<uint8_t> serializeBeacon(const math::SE3d& current_pose,
                                         uint32_t local_keyframe_count,
                                         uint16_t listen_data_port = kCslamDataPort) const;

    std::vector<uint8_t> serializePoseHeartbeat(double timestamp_sec,
                                                const math::SE3d& current_pose) const;

    std::vector<uint8_t> serializeKeyframeSync(const core::KeyframeSubmap& submap,
                                               const backend::ScanContextDescriptor& descriptor,
                                               size_t max_points_per_packet = 1500) const;

    std::vector<uint8_t> serializeSharedOriginSync() const;

    // Ingest an incoming wire packet from any peer (iOS, Jetson, Windows, Linux)
    // Returns true if valid and processed.
    bool ingestRemotePacket(const uint8_t* data,
                            size_t len,
                            const std::string& sender_ip,
                            double now_sec);

    // Manually declare that all currently connected peers share the same initial origin
    // (e.g., devices started side-by-side) so live clouds appear immediately before loop overlap.
    void syncSharedOriginWithAllPeers();

    // Register or update a peer discovered via external transport (e.g., MultipeerConnectivity)
    void registerExternalPeer(const std::string& agent_id,
                              const std::string& display_name,
                              PeerPlatform platform,
                              PeerApp app,
                              const std::string& endpoint_label,
                              double now_sec);

    // Build merged point cloud of all aligned peer keyframes transformed into local world frame
    std::vector<core::LidarPointXYZIRT> buildAlignedPeerPointCloud(size_t max_total_points = 120000) const;

    // Get live trajectories (latest + keyframe poses) of all aligned peers in local world frame
    std::vector<core::SlamState> getAlignedPeerTrajectories() const;

    std::vector<PeerInfo> getPeers(double now_sec, double stale_timeout_sec = 15.0) const;
    CslamStats getStats() const;

    void reset();

    static std::string platformToString(PeerPlatform p);
    static std::string appToString(PeerApp a);

private:
    bool tryAlignPeerWithLocalMap(PeerInfo& peer, const RemoteKeyframeRecord& remote_kf);
    static std::array<uint8_t, 3> colorForAgent(const std::string& agent_id);

    std::string local_agent_id_;
    std::string local_display_name_;
    PeerPlatform local_platform_;
    PeerApp local_app_;

    mutable std::mutex mutex_;
    std::vector<core::KeyframeSubmap> local_submaps_;
    std::vector<backend::ScanContextDescriptor> local_descriptors_;
    std::unordered_map<std::string, PeerInfo> peers_;
    std::unordered_map<std::string, std::vector<RemoteKeyframeRecord>> peer_keyframes_;
    backend::GicpVerifier gicp_verifier_;
    CslamStats stats_{};
};

}  // namespace cslam
}  // namespace avscan
