#include "avscan/slam/VoxelHashMap.hpp"
#include <algorithm>
#include <cmath>

namespace avscan {
namespace slam {

VoxelHashMap::VoxelHashMap(double voxel_size_m, size_t max_points_per_voxel, size_t max_active_voxels)
    : voxel_size_m_(std::max(0.05, voxel_size_m)),
      inv_voxel_size_(1.0 / std::max(0.05, voxel_size_m)),
      max_points_per_voxel_(std::max<size_t>(5, max_points_per_voxel)),
      max_active_voxels_(std::max<size_t>(1000, max_active_voxels)) {}

void VoxelHashMap::setVoxelSize(double voxel_size_m) {
    voxel_size_m_ = std::max(0.05, voxel_size_m);
    inv_voxel_size_ = 1.0 / voxel_size_m_;
}

VoxelKey VoxelHashMap::pointToKey(const math::Vec3d& p) const {
    return {
        static_cast<int64_t>(std::floor(p.x * inv_voxel_size_)),
        static_cast<int64_t>(std::floor(p.y * inv_voxel_size_)),
        static_cast<int64_t>(std::floor(p.z * inv_voxel_size_))
    };
}

void VoxelHashMap::recomputePlaneParameters(VoxelPlaneNode& node) {
    node.dirty = false;
    if (node.points.size() < 5) {
        node.is_valid_plane = false;
        return;
    }

    math::Vec3d mean(0.0, 0.0, 0.0);
    for (const auto& p : node.points) {
        mean += p;
    }
    mean /= static_cast<double>(node.points.size());
    node.mean = mean;

    math::Mat3d cov = math::Mat3d::Zero();
    for (const auto& p : node.points) {
        math::Vec3d d = p - mean;
        cov = cov + math::Mat3d::OuterProduct(d, d);
    }
    cov = cov * (1.0 / static_cast<double>(node.points.size()));
    node.covariance = cov;

    math::SymEigen3d eig = math::SymEigen3d::compute(cov);
    double l0 = std::max(0.0, eig.eigenvalues[0]);
    double l1 = std::max(0.0, eig.eigenvalues[1]);
    double l2 = std::max(0.0, eig.eigenvalues[2]);

    node.min_eigenvalue = l0;
    node.normal = math::Vec3d(eig.eigenvectors(0, 0), eig.eigenvectors(1, 0), eig.eigenvectors(2, 0)).normalized();
    node.d_coeff = -node.normal.dot(node.mean);

    // Planarity: require smallest eigenvalue << second eigenvalue, and second eigenvalue non-degenerate
    if (l1 > 1e-4 && l0 < 0.08 * l1 && l0 < 0.0025) {
        node.planarity = std::clamp(1.0 - (l0 / l1), 0.0, 1.0);
        node.is_valid_plane = true;
    } else {
        node.planarity = 0.0;
        node.is_valid_plane = false;
    }
}

void VoxelHashMap::insertPoints(const std::vector<core::LidarPointXYZIRT>& world_points, uint64_t frame_id) {
    for (const auto& pt : world_points) {
        math::Vec3d pw(pt.x, pt.y, pt.z);
        VoxelKey key = pointToKey(pw);
        auto& node = voxels_[key];
        node.last_access_frame = frame_id;
        node.observation_pass_count++;

        if (node.points.size() < max_points_per_voxel_) {
            // Check minimum inter-point distance inside voxel to prevent redundant clustering
            bool too_close = false;
            for (const auto& existing : node.points) {
                if ((existing - pw).squaredNorm() < 0.0004) {  // 2 cm threshold
                    too_close = true;
                    break;
                }
            }
            if (!too_close) {
                node.points.push_back(pw);
                node.dirty = true;
            }
        }
    }

    for (auto& kv : voxels_) {
        if (kv.second.dirty) {
            recomputePlaneParameters(kv.second);
        }
    }
}

bool VoxelHashMap::queryPlaneCorrespondence(const math::Vec3d& p_body,
                                            const math::Vec3d& p_world,
                                            double max_plane_dist_m,
                                            PlaneCorrespondence& out_corr) const {
    out_corr.valid = false;
    VoxelKey base_key = pointToKey(p_world);

    static constexpr int64_t kOffsets[7][3] = {
        {0, 0, 0},
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1}
    };

