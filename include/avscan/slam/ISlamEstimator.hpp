#pragma once

#include "avscan/core/Types.hpp"
#include "avscan/sync/TimeSynchronizer.hpp"
#include <string>
#include <vector>

namespace avscan {
namespace slam {

struct EstimatorMetrics {
    uint64_t frames_processed{0};
    size_t deskewed_points_count{0};
    size_t geometric_correspondences{0};
    size_t visual_patch_correspondences{0};
    double geometric_rmse_m{0.0};
    double photometric_rmse{0.0};
    double processing_time_ms{0.0};
    double min_observability_eigenvalue{1.0};
    bool is_degenerate{false};
    bool visual_update_applied{false};
    bool keyframe_emitted{false};
};

// Abstract estimator interface isolating SLAM front-end implementation from proprietary app logic
class ISlamEstimator {
public:
    virtual ~ISlamEstimator() = default;

    virtual void setFactoryCalibration(const core::AiryFactoryCalibration& calib) = 0;
    virtual void setExtrinsicCalibration(const core::ExtrinsicCalibrationProfile& profile) = 0;
    virtual void setVisualFusionEnabled(bool enabled) = 0;

    virtual bool processBundle(const sync::SyncBundle& bundle,
                               core::LidarFrame& out_deskewed_body_frame,
                               core::LidarFrame& out_world_frame,
                               EstimatorMetrics& out_metrics) = 0;

    virtual core::SlamState getCurrentState() const = 0;
    virtual void resetPose(const math::SE3d& new_T_world_body) = 0;
    virtual void reset() = 0;
    virtual std::string getLicenseIdentifier() const = 0;
};

}  // namespace slam
}  // namespace avscan
