#include "avscan/system/ResourceController.hpp"
#include <algorithm>

namespace avscan {
namespace system {

ResourceController::ResourceController(size_t ram_budget_mb)
    : ram_budget_mb_(std::max<size_t>(256, ram_budget_mb)) {}

WorkloadPolicy ResourceController::computeAdaptivePolicy(ThermalState thermal_state,
                                                         size_t current_ram_usage_mb) {
    WorkloadPolicy policy;
    policy.mode = current_mode_;

    if (current_mode_ == core::OperatingMode::TRACKING_REALTIME) {
        policy.lidar_point_stride = 3;
        policy.camera_target_fps = 15;
        policy.metal_render_target_fps = 30;
        policy.max_in_ram_keyframes = 40;
        policy.allow_background_loop_closure = false;
        policy.allow_visual_patch_extraction = true;
    } else if (current_mode_ == core::OperatingMode::FINAL_REFINEMENT) {
        policy.lidar_point_stride = 1;
        policy.camera_target_fps = 0;
        policy.metal_render_target_fps = 15;
        policy.max_in_ram_keyframes = 120;
        policy.allow_background_loop_closure = true;
        policy.allow_visual_patch_extraction = false;
    } else {
        // MAPPING_DENSE default
        policy.lidar_point_stride = 2;
        policy.camera_target_fps = 30;
        policy.metal_render_target_fps = 30;
        policy.max_in_ram_keyframes = 60;
        policy.allow_background_loop_closure = true;
        policy.allow_visual_patch_extraction = true;
    }

    // Apply thermal throttling adjustments for sustained A14 Bionic stability
    if (thermal_state == ThermalState::FAIR) {
        policy.camera_target_fps = std::min(policy.camera_target_fps, 20);
        policy.max_in_ram_keyframes = std::min<size_t>(policy.max_in_ram_keyframes, 50);
    } else if (thermal_state == ThermalState::SERIOUS) {
        policy.lidar_point_stride = std::max(policy.lidar_point_stride, 3);
        policy.camera_target_fps = std::min(policy.camera_target_fps, 15);
        policy.metal_render_target_fps = 20;
        policy.max_in_ram_keyframes = std::min<size_t>(policy.max_in_ram_keyframes, 35);
    } else if (thermal_state == ThermalState::CRITICAL) {
        // Prioritize uninterrupted real-time pose tracking over dense visualization/mapping
        policy.mode = core::OperatingMode::TRACKING_REALTIME;
        policy.lidar_point_stride = 4;
        policy.camera_target_fps = 10;
        policy.metal_render_target_fps = 15;
        policy.max_in_ram_keyframes = 20;
        policy.allow_background_loop_closure = false;
        policy.allow_visual_patch_extraction = false;
    }

    if (current_ram_usage_mb > (ram_budget_mb_ * 85) / 100) {
        policy.max_in_ram_keyframes = std::min<size_t>(policy.max_in_ram_keyframes, 25);
        policy.lidar_point_stride = std::max(policy.lidar_point_stride, 3);
    }

    return policy;
}

size_t ResourceController::enforceSubmapMemoryBudget(std::vector<core::KeyframeSubmap>& io_submaps,
                                                     const storage::ProjectStore& store,
                                                     size_t max_ram_submaps) const {
    size_t in_ram_count = 0;
    for (const auto& sm : io_submaps) {
        if (!sm.local_points.empty()) in_ram_count++;
    }
    if (in_ram_count <= max_ram_submaps) {
        return 0;
    }

    size_t to_archive = in_ram_count - max_ram_submaps;
    size_t archived = 0;
    for (size_t i = 0; i < io_submaps.size() && archived < to_archive; ++i) {
        if (!io_submaps[i].local_points.empty()) {
            if (store.archiveSubmapToDisk(io_submaps[i])) {
                archived++;
            }
        }
    }
    return archived;
}

}  // namespace system
}  // namespace avscan
