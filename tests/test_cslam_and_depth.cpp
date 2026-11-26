#include "avscan/cslam/CollaborativeSlamEngine.hpp"
#include "avscan/cslam/CollaborativeTransport.hpp"
#include "avscan/sensors/IDepthProvider.hpp"
#include <cmath>
#include <cstdio>

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

using namespace avscan;

int main() {
    // 1. Test hardware-independent DepthComputeTier & LiDAR-anchored metric depth alignment
    auto tier_a14 = sensors::LidarAnchoredDepthAligner::computeTierForHardware("iPad13,18");
    auto tier_m1  = sensors::LidarAnchoredDepthAligner::computeTierForHardware("iPad13,16");
    auto tier_orin = sensors::LidarAnchoredDepthAligner::computeTierForHardware("Jetson Orin NX");
    CHECK(tier_a14.default_fps >= 1.0 && tier_a14.default_fps <= 3.0);
    CHECK(tier_m1.default_fps >= 10.0 && tier_m1.default_fps <= 15.0);
    CHECK(tier_orin.default_fps >= 10.0);

    sensors::DepthFrame df;
    df.width = 160;
    df.height = 120;
    df.units = sensors::DepthUnits::RELATIVE_INVERSE_DEPTH;
    df.intrinsics = {160, 120, 120.0, 120.0, 80.0, 60.0, {0, 0, 0, 0, 0}};
    df.depth_data.resize(160 * 120, 0.0f);
    df.confidence_data.resize(160 * 120, 0.9f);

    // True depth z = 2.0 * d_raw + 0.5
    std::vector<core::LidarPointXYZIRT> lidar_pts;
    for (int v = 24; v < 100; v += 8) {
        for (int u = 24; u < 140; u += 8) {
            float d_raw = 0.5f + 0.02f * static_cast<float>(u - 20);
            float z_true = 2.0f * d_raw + 0.5f;
            df.depth_data[static_cast<size_t>(v * 160 + u)] = d_raw;

            core::LidarPointXYZIRT p;
            p.x = (static_cast<double>(u) - 80.0) * z_true / 120.0;
            p.y = (static_cast<double>(v) - 60.0) * z_true / 120.0;
            p.z = z_true;
            lidar_pts.push_back(p);
        }
    }

    double rmse = 1e9;
    size_t inliers = 0;
    bool anchored = sensors::LidarAnchoredDepthAligner::anchorToLidarPoints(
        df, lidar_pts, math::SE3d::Identity(), &rmse, &inliers);
    CHECK(anchored);
    CHECK(inliers >= 50);
    CHECK(rmse < 0.02);
    CHECK(std::abs(df.metric_scale - 2.0) < 0.05);
    CHECK(std::abs(df.metric_shift - 0.5) < 0.05);

    auto dense_pts = sensors::LidarAnchoredDepthAligner::backProjectToPointCloud(df, std::nullopt, 8);
    CHECK(!dense_pts.empty());

    // 2. Test Cross-Platform Collaborative SLAM Engine (iOS <-> Jetson wire round-trip + alignment)
    cslam::CollaborativeSlamEngine ipad_node("ipad_air5_01", "iPad Air 5", cslam::PeerPlatform::IOS, cslam::PeerApp::AV_IOS);
    cslam::CollaborativeSlamEngine jetson_node("jetson_orin_01", "Jetson Orin AV-SLAM", cslam::PeerPlatform::JETSON, cslam::PeerApp::AV_SLAM);

    // Beacon round-trip
    auto beacon = jetson_node.serializeBeacon(math::SE3d::Identity(), 1);
    CHECK(ipad_node.ingestRemotePacket(beacon.data(), beacon.size(), "192.168.1.50", 10.0));
    auto peers = ipad_node.getPeers(10.5);
    CHECK(peers.size() == 1);
    CHECK(peers[0].platform == cslam::PeerPlatform::JETSON);
    CHECK(peers[0].app == cslam::PeerApp::AV_SLAM);

    // Build identical room corner submap on both devices
    core::KeyframeSubmap kf;
    kf.keyframe_id = 1;
    kf.timestamp_sec = 10.0;
    kf.T_world_keyframe = math::SE3d::Identity();
    for (int i = -5; i <= 5; ++i) {
        for (int j = -5; j <= 5; ++j) {
            core::LidarPointXYZIRT p;
            p.x = 3.0; p.y = i * 0.2; p.z = j * 0.2; p.intensity = 120.0f;
            kf.local_points.push_back(p);
            core::LidarPointXYZIRT p2;
            p2.x = i * 0.2; p2.y = 3.0; p2.z = j * 0.2; p2.intensity = 150.0f;
            kf.local_points.push_back(p2);
        }
    }
    auto desc = backend::ScanContextDatabase::computeDescriptor(1, 10.0, kf.local_points);
    ipad_node.addLocalKeyframe(kf, desc);

    // Jetson broadcasts keyframe -> iPad ingests and aligns
    auto kf_pkt = jetson_node.serializeKeyframeSync(kf, desc, 500);
    CHECK(ipad_node.ingestRemotePacket(kf_pkt.data(), kf_pkt.size(), "192.168.1.50", 11.0));

    // Shared origin sync ensures immediate alignment even before loop overlap
    auto sync_pkt = jetson_node.serializeSharedOriginSync();
    CHECK(ipad_node.ingestRemotePacket(sync_pkt.data(), sync_pkt.size(), "192.168.1.50", 11.5));

    auto merged_cloud = ipad_node.buildAlignedPeerPointCloud();
    auto peer_traj = ipad_node.getAlignedPeerTrajectories();
    CHECK(!merged_cloud.empty());
    CHECK(!peer_traj.empty());

    std::printf("[PASS] IDepthProvider & Cross-Platform Collaborative SLAM Test (%zu merged peer pts, rmse=%.4fm)\n",
                merged_cloud.size(), rmse);
    return 0;
}
