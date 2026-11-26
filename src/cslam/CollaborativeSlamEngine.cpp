#include "avscan/cslam/CollaborativeSlamEngine.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace avscan {
namespace cslam {

namespace {

template <typename T>
void appendPod(std::vector<uint8_t>& buf, const T& val) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
    buf.insert(buf.end(), p, p + sizeof(T));
}

void appendString(std::vector<uint8_t>& buf, const std::string& s) {
    uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.size(), 128));
    appendPod(buf, len);
    if (len > 0) {
        buf.insert(buf.end(), s.data(), s.data() + len);
    }
}

void appendSe3(std::vector<uint8_t>& buf, const math::SE3d& T) {
    math::Quatd q = T.so3.unit_quaternion();
    double v[7] = {q.x, q.y, q.z, q.w, T.t.x, T.t.y, T.t.z};
    for (int i = 0; i < 7; ++i) appendPod(buf, v[i]);
}

template <typename T>
bool readPod(const uint8_t* data, size_t len, size_t& off, T& out) {
    if (off + sizeof(T) > len) return false;
    std::memcpy(&out, data + off, sizeof(T));
    off += sizeof(T);
    return true;
}

bool readString(const uint8_t* data, size_t len, size_t& off, std::string& out) {
    uint16_t slen = 0;
    if (!readPod(data, len, off, slen)) return false;
    if (off + slen > len) return false;
    out.assign(reinterpret_cast<const char*>(data + off), slen);
    off += slen;
    return true;
}

bool readSe3(const uint8_t* data, size_t len, size_t& off, math::SE3d& out) {
    double v[7]{};
    for (int i = 0; i < 7; ++i) {
        if (!readPod(data, len, off, v[i])) return false;
        if (!std::isfinite(v[i])) return false;
    }
    math::Quatd q(v[0], v[1], v[2], v[3]);
    q.normalize();
    out = math::SE3d(q, math::Vec3d(v[4], v[5], v[6]));
    return true;
}

#pragma pack(push, 1)
struct CompactWirePoint {
    float x, y, z;
    float intensity;
    uint8_t r, g, b, colorized;
};
#pragma pack(pop)

}  // namespace

CollaborativeSlamEngine::CollaborativeSlamEngine(std::string local_agent_id,
                                                 std::string local_display_name,
                                                 PeerPlatform local_platform,
                                                 PeerApp local_app)
    : local_agent_id_(std::move(local_agent_id)),
      local_display_name_(std::move(local_display_name)),
      local_platform_(local_platform),
      local_app_(local_app) {}

void CollaborativeSlamEngine::setLocalIdentity(const std::string& agent_id, const std::string& display_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!agent_id.empty()) local_agent_id_ = agent_id;
    if (!display_name.empty()) local_display_name_ = display_name;
}

std::string CollaborativeSlamEngine::platformToString(PeerPlatform p) {
    switch (p) {
        case PeerPlatform::IOS:     return "iOS/iPadOS";
        case PeerPlatform::JETSON:  return "Jetson";
        case PeerPlatform::WINDOWS: return "Windows";
        case PeerPlatform::LINUX:   return "Linux";
        case PeerPlatform::MACOS:   return "macOS";
        default:                    return "Unknown";
    }
}

std::string CollaborativeSlamEngine::appToString(PeerApp a) {
    switch (a) {
        case PeerApp::AV_IOS:    return "AV Scan iOS";
        case PeerApp::AV_SLAM:   return "AV SLAM";
        case PeerApp::AV_SCANAR: return "AV Scanar";
        case PeerApp::AV_SCAN:   return "AV Scan";
        default:                 return "AV Node";
    }
}

std::array<uint8_t, 3> CollaborativeSlamEngine::colorForAgent(const std::string& agent_id) {
    static constexpr std::array<std::array<uint8_t, 3>, 6> kPalette = {{
        {56, 189, 248},   // Cyan
        {74, 222, 128},   // Emerald
        {245, 158, 11},   // Amber
        {192, 132, 252},  // Violet
        {248, 113, 113},  // Coral
        {250, 204, 21}    // Gold
    }};
    size_t h = 2166136261u;
    for (unsigned char c : agent_id) {
        h ^= c;
        h *= 16777619u;
    }
    return kPalette[h % kPalette.size()];
}

