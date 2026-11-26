#pragma once

#include "avscan/core/Types.hpp"
#include <cstddef>
#include <vector>

namespace avscan {
namespace processing {

struct CleanupConfig {
    // Stage 1: Multi-submap ray-carved dynamic/transient object removal
    bool enable_dynamic_filter{true};
    double dynamic_voxel_m{0.20};
    uint32_t dynamic_min_pass_through{3};

    // Stage 2: Multi-observation voxel centroid + confidence-weighted RGB fusion
    bool enable_voxel_centroid_fusion{true};
    double voxel_size_m{0.015};

    // Stage 3: Fast O(N) spatial-hash Radius + Statistical Outlier Removal (dust, glass multipath, floaters)
    bool enable_ror{true};
    double ror_radius_m{0.08};
    int ror_min_neighbors{4};
    bool enable_sor{true};
    int sor_k{12};
    double sor_std_mul{2.2};

    // Stage 4: Edge-preserving planar projection denoising (walls, floors, ceilings; preserves corners)
    bool enable_planar_denoise{true};
    double planar_cell_m{0.12};
    double planar_blend{0.65};
    double planar_max_shift_m{0.025};

    // Stage 5: Guarded dominant floor-plane tilt leveling (removes small residual IMU gravity tilt <= 5 deg)
    bool enable_floor_leveling{true};
    double min_level_tilt_deg{0.25};
    double max_level_tilt_deg{5.0};
};

struct CleanupStats {
    size_t raw_input_points{0};
    size_t dynamic_removed{0};
    size_t voxel_fused_points{0};
    size_t ror_removed{0};
    size_t sor_removed{0};
    size_t planar_denoised_points{0};
    size_t final_output_points{0};
    bool floor_leveled{false};
    double floor_tilt_corrected_deg{0.0};
    math::SE3d T_level_world{math::SE3d::Identity()};
    double elapsed_ms{0.0};
};

class PointCloudCleanup {
public:
    // Stage 1: Removes transient/dynamic objects seen in <= 1 keyframe that later LiDAR rays pass cleanly through
    static std::vector<core::LidarPointXYZIRT> filterDynamicTransients(
        const std::vector<core::KeyframeSubmap>& submaps,
        double voxel_m = 0.20,
        uint32_t min_pass_through = 3,
        size_t* out_removed_count = nullptr);

    // Stage 2: Averages XYZ, intensity, and view-confidence-weighted RGB per voxel (replaces first-hit dedup)
    static std::vector<core::LidarPointXYZIRT> fuseVoxelCentroidsWeightedRgb(
        const std::vector<core::LidarPointXYZIRT>& points,
        double voxel_size_m = 0.015);

    // Stage 3: Single-pass O(N) spatial-hash Radius Outlier Removal + Statistical Outlier Removal
    static std::vector<core::LidarPointXYZIRT> removeSpatialHashOutliers(
        const std::vector<core::LidarPointXYZIRT>& points,
        bool enable_ror = true,
        double ror_radius_m = 0.08,
        int ror_min_neighbors = 4,
        bool enable_sor = true,
        int sor_k = 12,
        double sor_std_mul = 2.2,
        size_t* out_ror_removed = nullptr,
        size_t* out_sor_removed = nullptr);

    // Stage 4: Gently projects locally planar surfaces toward their local eigen-plane (corners/edges untouched)
    static size_t denoisePlanarSurfaces(
        std::vector<core::LidarPointXYZIRT>& io_points,
        double cell_size_m = 0.12,
        double blend = 0.65,
        double max_shift_m = 0.025);

    // Stage 5: Fits dominant horizontal floor plane and corrects small residual gravity tilt [min_deg, max_deg]
    static bool computeAndApplyFloorLeveling(
        std::vector<core::LidarPointXYZIRT>& io_points,
        double min_tilt_deg = 0.25,
        double max_tilt_deg = 5.0,
        math::SE3d* out_T_level_world = nullptr,
        double* out_tilt_deg = nullptr);

    // End-to-end Save & Refine pipeline from pose-graph keyframe submaps
    static CleanupStats runPostScanPipeline(
        const std::vector<core::KeyframeSubmap>& submaps,
        std::vector<core::LidarPointXYZIRT>& out_cleaned_cloud,
        const CleanupConfig& config = CleanupConfig{});

    // End-to-end cleanup pipeline on an already-assembled world-frame cloud (e.g. terrestrial station)
    static CleanupStats runCloudCleanupPipeline(
        const std::vector<core::LidarPointXYZIRT>& input_cloud,
        std::vector<core::LidarPointXYZIRT>& out_cleaned_cloud,
        const CleanupConfig& config = CleanupConfig{});
};

}  // namespace processing
}  // namespace avscan