    double best_abs_dist = max_plane_dist_m;
    const VoxelPlaneNode* best_node = nullptr;
    double best_signed_dist = 0.0;

    for (int i = 0; i < 7; ++i) {
        VoxelKey nk{base_key.x + kOffsets[i][0], base_key.y + kOffsets[i][1], base_key.z + kOffsets[i][2]};
        auto it = voxels_.find(nk);
        if (it == voxels_.end() || !it->second.is_valid_plane) continue;

        // Down-weight or reject voxels flagged as transient/dynamic objects
        if (it->second.free_space_violations > it->second.observation_pass_count * 2) {
            continue;
        }

        const VoxelPlaneNode& node = it->second;
        double signed_d = node.normal.dot(p_world - node.mean);
        double abs_d = std::abs(signed_d);
        if (abs_d < best_abs_dist && (p_world - node.mean).norm() < voxel_size_m_ * 1.75) {
            best_abs_dist = abs_d;
            best_signed_dist = signed_d;
            best_node = &node;
        }
    }

    if (!best_node) return false;

    out_corr.point_body = p_body;
    out_corr.point_world = p_world;
    out_corr.plane_mean = best_node->mean;
    out_corr.plane_normal = best_node->normal;
    out_corr.signed_distance = best_signed_dist;

    double range_sq = p_body.squaredNorm();
    // Airy ranging precision is ~1.5 cm (1 sigma); combine with plane thickness eigenvalue
    out_corr.measurement_variance = std::max(1e-4, 0.000225 + best_node->min_eigenvalue + 1e-5 * range_sq);

    // Huber robust weight (delta = 0.05 m)
    constexpr double kHuberDelta = 0.05;
    out_corr.huber_weight = (best_abs_dist <= kHuberDelta) ? 1.0 : (kHuberDelta / best_abs_dist);
    out_corr.valid = true;
    return true;
}

void VoxelHashMap::updateVisualPatch(const math::Vec3d& p_world, const VisualPatch& patch) {
    VoxelKey key = pointToKey(p_world);
    auto it = voxels_.find(key);
    if (it != voxels_.end()) {
        it->second.visual_patch = patch;
        it->second.visual_patch.valid = true;
    }
}

std::vector<VisualPatch> VoxelHashMap::getActiveVisualPatches(const math::Vec3d& sensor_pos_world, double max_radius_m) const {
    std::vector<VisualPatch> patches;
    double r2 = max_radius_m * max_radius_m;
    for (const auto& kv : voxels_) {
        if (kv.second.visual_patch.valid) {
            if ((kv.second.visual_patch.center_world - sensor_pos_world).squaredNorm() <= r2) {
                patches.push_back(kv.second.visual_patch);
            }
        }
    }
    return patches;
}

size_t VoxelHashMap::pruneDistantOrStaleVoxels(const math::Vec3d& center_world, double max_radius_m, uint64_t current_frame_id) {
    double r2 = max_radius_m * max_radius_m;
    size_t removed = 0;
    for (auto it = voxels_.begin(); it != voxels_.end(); ) {
        bool too_far = !it->second.points.empty() && ((it->second.mean - center_world).squaredNorm() > r2);
        bool too_old = (voxels_.size() > max_active_voxels_) &&
                       (current_frame_id > it->second.last_access_frame + 150);
        if (too_far || too_old) {
            it = voxels_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    return removed;
}

size_t VoxelHashMap::numValidPlanes() const {
    size_t count = 0;
    for (const auto& kv : voxels_) {
        if (kv.second.is_valid_plane) count++;
    }
    return count;
}

}  // namespace slam
}  // namespace avscan