void CollaborativeSlamEngine::addLocalKeyframe(const core::KeyframeSubmap& submap,
                                               const backend::ScanContextDescriptor& descriptor) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_submaps_.push_back(submap);
    local_descriptors_.push_back(descriptor);
    // Cap in-RAM local copies used for inter-agent GICP to 250 recent keyframes
    if (local_submaps_.size() > 250) {
        local_submaps_.erase(local_submaps_.begin());
        local_descriptors_.erase(local_descriptors_.begin());
    }
}

std::vector<uint8_t> CollaborativeSlamEngine::serializeBeacon(const math::SE3d& current_pose,
                                                              uint32_t local_keyframe_count,
                                                              uint16_t listen_data_port) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> buf;
    buf.reserve(192);
    appendPod(buf, kCslamMagic);
    appendPod(buf, kCslamProtocolVersion);
    appendPod(buf, static_cast<uint8_t>(CslamPacketType::PEER_BEACON));
    appendPod(buf, static_cast<uint8_t>(local_platform_));
    appendPod(buf, static_cast<uint8_t>(local_app_));
    appendPod(buf, listen_data_port);
    appendPod(buf, local_keyframe_count);
    appendString(buf, local_agent_id_);
    appendString(buf, local_display_name_);
    appendSe3(buf, current_pose);
    return buf;
}

std::vector<uint8_t> CollaborativeSlamEngine::serializePoseHeartbeat(double timestamp_sec,
                                                                     const math::SE3d& current_pose) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> buf;
    buf.reserve(128);
    appendPod(buf, kCslamMagic);
    appendPod(buf, kCslamProtocolVersion);
    appendPod(buf, static_cast<uint8_t>(CslamPacketType::POSE_HEARTBEAT));
    appendPod(buf, static_cast<uint8_t>(local_platform_));
    appendPod(buf, static_cast<uint8_t>(local_app_));
    appendString(buf, local_agent_id_);
    appendPod(buf, timestamp_sec);
    appendSe3(buf, current_pose);
    return buf;
}

std::vector<uint8_t> CollaborativeSlamEngine::serializeKeyframeSync(
    const core::KeyframeSubmap& submap,
    const backend::ScanContextDescriptor& descriptor,
    size_t max_points_per_packet) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> buf;
    size_t stride = 1;
    if (max_points_per_packet > 0 && submap.local_points.size() > max_points_per_packet) {
        stride = (submap.local_points.size() + max_points_per_packet - 1) / max_points_per_packet;
    }
    uint32_t emitted_pts = 0;
    for (size_t i = 0; i < submap.local_points.size(); i += stride) emitted_pts++;

    buf.reserve(512 + descriptor.matrix.size() * sizeof(float) + emitted_pts * sizeof(CompactWirePoint));
    appendPod(buf, kCslamMagic);
    appendPod(buf, kCslamProtocolVersion);
    appendPod(buf, static_cast<uint8_t>(CslamPacketType::KEYFRAME_SYNC));
    appendPod(buf, static_cast<uint8_t>(local_platform_));
    appendPod(buf, static_cast<uint8_t>(local_app_));
    appendString(buf, local_agent_id_);
    appendString(buf, local_display_name_);
    appendPod(buf, submap.keyframe_id);
    appendPod(buf, submap.timestamp_sec);
    appendSe3(buf, submap.T_world_keyframe);

    // Compact Scan Context descriptor (ring_key + matrix)
    for (float rk : descriptor.ring_key) appendPod(buf, rk);
    for (float mv : descriptor.matrix) appendPod(buf, mv);

    appendPod(buf, emitted_pts);
    for (size_t i = 0; i < submap.local_points.size(); i += stride) {
        const auto& p = submap.local_points[i];
        CompactWirePoint wp;
        wp.x = static_cast<float>(p.x);
        wp.y = static_cast<float>(p.y);
        wp.z = static_cast<float>(p.z);
        wp.intensity = p.intensity;
        wp.r = p.r;
        wp.g = p.g;
        wp.b = p.b;
        wp.colorized = p.colorized ? 1 : 0;
        appendPod(buf, wp);
    }
    return buf;
}

std::vector<uint8_t> CollaborativeSlamEngine::serializeSharedOriginSync() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> buf;
    buf.reserve(64);
    appendPod(buf, kCslamMagic);
    appendPod(buf, kCslamProtocolVersion);
    appendPod(buf, static_cast<uint8_t>(CslamPacketType::SHARED_ORIGIN_SYNC));
    appendPod(buf, static_cast<uint8_t>(local_platform_));
    appendPod(buf, static_cast<uint8_t>(local_app_));
    appendString(buf, local_agent_id_);
    appendString(buf, local_display_name_);
    return buf;
}

