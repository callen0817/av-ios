#pragma once

#include "avscan/core/Types.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace avscan {
namespace slam {

struct VoxelKey {
    int64_t x{0}, y{0}, z{0};

    bool operator==(const VoxelKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct VoxelKeyHash {
    size_t operator()(const VoxelKey& k) const {
        uint64_t h1 = static_cast<uint64_t>(k.x) * 73856093ULL;
        uint64_t h2 = static_cast<uint64_t>(k.y) * 19349663ULL;
        uint64_t h3 = static_cast<uint64_t>(k.z) * 83492791ULL;
        return static_cast<size_t>(h1 ^ h2 ^ h3);
    }
};

struct VisualPatch {
    math::Vec3d center_world{0.0, 0.0, 0.0};
    math::Vec3d normal_world{0.0, 0.0, 1.0};
    math::SE3d ref_T_cam_world{math::SE3d::Identity()};
    core::CameraIntrinsics ref_intrinsics{};
    std::array<float, 64> patch_intensities{};  // 8x8 local photometric patch
    double ref_exposure_sec{0.008};
    uint64_t last_updated_frame{0};
    bool valid{false};
};

struct VoxelPlaneNode {
    std::vector<math::Vec3d> points;
    math::Vec3d mean{0.0, 0.0, 0.0};
    math::Mat3d covariance{math::Mat3d::Zero()};
    math::Vec3d normal{0.0, 0.0, 1.0};
    double d_coeff{0.0};
    double planarity{0.0};        // [0, 1] score based on smallest vs intermediate eigenvalue
    double min_eigenvalue{1.0};   // Plane thickness variance (m^2)
    bool is_valid_plane{false};
    bool dirty{false};
    uint64_t last_access_frame{0};
    uint32_t observation_pass_count{0};
    uint32_t free_space_violations{0};
    VisualPatch visual_patch{};
};

struct PlaneCorrespondence {
    math::Vec3d point_body{0.0, 0.0, 0.0};
    math::Vec3d point_world{0.0, 0.0, 0.0};
    math::Vec3d plane_mean{0.0, 0.0, 0.0};
    math::Vec3d plane_normal{0.0, 0.0, 1.0};
    double signed_distance{0.0};
    double measurement_variance{0.001};
    double huber_weight{1.0};
    bool valid{false};
};

class VoxelHashMap {
public:
    explicit VoxelHashMap(double voxel_size_m = 0.25, size_t max_points_per_voxel = 30, size_t max_active_voxels = 40000);

    void setVoxelSize(double voxel_size_m);
    double getVoxelSize() const { return voxel_size_m_; }

    VoxelKey pointToKey(const math::Vec3d& p) const;

    // Inserts world-frame points into the voxel map and updates incremental plane estimates
    void insertPoints(const std::vector<core::LidarPointXYZIRT>& world_points, uint64_t frame_id);

    // Queries nearest planar surface for a world-frame point (searches 7-voxel neighborhood)
    bool queryPlaneCorrespondence(const math::Vec3d& p_body,
                                  const math::Vec3d& p_world,
                                  double max_plane_dist_m,
                                  PlaneCorrespondence& out_corr) const;

    // Updates or attaches a direct visual patch to the voxel containing p_world
    void updateVisualPatch(const math::Vec3d& p_world, const VisualPatch& patch);

    // Collects active visual patches within radius of sensor position
    std::vector<VisualPatch> getActiveVisualPatches(const math::Vec3d& sensor_pos_world, double max_radius_m) const;

    // Evicts stale voxels beyond active horizon to respect A14 Bionic RAM budget
    size_t pruneDistantOrStaleVoxels(const math::Vec3d& center_world, double max_radius_m, uint64_t current_frame_id);

    size_t numActiveVoxels() const { return voxels_.size(); }
    size_t numValidPlanes() const;
    void clear() { voxels_.clear(); }

    const std::unordered_map<VoxelKey, VoxelPlaneNode, VoxelKeyHash>& getVoxels() const { return voxels_; }

private:
    static void recomputePlaneParameters(VoxelPlaneNode& node);

    double voxel_size_m_{0.25};
    double inv_voxel_size_{4.0};
    size_t max_points_per_voxel_{30};
    size_t max_active_voxels_{40000};
    std::unordered_map<VoxelKey, VoxelPlaneNode, VoxelKeyHash> voxels_;
};

}  // namespace slam
}  // namespace avscan
