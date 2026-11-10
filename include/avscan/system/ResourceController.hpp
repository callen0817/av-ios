#pragma once

#include "avscan/core/Types.hpp"
#include "avscan/storage/ProjectStore.hpp"
#include <cstdint>
#include <vector>

namespace avscan {
namespace system {

// Mirrors iOS NSProcessInfoThermalState
enum class ThermalState : uint8_t {
    NOMINAL = 0,
    FAIR = 1,
    SERIOUS = 2,
    CRITICAL = 3
};

struct WorkloadPolicy {
    core::OperatingMode mode{core::OperatingMode::MAPPING_DENSE};
    int lidar_point_stride{2};
    int camera_target_fps{30};
    int metal_render_target_fps{30};
    size_t max_in_ram_keyframes{60};
    bool allow_background_loop_closure{true};
    bool allow_visual_patch_extraction{true};
};

class ResourceController {
public:
    explicit ResourceController(size_t ram_budget_mb = 1200);

    void setOperatingMode(core::OperatingMode mode) { current_mode_ = mode; }
    core::OperatingMode getOperatingMode() const { return current_mode_; }

    // Computes adaptive processing policy based on thermal state, active memory usage, and operating mode
    WorkloadPolicy computeAdaptivePolicy(ThermalState thermal_state,
                                         size_t current_ram_usage_mb);

    // Enforces keyframe RAM budget by archiving older submaps to disk via ProjectStore
    size_t enforceSubmapMemoryBudget(std::vector<core::KeyframeSubmap>& io_submaps,
                                     const storage::ProjectStore& store,
                                     size_t max_ram_submaps) const;

private:
    size_t ram_budget_mb_{1200};
    core::OperatingMode current_mode_{core::OperatingMode::MAPPING_DENSE};
};

}  // namespace system
}  // namespace avscan