void CollaborativeSlamEngine::registerExternalPeer(const std::string& agent_id,
                                                   const std::string& display_name,
                                                   PeerPlatform platform,
                                                   PeerApp app,
                                                   const std::string& endpoint_label,
                                                   double now_sec) {
    if (agent_id.empty() || agent_id == local_agent_id_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& peer = peers_[agent_id];
    if (peer.agent_id.empty()) {
        peer.agent_id = agent_id;
        peer.display_rgb = colorForAgent(agent_id);
    }
    if (!display_name.empty()) peer.display_name = display_name;
    if (platform != PeerPlatform::UNKNOWN) peer.platform = platform;
    if (app != PeerApp::UNKNOWN) peer.app = app;
    if (!endpoint_label.empty()) peer.ip_address = endpoint_label;
    peer.connected = true;
    peer.last_seen_sec = now_sec;
}

bool CollaborativeSlamEngine::tryAlignPeerWithLocalMap(PeerInfo& peer, const RemoteKeyframeRecord& remote_kf) {
    if (local_descriptors_.empty() || local_submaps_.empty() || remote_kf.local_points.empty()) {
        return false;
    }

    double best_dist = 0.28;
    int best_idx = -1;
    int best_shift = 0;

    for (size_t i = 0; i < local_descriptors_.size(); ++i) {
        int shift = 0;
        double d = backend::ScanContextDatabase::computeDistanceAndYawShift(
            remote_kf.sc_descriptor, local_descriptors_[i], shift);
        if (d < best_dist) {
            best_dist = d;
            best_idx = static_cast<int>(i);
            best_shift = shift;
        }
    }

    if (best_idx < 0) return false;

    const auto& local_sm = local_submaps_[static_cast<size_t>(best_idx)];
    if (local_sm.local_points.empty()) return false;

    double yaw_rad = static_cast<double>(best_shift) *
                     (2.0 * 3.14159265358979323846 / static_cast<double>(backend::ScanContextDescriptor::kNumSectors));
    math::SE3d init_guess(math::SO3d::exp(math::Vec3d(0.0, 0.0, yaw_rad)), math::Vec3d(0.0, 0.0, 0.0));

    auto ver = gicp_verifier_.verifyLoopConstraint(
        remote_kf.local_points, local_sm.local_points, init_guess);
    if (!ver.verified) {
        return false;
    }

    // T_localWorld_peerWorld = T_localWorld_localKf * T_localKf_remoteKf * (T_peerWorld_remoteKf)^-1
    peer.T_local_peer = local_sm.T_world_keyframe * ver.T_target_source * remote_kf.T_peer_keyframe.inverse();
    peer.aligned = true;
    peer.alignment_rmse_m = ver.fitness_rmse_m;
    stats_.inter_agent_matches++;
    return true;
}

bool CollaborativeSlamEngine::ingestRemotePacket(const uint8_t* data,
                                                 size_t len,
                                                 const std::string& sender_ip,
                                                 double now_sec) {
    if (!data || len < 9) return false;

    size_t off = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint8_t ptype_raw = 0;
    uint8_t plat_raw = 0;
    uint8_t app_raw = 0;

    if (!readPod(data, len, off, magic) || magic != kCslamMagic) return false;
    if (!readPod(data, len, off, version) || version != kCslamProtocolVersion) return false;
    if (!readPod(data, len, off, ptype_raw)) return false;
    if (!readPod(data, len, off, plat_raw)) return false;
    if (!readPod(data, len, off, app_raw)) return false;

    auto ptype = static_cast<CslamPacketType>(ptype_raw);
    auto plat = static_cast<PeerPlatform>(plat_raw);
    auto app = static_cast<PeerApp>(app_raw);

    std::lock_guard<std::mutex> lock(mutex_);

    if (ptype == CslamPacketType::PEER_BEACON) {
        uint16_t data_port = kCslamDataPort;
        uint32_t kf_count = 0;
        std::string agent_id, display_name;
        math::SE3d pose;
        if (!readPod(data, len, off, data_port)) return false;
        if (!readPod(data, len, off, kf_count)) return false;
        if (!readString(data, len, off, agent_id)) return false;
        if (!readString(data, len, off, display_name)) return false;
        if (!readSe3(data, len, off, pose)) return false;
        if (agent_id.empty() || agent_id == local_agent_id_) return false;

        auto& peer = peers_[agent_id];
        if (peer.agent_id.empty()) {
            peer.agent_id = agent_id;
            peer.display_rgb = colorForAgent(agent_id);
        }
        peer.display_name = display_name;
        peer.platform = plat;
        peer.app = app;
        if (!sender_ip.empty()) peer.ip_address = sender_ip;
        peer.data_port = data_port;
        peer.connected = true;
        peer.last_seen_sec = now_sec;
        peer.keyframe_count = std::max(peer.keyframe_count, kf_count);
        peer.latest_pose_in_peer_world = pose;
        stats_.packets_received++;
        return true;
    }

    if (ptype == CslamPacketType::POSE_HEARTBEAT) {
        std::string agent_id;
        double ts = 0.0;
        math::SE3d pose;
        if (!readString(data, len, off, agent_id)) return false;
        if (!readPod(data, len, off, ts)) return false;
        if (!readSe3(data, len, off, pose)) return false;
        if (agent_id.empty() || agent_id == local_agent_id_) return false;

        auto& peer = peers_[agent_id];
        if (peer.agent_id.empty()) {
            peer.agent_id = agent_id;
            peer.display_name = agent_id;
            peer.display_rgb = colorForAgent(agent_id);
        }
        peer.platform = plat;
        peer.app = app;
        if (!sender_ip.empty()) peer.ip_address = sender_ip;
        peer.connected = true;
        peer.last_seen_sec = now_sec;
        peer.latest_pose_in_peer_world = pose;
        stats_.packets_received++;
        return true;
    }

    if (ptype == CslamPacketType::SHARED_ORIGIN_SYNC) {
        std::string agent_id, display_name;
        if (!readString(data, len, off, agent_id)) return false;
        if (!readString(data, len, off, display_name)) return false;
        if (agent_id.empty() || agent_id == local_agent_id_) return false;

        auto& peer = peers_[agent_id];
        if (peer.agent_id.empty()) {
            peer.agent_id = agent_id;
            peer.display_rgb = colorForAgent(agent_id);
        }
        peer.display_name = display_name;
        peer.platform = plat;
        peer.app = app;
        if (!sender_ip.empty()) peer.ip_address = sender_ip;
        peer.connected = true;
        peer.last_seen_sec = now_sec;
        peer.T_local_peer = math::SE3d::Identity();
        peer.aligned = true;
        peer.alignment_rmse_m = 0.0;
        stats_.packets_received++;
        return true;
    }

    if (ptype == CslamPacketType::KEYFRAME_SYNC) {
        std::string agent_id, display_name;
        RemoteKeyframeRecord rec;
        if (!readString(data, len, off, agent_id)) return false;
        if (!readString(data, len, off, display_name)) return false;
        if (agent_id.empty() || agent_id == local_agent_id_) return false;

        rec.agent_id = agent_id;
        if (!readPod(data, len, off, rec.keyframe_id)) return false;
        if (!readPod(data, len, off, rec.timestamp_sec)) return false;
        if (!readSe3(data, len, off, rec.T_peer_keyframe)) return false;

        rec.sc_descriptor.keyframe_id = rec.keyframe_id;
        rec.sc_descriptor.timestamp_sec = rec.timestamp_sec;
        for (float& rk : rec.sc_descriptor.ring_key) {
            if (!readPod(data, len, off, rk)) return false;
        }
        for (float& mv : rec.sc_descriptor.matrix) {
            if (!readPod(data, len, off, mv)) return false;
        }

        uint32_t pt_count = 0;
        if (!readPod(data, len, off, pt_count) || pt_count > 50000u) return false;
        rec.local_points.reserve(pt_count);
        for (uint32_t i = 0; i < pt_count; ++i) {
            CompactWirePoint wp{};
            if (!readPod(data, len, off, wp)) return false;
            core::LidarPointXYZIRT p;
            p.x = wp.x; p.y = wp.y; p.z = wp.z;
            p.intensity = wp.intensity;
            p.r = wp.r; p.g = wp.g; p.b = wp.b;
            p.colorized = (wp.colorized != 0);
            rec.local_points.push_back(p);
        }

        auto& peer = peers_[agent_id];
        if (peer.agent_id.empty()) {
            peer.agent_id = agent_id;
            peer.display_rgb = colorForAgent(agent_id);
        }
        peer.display_name = display_name;
        peer.platform = plat;
        peer.app = app;
        if (!sender_ip.empty()) peer.ip_address = sender_ip;
        peer.connected = true;
        peer.last_seen_sec = now_sec;
        peer.latest_pose_in_peer_world = rec.T_peer_keyframe;

        // Deduplicate by keyframe_id
        auto& kf_list = peer_keyframes_[agent_id];
        bool duplicate = false;
        for (const auto& existing : kf_list) {
            if (existing.keyframe_id == rec.keyframe_id) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            kf_list.push_back(rec);
            if (kf_list.size() > 160) {
                kf_list.erase(kf_list.begin());
            }
            peer.keyframe_count = static_cast<uint32_t>(kf_list.size());
            stats_.remote_keyframes_received++;
            if (!peer.aligned) {
                tryAlignPeerWithLocalMap(peer, kf_list.back());
            }
        }
        stats_.packets_received++;
        return true;
    }

    return false;
}

void CollaborativeSlamEngine::syncSharedOriginWithAllPeers() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : peers_) {
        kv.second.T_local_peer = math::SE3d::Identity();
        kv.second.aligned = true;
        kv.second.alignment_rmse_m = 0.0;
    }
}

