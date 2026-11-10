#include "avscan/export/PointCloudExporter.hpp"
#include "avscan/storage/ProjectStore.hpp"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace avscan;

bool run_test_m10_storage_export() {
    const std::string test_dir = "test_avscan_project_m10";
    std::filesystem::remove_all(test_dir);

    // 1. Test ProjectStore WAL Crash Recovery & Submap Disk Archiving
    storage::ProjectStore store;
    if (!store.initializeProject(test_dir, "PROJ_M10", "M10_Crash_And_Export_Test")) {
        std::printf("  [M10 FAIL] initializeProject failed\n");
        return false;
    }

    std::vector<core::KeyframeSubmap> submaps;
    for (uint64_t k = 0; k < 3; ++k) {
        core::KeyframeSubmap sm;
        sm.keyframe_id = k;
        sm.timestamp_sec = 100.0 + k;
        sm.T_world_submap = math::SE3d(math::SO3d::Identity(), math::Vec3d(1.0 * k, 2.0 * k, 0.0));
        for (int i = 0; i < 40; ++i) {
            core::LidarPointXYZIRT pt;
            pt.x = static_cast<float>(i * 0.1 + k);
            pt.y = static_cast<float>(i * 0.05);
            pt.z = -0.8f;
            pt.intensity = 150.0f;
            pt.r = 200;
            pt.g = 120;
            pt.b = 60;
            pt.has_rgb = 1;
            sm.local_points.push_back(pt);
        }
        if (!store.appendKeyframeToWal(sm)) {
            std::printf("  [M10 FAIL] appendKeyframeToWal failed\n");
            return false;
        }
        submaps.push_back(sm);
    }

    // Test submap archiving to disk and reloading into RAM
    if (!store.archiveSubmapToDisk(submaps[0]) || !submaps[0].archived_to_disk || !submaps[0].local_points.empty()) {
        std::printf("  [M10 FAIL] archiveSubmapToDisk did not offload points\n");
        return false;
    }
    if (!store.loadSubmapFromDisk(submaps[0]) || submaps[0].archived_to_disk || submaps[0].local_points.size() != 40) {
        std::printf("  [M10 FAIL] loadSubmapFromDisk failed to restore 40 points\n");
        return false;
    }

    // Simulate ungraceful crash before saveProjectSession is called, then recover from WAL
    {
        storage::ProjectStore recovery_store;
        storage::ProjectMetadata rec_meta{};
        core::ExtrinsicCalibrationProfile rec_calib{};
        std::vector<core::KeyframeSubmap> rec_submaps;
        std::vector<backend::PoseGraphFactor> rec_factors;
        std::vector<storage::PoiPhotoRecord> rec_photos;
        bool recovered_from_crash = false;

        if (!recovery_store.recoverOrLoadSession(
                test_dir, rec_meta, rec_calib, rec_submaps, rec_factors, rec_photos, recovered_from_crash)) {
            std::printf("  [M10 FAIL] recoverOrLoadSession failed on WAL recovery\n");
            return false;
        }
        if (!recovered_from_crash || rec_submaps.size() != 3 || rec_submaps[1].local_points.size() != 40) {
            std::printf("  [M10 FAIL] WAL crash recovery did not restore all 3 keyframes\n");
            return false;
        }
    }

    // Now test clean session save & reload
    core::ExtrinsicCalibrationProfile calib{};
    calib.device_model = "iPad_A2696";
    std::vector<backend::PoseGraphFactor> factors;
    std::vector<storage::PoiPhotoRecord> photos;
    storage::PoiPhotoRecord poi;
    poi.photo_id = 1;
    poi.timestamp_sec = 101.5;
    poi.image_file_path = "poi_001.jpg";
    poi.note = "North electrical panel";
    photos.push_back(poi);

    if (!store.saveProjectSession(calib, submaps, factors, photos)) {
        std::printf("  [M10 FAIL] saveProjectSession failed\n");
        return false;
    }

    // 2. Test PointCloudExporter (PLY, PCD, and ASTM E2807-11 E57 with CRC32C verification)
    const std::string ply_path = test_dir + "/scan_export.ply";
    const std::string pcd_path = test_dir + "/scan_export.pcd";
    const std::string e57_path = test_dir + "/scan_export.e57";

    const auto& export_pts = submaps[1].local_points;
    std::vector<core::LidarPointXYZIRT> read_ply, read_pcd, read_e57;

    if (!exporters::PointCloudExporter::exportToPly(ply_path, export_pts, true) ||
        !exporters::PointCloudExporter::readFromPly(ply_path, read_ply) ||
        read_ply.size() != export_pts.size()) {
        std::printf("  [M10 FAIL] PLY export/read round-trip failed\n");
        return false;
    }

    if (!exporters::PointCloudExporter::exportToPcd(pcd_path, export_pts, true) ||
        !exporters::PointCloudExporter::readFromPcd(pcd_path, read_pcd) ||
        read_pcd.size() != export_pts.size()) {
        std::printf("  [M10 FAIL] PCD export/read round-trip failed\n");
        return false;
    }

    if (!exporters::PointCloudExporter::exportToE57(e57_path, export_pts) ||
        !exporters::PointCloudExporter::readFromE57(e57_path, read_e57) ||
        read_e57.size() != export_pts.size()) {
        std::printf("  [M10 FAIL] E57 export/read round-trip failed\n");
        return false;
    }

    for (size_t i = 0; i < export_pts.size(); ++i) {
        if (std::abs(read_e57[i].x - export_pts[i].x) > 1e-4f ||
            read_e57[i].r != export_pts[i].r || read_e57[i].g != export_pts[i].g || read_e57[i].b != export_pts[i].b) {
            std::printf("  [M10 FAIL] E57 point data mismatch at index %zu\n", i);
            return false;
        }
    }

    std::filesystem::remove_all(test_dir);
    return true;
}
