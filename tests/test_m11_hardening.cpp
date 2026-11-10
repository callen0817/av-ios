#include "avscan/system/ResourceController.hpp"
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace avscan;

bool run_test_m11_hardening() {
    system::ResourceController controller(1200); // 1200 MB budget for iPad A2696 (A14 Bionic)
    controller.setOperatingMode(core::OperatingMode::MAPPING_DENSE);

    // 1. Nominal thermal state
    const auto p_nominal = controller.computeAdaptivePolicy(system::ThermalState::NOMINAL, 500);
    if (p_nominal.camera_target_fps != 30 || p_nominal.lidar_point_stride != 1 || !p_nominal.allow_background_loop_closure) {
        std::printf("  [M11 FAIL] Nominal policy unexpected parameters\n");
        return false;
    }

    // 2. Serious thermal state throttling
    const auto p_serious = controller.computeAdaptivePolicy(system::ThermalState::SERIOUS, 950);
    if (p_serious.camera_target_fps >= p_nominal.camera_target_fps ||
        p_serious.lidar_point_stride <= p_nominal.lidar_point_stride) {
        std::printf("  [M11 FAIL] Serious thermal state did not throttle camera FPS and LiDAR stride\n");
        return false;
    }

    // 3. Critical thermal state protection
    const auto p_critical = controller.computeAdaptivePolicy(system::ThermalState::CRITICAL, 1150);
    if (p_critical.allow_background_loop_closure || p_critical.allow_visual_patch_extraction ||
        p_critical.metal_render_target_fps > 15) {
        std::printf("  [M11 FAIL] Critical thermal state did not shed heavy background & visual workloads\n");
        return false;
    }

    // 4. Test RAM submap budget enforcement (offloading old keyframes to disk)
    const std::string test_dir = "test_avscan_m11_budget";
    std::filesystem::remove_all(test_dir);
    storage::ProjectStore store;
    store.initializeProject(test_dir, "PROJ_M11", "RAM_Budget_Test");

    std::vector<core::KeyframeSubmap> submaps;
    for (uint64_t k = 0; k < 10; ++k) {
        core::KeyframeSubmap sm;
        sm.keyframe_id = k;
        sm.local_points.resize(50);
        submaps.push_back(sm);
    }

    const size_t archived_count = controller.enforceSubmapMemoryBudget(submaps, store, 4);
    if (archived_count != 6) {
        std::printf("  [M11 FAIL] enforceSubmapMemoryBudget expected to archive 6 submaps, archived %zu\n",
                    archived_count);
        return false;
    }

    size_t in_ram_count = 0;
    for (const auto& sm : submaps) {
        if (!sm.archived_to_disk && !sm.local_points.empty()) {
            in_ram_count++;
        }
    }
    if (in_ram_count != 4) {
        std::printf("  [M11 FAIL] Expected 4 submaps remaining in RAM, got %zu\n", in_ram_count);
        return false;
    }

    std::filesystem::remove_all(test_dir);
    return true;
}