std::vector<core::LidarPointXYZIRT> CollaborativeSlamEngine::buildAlignedPeerPointCloud(size_t max_total_points) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::LidarPointXYZIRT> merged;

    size_t total_available = 0;
    for (const auto& kv : peer_keyframes_) {
        auto pit = peers_.find(kv.first);
        if (pit == peers_.end() || !pit->second.aligned) continue;
        for (const auto& kf : kv.second) total_available += kf.local_points.size();
    }
    if (total_available == 0) return merged;

    size_t stride = 1;
    if (max_total_points > 0 && total_available > max_total_points) {
        stride = (total_available + max_total_points - 1) / max_total_points;
    }
    merged.reserve(std::min(total_available, max_total_points));

    for (const auto& kv : peer_keyframes_) {
        auto pit = peers_.find(kv.first);
        if (pit == peers_.end() || !pit->second.aligned) continue;
        const auto& peer = pit->second;

        for (const auto& kf : kv.second) {
            math::SE3d T_local_kf = peer.T_local_peer * kf.T_peer_keyframe;
            for (size_t i = 0; i < kf.local_points.size(); i += stride) {
                const auto& p = kf.local_points[i];
                math::Vec3d pw = T_local_kf.transform(math::Vec3d(p.x, p.y, p.z));
                core::LidarPointXYZIRT out = p;
                out.x = pw.x; out.y = pw.y; out.z = pw.z;
                // If peer point wasn't RGB-colorized, tint subtly with peer's assigned color
                if (!out.colorized) {
                    out.r = peer.display_rgb[0];
                    out.g = peer.display_rgb[1];
                    out.b = peer.display_rgb[2];
                }
                merged.push_back(out);
            }
        }
    }
    return merged;
}

