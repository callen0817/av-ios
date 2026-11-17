#pragma once

#include "avscan/backend/PoseGraphOptimizer.hpp"
#include "avscan/core/DeviceMountProfile.hpp"
#include "avscan/core/Types.hpp"
#include "avscan/quality/QualityMonitor.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace avscan {
namespace export_bundle {

struct PanoStationRecord {
    std::string station_id{"st_000"};
    uint64_t station_index{0};
    uint64_t keyframe_id{0};
    double timestamp_sec{0.0};
    math::SE3d T_world_station{math::SE3d::Identity()}; // Center of spherical panorama
    std::string pano_image_relpath{"panos/pano_000.jpg"};
    std::string depth_image_relpath{"panos/pano_000_depth.png"};
    size_t point_count{0};
};

struct FloorLevelRecord {
    std::string floor_id{"floor_0"};
    std::string name{"Ground Level"};
    double elevation_m{0.0};
    double height_m{3.0};
    double slab_z_m{-0.2};
    std::string keyplan_relpath{"floors/floor_0_keyplan.png"};
    double keyplan_resolution_m{0.05}; // 5cm per pixel
    std::vector<std::string> station_ids;
};

struct AvShowcaseProjectConfig {
    std::string project_id{"PROJ_001"};
    std::string project_name{"AV Scan Session"};
    std::string scan_id{"SCAN_001"};
    std::string scan_date{"2026-11-12"};
    std::string operator_name{"Field Operator"};
    std::string device_profile_id{"ipad_10th_gen_airy_rigid"};
    std::string target_av_showcase_dir{}; // If populated, writes project.avproj & Scans/<scan_key>/
};

class AvSpatialBundleExporter {
public:
    // 1. Exports canonical AV Showcase / AV Structure `cache/lod0.bin`
    // Format: 4-byte uint32 count + N * [float32 x, float32 y, float32 z, uint8 r, uint8 g, uint8 b] (15 bytes/pt)
    static bool writeLod0Bin(const std::string& out_bin_path,
                             const std::vector<core::LidarPointXYZIRT>& points,
                             size_t max_points = 350000);

    // Reads `cache/lod0.bin` back into point cloud vector
    static bool readLod0Bin(const std::string& in_bin_path,
                            std::vector<core::LidarPointXYZIRT>& out_points);

    // 2. Exports complete AV Spatial Bundle v1.1 (`av_walkthrough_bundle.json`)
    // Compatible with AV Structure, AV Showcase v2.0, AV Scan, and AV ScanAR
    static bool exportWalkthroughBundle(const std::string& session_dir,
                                       const AvShowcaseProjectConfig& config,
                                       const std::vector<core::LidarPointXYZIRT>& global_cloud,
                                       const std::vector<core::SlamState>& trajectory,
                                       const quality::FloorPlanGrid2D& floor_plan,
                                       const std::vector<PanoStationRecord>& stations);

    // 3. Exports or updates root `project.avproj` for direct opening in AV Showcase v2.0
    static bool exportOrAppendShowcaseProject(const std::string& showcase_root_dir,
                                             const std::string& bundle_dir,
                                             const AvShowcaseProjectConfig& config,
                                             const std::vector<core::LidarPointXYZIRT>& global_cloud);

    // 4. Synthesizes a 2D floor keyplan image in PNG format matching AV Structure 5cm/px standard
    static bool exportFloorKeyplanPng(const std::string& out_png_path,
                                      const quality::FloorPlanGrid2D& plan);
};

} // namespace export_bundle
} // namespace avscan
