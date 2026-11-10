#pragma once

#include "avscan/backend/GicpVerifier.hpp"
#include "avscan/backend/PoseGraphOptimizer.hpp"
#include "avscan/backend/ScanContext.hpp"
#include "avscan/core/Types.hpp"
#include "avscan/slam/ISlamEstimator.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace avscan {
namespace quality {

struct ScanQualityReport {
    double overall_quality_score{1.0};       // [0.0, 1.0]
    double scan_overlap_ratio{1.0};
    double registration_rmse_m{0.0};
    double estimated_drift_risk_m{0.0};
    double surface_coverage_pct{100.0};
    bool tracking_lost{false};
    bool degenerate_corridor_warning{false};
    bool rescan_recommended{false};
    std::string operator_guidance_message{"Scan quality nominal."};
};

struct FloorPlanGrid2D {
    double origin_x_m{-25.0};
    double origin_y_m{-25.0};
    double resolution_m{0.10};               // 10 cm per grid cell
    int width_cells{500};
    int height_cells{500};
    double estimated_floor_z_m{0.0};
    double estimated_ceiling_z_m{2.8};
    // Cell values: 0 = unknown, 1 = free floor space, 2 = wall/obstacle, 3 = low-density rescan alert
    std::vector<uint8_t> occupancy_cells;
    std::vector<math::Vec2d> trajectory_xy;
    std::vector< std::pair<math::Vec2d, math::Vec2d> > loop_closure_links;
};

struct ReferenceDistanceCheck {
    std::string label{};
    math::Vec3d map_point_a{0, 0, 0};
    math::Vec3d map_point_b{0, 0, 0};
    double reference_tape_distance_m{0.0};
    double measured_map_distance_m{0.0};
    double signed_error_m{0.0};
    double relative_error_pct{0.0};
};

struct AccuracyQualificationSummary {
    size_t num_checks{0};
    double rmse_m{0.0};
    double mae_m{0.0};
    double max_abs_error_m{0.0};
    double mean_relative_error_pct{0.0};
    std::vector<ReferenceDistanceCheck> checks;
};

class QualityMonitor {
public:
    QualityMonitor() = default;

    // Evaluates current frame tracking health, coverage, drift accumulation, and operator guidance
    ScanQualityReport evaluateFrameQuality(const core::SlamState& current_state,
                                           const slam::EstimatorMetrics& metrics,
                                           double unclosed_trajectory_length_m);

    // Attempts automatic 6-DOF relocalization against saved Scan Context descriptors and submaps
    bool attemptRelocalization(const core::LidarFrame& current_local_scan,
                               const ScanContextDatabase& sc_db,
                               const std::vector<core::KeyframeSubmap>& submaps,
                               const GicpVerifier& gicp,
                               math::SE3d& out_recovered_T_world_body,
                               uint64_t& out_matched_keyframe_id,
                               double& out_fitness_rmse_m) const;

    // Filters transient dynamic objects (pedestrians/vehicles) by multi-pass voxel consistency
    static std::vector<core::LidarPointXYZIRT> filterDynamicObjects(
        const std::vector<core::KeyframeSubmap>& submaps,
        double voxel_size_m = 0.25,
        uint32_t min_static_observations = 2);

    // Generates a live 2D architectural floor plan from optimized LiDAR geometry and trajectory
    static FloorPlanGrid2D generateLiveFloorPlan(
        const std::vector<core::LidarPointXYZIRT>& global_points,
        const std::vector<core::KeyframeSubmap>& submaps,
        const std::vector<backend::PoseGraphFactor>& factors,
        double resolution_m = 0.10);

    // Evaluates independent reference distances against map coordinates
    static AccuracyQualificationSummary evaluateReferenceDistances(
        const std::vector< std::tuple<std::string, math::Vec3d, math::Vec3d, double> >& reference_measurements);

private:
    uint32_t consecutive_poor_frames_{0};
};

}  // namespace quality
}  // namespace avscan
