#pragma once

#include "avscan/core/Types.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace avscan {
namespace backend {

enum class FactorType : uint8_t {
    ODOMETRY = 0,
    LOOP_CLOSURE = 1
};

struct PoseGraphFactor {
    uint64_t from_id{0};
    uint64_t to_id{0};
    FactorType type{FactorType::ODOMETRY};
    math::SE3d T_from_to_measured{math::SE3d::Identity()};
    std::array<double, 6> information_diag{100.0, 100.0, 100.0, 400.0, 400.0, 400.0};
    double robust_huber_delta{0.25};
    bool active{true};
};

struct OptimizationReport {
    bool converged{false};
    int iterations{0};
    double initial_chi2_error{0.0};
    double final_chi2_error{0.0};
    double max_pose_correction_m{0.0};
    size_t num_keyframes{0};
    size_t num_loop_factors{0};
};

class PoseGraphOptimizer {
public:
    PoseGraphOptimizer() = default;

    // Adds a new keyframe submap (and sequential odometry constraint from previous keyframe if any)
    void addKeyframeSubmap(const core::KeyframeSubmap& submap,
                           const std::array<double, 6>& odom_info = {200.0, 200.0, 200.0, 800.0, 800.0, 800.0});

    // Adds a geometrically verified loop closure constraint (never accepts unverified constraints)
    bool addVerifiedLoopFactor(uint64_t from_id,
                               uint64_t to_id,
                               const math::SE3d& measured_T_from_to,
                               const std::array<double, 6>& info_diag,
                               bool is_geometrically_verified);

    // Executes nonlinear Levenberg-Marquardt pose-graph optimization over SE(3) with Huber/DCS robust kernels
    // Updates all keyframe submap global poses in place
    OptimizationReport optimizeGlobalPoseGraph(int max_iterations = 25);

    // Reconstructs the globally consistent point cloud from all corrected keyframe submaps
    std::vector<core::LidarPointXYZIRT> buildGloballyConsistentPointCloud(size_t downsample_stride = 1) const;

    const std::vector<core::KeyframeSubmap>& getSubmaps() const { return submaps_; }
    std::vector<core::KeyframeSubmap>& getMutableSubmaps() { return submaps_; }
    const std::vector<PoseGraphFactor>& getFactors() const { return factors_; }
    size_t numLoopClosures() const;
    void clear();

private:
    static math::Vec6d computeFactorResidual(const math::SE3d& T_w_i,
                                             const math::SE3d& T_w_j,
                                             const math::SE3d& T_i_j_meas);

    mutable std::mutex mutex_;
    std::vector<core::KeyframeSubmap> submaps_;
    std::unordered_map<uint64_t, size_t> id_to_index_;
    std::vector<PoseGraphFactor> factors_;
};

}  // namespace backend
}  // namespace avscan