std::vector<core::SlamState> CollaborativeSlamEngine::getAlignedPeerTrajectories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::SlamState> traj;
    for (const auto& kv : peers_) {
        const auto& peer = kv.second;
        if (!peer.aligned) continue;
        auto kfit = peer_keyframes_.find(peer.agent_id);
        if (kfit != peer_keyframes_.end()) {
            for (const auto& kf : kfit->second) {
                core::SlamState st;
                st.timestamp_sec = kf.timestamp_sec;
                st.T_world_body = peer.T_local_peer * kf.T_peer_keyframe;
                traj.push_back(st);
            }
        }
        core::SlamState latest;
        latest.timestamp_sec = peer.last_seen_sec;
        latest.T_world_body = peer.T_local_peer * peer.latest_pose_in_peer_world;
        traj.push_back(latest);
    }
    return traj;
}

std::vector<PeerInfo> CollaborativeSlamEngine::getPeers(double now_sec, double stale_timeout_sec) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerInfo> out;
    out.reserve(peers_.size());
    for (const auto& kv : peers_) {
        PeerInfo p = kv.second;
        if (now_sec > 0.0 && p.last_seen_sec > 0.0 && (now_sec - p.last_seen_sec) > stale_timeout_sec) {
            p.connected = false;
        }
        out.push_back(p);
    }
    return out;
}

CslamStats CollaborativeSlamEngine::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CslamStats s = stats_;
    s.discovered_peers = peers_.size();
    s.aligned_peers = 0;
    for (const auto& kv : peers_) {
        if (kv.second.aligned) s.aligned_peers++;
    }
    return s;
}

void CollaborativeSlamEngine::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    local_submaps_.clear();
    local_descriptors_.clear();
    peer_keyframes_.clear();
    for (auto& kv : peers_) {
        kv.second.aligned = false;
        kv.second.keyframe_count = 0;
        kv.second.T_local_peer = math::SE3d::Identity();
    }
    stats_ = CslamStats{};
}

}  // namespace cslam
}  // namespace avscan
