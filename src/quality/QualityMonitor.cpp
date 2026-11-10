#include "avscan/quality/QualityMonitor.hpp"
#include "avscan/slam/VoxelHashMap.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace avscan {
namespace quality {

ScanQualityReport QualityMonitor::evaluateFrameQuality(const core::SlamState& current_state,
                                                       const slam::EstimatorMetrics& metrics,
                                                       double unclosed_trajectory_length_m) {
    ScanQualityReport rep;
    rep.registration_rmse_m = metrics.geometric_rmse_m;
    rep.degenerate_corridor_warning = metrics.is_degenerate;
    rep.estimated_drift_risk_m = 0.008 * unclosed_trajectory_length_m + current_state.position_uncertainty_m;

    if (metrics.deskewed_points_count > 0) {
        rep.scan_overlap_ratio = std::clamp(
            static_cast<double>(metrics.geometric_correspondences * 2) /
                static_cast<double>(metrics.deskewed_points_count),
            0.0, 1.0);
    } else {
        rep.scan_overlap_ratio = 0.0;
    }

    bool poor_frame = (metrics.geometric_correspondences < 12) || (metrics.geometric_rmse_m > 0.35);
    if (poor_frame) {
        consecutive_poor_frames_++;
    } else {
        consecutive_poor_frames_ = 0;
    }

    rep.tracking_lost = (consecutive_poor_frames_ >= 3) ||
                        (current_state.status == core::TrackingStatus::LOST_RELOCALIZING);

    double rmse_score = std::clamp(1.0 - (metrics.geometric_rmse_m / 0.25), 0.0, 1.0);
    double overlap_score = std::clamp(rep.scan_overlap_ratio / 0.50, 0.0, 1.0);
    double drift_score = std::clamp(1.0 - (rep.estimated_drift_risk_m / 1.0), 0.2, 1.0);

    rep.overall_quality_score = 0.45 * rmse_score + 0.35 * overlap_score + 0.20 * drift_score;
    if (rep.degenerate_corridor_warning) {
        rep.overall_quality_score *= 0.75;
    }
    if (rep.tracking_lost) {
        rep.overall_quality_score = 0.0;
    }

    rep.surface_coverage_pct = std::clamp(overlap_score * 100.0, 0.0, 100.0);
    rep.rescan_recommended = (rep.overall_quality_score < 0.55) || rep.degenerate_corridor_warning;

    if (rep.tracking_lost) {
        rep.operator_guidance_message = "Tracking lost. Hold scanner steady or return to a previously scanned room for relocalization.";
    } else if (rep.degenerate_corridor_warning) {
        rep.operator_guidance_message = "Geometric degeneracy detected (featureless corridor). Keep textured surfaces in camera FOV and move smoothly.";
    } else if (rep.estimated_drift_risk_m > 0.35) {
        rep.operator_guidance_message = "Accumulated open-loop distance high. Revisit a known room or hallway to trigger loop closure.";
    } else if (rep.scan_overlap_ratio < 0.30) {
        rep.operator_guidance_message = "Low surface overlap. Slow rotation speed and perform a second pass over occluded walls.";
    } else {
        rep.operator_guidance_message = "Scan quality nominal.";
    }

    return rep;
}

bool QualityMonitor::attemptRelocalization(
    const core::LidarFrame& current_local_scan,
    const backend::ScanContextDatabase& sc_db,
    const std::vector<core::KeyframeSubmap>& submaps,
    const backend::GicpVerifier& gicp,
    math::SE3d& out_recovered_T_world_body,
    uint64_t& out_matched_keyframe_id,
    double& out_fitness_rmse_m) const {
    out_matched_keyframe_id = 0;
    out_fitness_rmse_m = 10.0;

    if (current_local_scan.points.empty() || sc_db.size() == 0 || submaps.empty()) {
        return false;
    }

    auto query_desc = backend::ScanContextDatabase::computeDescriptor(
        0, current_local_scan.timestamp_end_sec, current_local_scan.points);
    auto cand = sc_db.findRelocalizationCandidate(query_desc);
    if (!cand.valid) {
        return false;
    }

    const core::KeyframeSubmap* target_submap = nullptr;
    for (const auto& sm : submaps) {
        if (sm.keyframe_id == cand.match_keyframe_id) {
            target_submap = &sm;
            break;
        }
    }
    if (!target_submap || target_submap->local_points.empty()) {
        return false;
    }

    math::SO3d R_yaw = math::SO3d::exp(math::Vec3d(0.0, 0.0, cand.initial_yaw_shift_rad));
    math::SE3d T_target_source_init(R_yaw, math::Vec3d(0.0, 0.0, 0.0));

    auto gicp_res = gicp.verifyLoopConstraint(
        current_local_scan.points, target_submap->local_points, T_target_source_init);
    if (!gicp_res.verified) {
        return false;
    }

    out_recovered_T_world_body = target_submap->T_world_keyframe * gicp_res.T_target_source;
    out_matched_keyframe_id = target_submap->keyframe_id;
    out_fitness_rmse_m = gicp_res.fitness_rmse_m;
    return true;
}

std::vector<core::LidarPointXYZIRT> QualityMonitor::filterDynamicObjects(
    const std::vector<core::KeyframeSubmap>& submaps,
    double voxel_size_m,
    uint32_t min_static_observations) {
    if (submaps.empty()) return {};

    double inv_v = 1.0 / std::max(0.05, voxel_size_m);
    auto toKey = [inv_v](double x, double y, double z) -> slam::VoxelKey {
        return {
            static_cast<int64_t>(std::floor(x * inv_v)),
            static_cast<int64_t>(std::floor(y * inv_v)),
            static_cast<int64_t>(std::floor(z * inv_v))
        };
    };

    struct VoxelStats {
        uint32_t hit_submaps{0};
        uint32_t pass_through_rays{0};
        uint64_t last_submap_id{0};
    };
    std::unordered_map<slam::VoxelKey, VoxelStats, slam::VoxelKeyHash> occ_map;

    for (const auto& sm : submaps) {
        math::Vec3d sensor_origin = sm.T_world_keyframe.t;
        for (const auto& pt : sm.local_points) {
            math::Vec3d pw = sm.T_world_keyframe.transform(math::Vec3d(pt.x, pt.y, pt.z));
            slam::VoxelKey hit_k = toKey(pw.x, pw.y, pw.z);
            auto& st = occ_map[hit_k];
            if (st.last_submap_id != sm.keyframe_id) {
                st.hit_submaps++;
                st.last_submap_id = sm.keyframe_id;
            }

            // Sample midpoint along ray from sensor to hit point for free-space violation check
            math::Vec3d ray = pw - sensor_origin;
            double len = ray.norm();
            if (len > voxel_size_m * 3.0) {
                math::Vec3d mid = sensor_origin + ray * 0.5;
                slam::VoxelKey mid_k = toKey(mid.x, mid.y, mid.z);
                if (!(mid_k == hit_k)) {
                    occ_map[mid_k].pass_through_rays++;
                }
            }
        }
    }

    std::vector<core::LidarPointXYZIRT> static_points;
    for (const auto& sm : submaps) {
        for (const auto& pt : sm.local_points) {
            math::Vec3d pw = sm.T_world_keyframe.transform(math::Vec3d(pt.x, pt.y, pt.z));
            slam::VoxelKey k = toKey(pw.x, pw.y, pw.z);
            auto it = occ_map.find(k);
            if (it != occ_map.end()) {
                bool is_transient = (it->second.hit_submaps < min_static_observations) &&
                                    (it->second.pass_through_rays > it->second.hit_submaps);
                if (is_transient) continue;
            }
            core::LidarPointXYZIRT out_pt = pt;
            out_pt.x = pw.x;
            out_pt.y = pw.y;
            out_pt.z = pw.z;
            static_points.push_back(out_pt);
        }
    }
    return static_points;
}

FloorPlanGrid2D QualityMonitor::generateLiveFloorPlan(
    const std::vector<core::LidarPointXYZIRT>& global_points,
    const std::vector<core::KeyframeSubmap>& submaps,
    const std::vector<backend::PoseGraphFactor>& factors,
    double resolution_m) {
    FloorPlanGrid2D plan;
    plan.resolution_m = std::max(0.05, resolution_m);

    if (global_points.empty()) {
        plan.occupancy_cells.assign(static_cast<size_t>(plan.width_cells * plan.height_cells), 0);
        return plan;
    }

    double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9, min_z = 1e9, max_z = -1e9;
    for (const auto& pt : global_points) {
        min_x = std::min(min_x, pt.x); max_x = std::max(max_x, pt.x);
        min_y = std::min(min_y, pt.y); max_y = std::max(max_y, pt.y);
        min_z = std::min(min_z, pt.z); max_z = std::max(max_z, pt.z);
    }

    // Estimate floor elevation from lower 10th percentile of Z values
    plan.estimated_floor_z_m = min_z + 0.05 * (max_z - min_z);
    plan.estimated_ceiling_z_m = std::max(plan.estimated_floor_z_m + 2.2, max_z - 0.1);

    double margin = 1.5;
    plan.origin_x_m = min_x - margin;
    plan.origin_y_m = min_y - margin;
    plan.width_cells = std::clamp(static_cast<int>(std::ceil((max_x - min_x + 2.0 * margin) / plan.resolution_m)), 32, 2048);
    plan.height_cells = std::clamp(static_cast<int>(std::ceil((max_y - min_y + 2.0 * margin) / plan.resolution_m)), 32, 2048);
    plan.occupancy_cells.assign(static_cast<size_t>(plan.width_cells * plan.height_cells), 0);

    std::vector<uint16_t> wall_counts(static_cast<size_t>(plan.width_cells * plan.height_cells), 0);
    double wall_z_min = plan.estimated_floor_z_m + 0.25;
    double wall_z_max = plan.estimated_floor_z_m + 2.10;

    for (const auto& pt : global_points) {
        int gx = static_cast<int>(std::floor((pt.x - plan.origin_x_m) / plan.resolution_m));
        int gy = static_cast<int>(std::floor((pt.y - plan.origin_y_m) / plan.resolution_m));
        if (gx < 0 || gx >= plan.width_cells || gy < 0 || gy >= plan.height_cells) continue;
        size_t idx = static_cast<size_t>(gy * plan.width_cells + gx);

        if (pt.z >= wall_z_min && pt.z <= wall_z_max) {
            wall_counts[idx]++;
            plan.occupancy_cells[idx] = (wall_counts[idx] >= 2) ? 2 : 3;
        } else if (pt.z < wall_z_min && plan.occupancy_cells[idx] == 0) {
            plan.occupancy_cells[idx] = 1;  // Floor free space
        }
    }

    std::unordered_map<uint64_t, math::Vec2d> kf_xy;
    for (const auto& sm : submaps) {
        math::Vec2d xy(sm.T_world_keyframe.t.x, sm.T_world_keyframe.t.y);
        plan.trajectory_xy.push_back(xy);
        kf_xy[sm.keyframe_id] = xy;
    }

    for (const auto& f : factors) {
        if (f.type == backend::FactorType::LOOP_CLOSURE && f.active) {
            auto it1 = kf_xy.find(f.from_id);
            auto it2 = kf_xy.find(f.to_id);
            if (it1 != kf_xy.end() && it2 != kf_xy.end()) {
                plan.loop_closure_links.push_back({it1->second, it2->second});
            }
        }
    }

    return plan;
}

AccuracyQualificationSummary QualityMonitor::evaluateReferenceDistances(
    const std::vector< std::tuple<std::string, math::Vec3d, math::Vec3d, double> >& reference_measurements) {
    AccuracyQualificationSummary summary;
    summary.num_checks = reference_measurements.size();
    if (reference_measurements.empty()) return summary;

    double sum_sq = 0.0;
    double sum_abs = 0.0;
    double sum_rel_pct = 0.0;

    for (const auto& item : reference_measurements) {
        ReferenceDistanceCheck chk;
        chk.label = std::get<0>(item);
        chk.map_point_a = std::get<1>(item);
        chk.map_point_b = std::get<2>(item);
        chk.reference_tape_distance_m = std::get<3>(item);
        chk.measured_map_distance_m = (chk.map_point_b - chk.map_point_a).norm();
        chk.signed_error_m = chk.measured_map_distance_m - chk.reference_tape_distance_m;
        double abs_err = std::abs(chk.signed_error_m);
        chk.relative_error_pct = (chk.reference_tape_distance_m > 1e-6)
            ? ((abs_err / chk.reference_tape_distance_m) * 100.0)
            : 0.0;

        sum_sq += abs_err * abs_err;
        sum_abs += abs_err;
        sum_rel_pct += chk.relative_error_pct;
        summary.max_abs_error_m = std::max(summary.max_abs_error_m, abs_err);
        summary.checks.push_back(chk);
    }

    double inv_n = 1.0 / static_cast<double>(summary.num_checks);
    summary.rmse_m = std::sqrt(sum_sq * inv_n);
    summary.mae_m = sum_abs * inv_n;
    summary.mean_relative_error_pct = sum_rel_pct * inv_n;
    return summary;
}

}  // namespace quality
}  // namespace avscan
