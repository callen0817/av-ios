#pragma once

#include "avscan/core/Types.hpp"
#include <vector>

namespace avscan {
namespace backend {

struct GicpVerificationConfig {
    double voxel_resolution_m{0.30};
    double max_correspondence_dist_m{0.50};
    int max_iterations{20};
    double min_inlier_overlap_ratio{0.35};
    double max_fitness_rmse_m{0.18};
};

struct GicpVerificationResult {
    bool verified{false};
    math::SE3d T_target_source{math::SE3d::Identity()};
    double fitness_rmse_m{10.0};
    double inlier_overlap_ratio{0.0};
    size_t inlier_count{0};
    int iterations_run{0};
    std::array<double, 6> information_diagonal{100.0, 100.0, 100.0, 400.0, 400.0, 400.0};
};

class GicpVerifier {
public:
    explicit GicpVerifier(const GicpVerificationConfig& config = GicpVerificationConfig{});

    // Aligns source keyframe submap into target keyframe submap starting from initial_guess_T_target_source
    // Strictly rejects matches whose overlap < min_inlier_overlap_ratio or RMSE > max_fitness_rmse_m
    GicpVerificationResult verifyLoopConstraint(
        const std::vector<core::LidarPointXYZIRT>& source_local_points,
        const std::vector<core::LidarPointXYZIRT>& target_local_points,
        const math::SE3d& initial_guess_T_target_source) const;

private:
    GicpVerificationConfig config_;
};

}  // namespace backend
}  // namespace avscan
