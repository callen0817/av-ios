#pragma once

#include "avscan/core/Types.hpp"
#include "avscan/math/SophusLie.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace avscan {
namespace sensors {

enum class DepthProviderType : uint8_t {
    MONOCULAR_COREML = 0,     // iOS / macOS Core ML monocular depth (e.g., DepthAnything / DepthPro)
    MONOCULAR_ONNX_TRT = 1,   // Jetson / Windows / Linux ONNX Runtime / TensorRT monocular depth
    APPLE_NATIVE_LIDAR = 2,   // Supported iPhone Pro / iPad Pro dToF LiDAR via ARKit sceneDepth
    STEREO_CAMERA = 3,        // Hardware stereo depth (e.g., Orbbec Gemini 336L)
    PHYSICAL_LIDAR = 4        // External 3D LiDAR (e.g., RoboSense Airy)
};

enum class DepthMeasurementClass : uint8_t {
    MEASURED = 0,             // Physically measured metric depth (LiDAR / active stereo)
    INFERRED = 1              // Neural / monocular inferred depth (requires metric anchoring)
};

enum class DepthUnits : uint8_t {
    METERS = 0,
    MILLIMETERS = 1,
    RELATIVE_INVERSE_DEPTH = 2
};

// Standardized platform-independent depth frame consumed by AV SLAM / FAST-LIVO2
struct DepthFrame {
    uint64_t frame_id{0};
    double timestamp_sec{0.0};
    int width{0};
    int height{0};
    std::vector<float> depth_data;       // Row-major [height * width]
    std::vector<float> confidence_data;  // Optional row-major [height * width] in [0.0, 1.0]

    core::CameraIntrinsics intrinsics{};
    math::SE3d T_cam_lidar{math::SE3d::Identity()};

    DepthProviderType provider_type{DepthProviderType::MONOCULAR_COREML};
    DepthMeasurementClass measurement_class{DepthMeasurementClass::INFERRED};
    DepthUnits units{DepthUnits::METERS};

    double metric_scale{1.0};
    double metric_shift{0.0};
    bool is_metric_anchored{false};
    float mean_confidence{0.0f};

    bool valid() const {
        return width > 0 && height > 0 &&
               depth_data.size() == static_cast<size_t>(width * height);
    }

    // Returns metric depth in meters at pixel (u, v), or -1.0f if invalid
    float sampleMetricDepthMeters(int u, int v) const;
};

struct DepthComputeTier {
    std::string chip_family{"Generic"};
    double default_fps{3.0};
    double min_fps{1.0};
    double max_fps{15.0};
    int preferred_input_width{384};
    int preferred_input_height{288};
};

// Abstract interchangeable depth provider interface (runs asynchronously from SLAM)
class IDepthProvider {
public:
    virtual ~IDepthProvider() = default;

    virtual std::string providerName() const = 0;
    virtual DepthProviderType providerType() const = 0;
    virtual DepthMeasurementClass measurementClass() const = 0;
    virtual bool isAvailable() const = 0;

    virtual double targetInferenceFps() const = 0;
    virtual void setTargetInferenceFps(double fps) = 0;

    // Submit an RGB frame for asynchronous depth estimation (returns false if throttled/busy)
    virtual bool submitRgbFrame(const core::RgbFrame& rgb_frame,
                                const math::SE3d& T_cam_lidar) = 0;

    // Pop the most recent completed depth frame if available
    virtual bool popLatestDepthFrame(DepthFrame& out_frame) = 0;
};

// Metric scale aligner: anchors relative/inferred monocular depth to sparse Airy LiDAR points
// and back-projects dense metric colored points.
class LidarAnchoredDepthAligner {
public:
    // Computes adaptive inference tier for Apple Silicon (A14..M4) and Jetson/Desktop GPUs
    static DepthComputeTier computeTierForHardware(const std::string& machine_or_chip);

    // Fits linear metric scale/shift (z_metric = scale * d_raw + shift) against projected LiDAR points
    // Returns true if sufficient correspondences (>= 12) produced a stable metric anchor.
    static bool anchorToLidarPoints(DepthFrame& io_depth_frame,
                                    const std::vector<core::LidarPointXYZIRT>& lidar_points_in_lidar_frame,
                                    const math::SE3d& T_cam_lidar,
                                    double* out_rmse_m = nullptr,
                                    size_t* out_inliers = nullptr);

    // Back-projects an anchored metric DepthFrame (+ optional RGB) into 3D points in camera frame
    static std::vector<core::LidarPointXYZIRT> backProjectToPointCloud(
        const DepthFrame& depth_frame,
        const std::optional<core::RgbFrame>& rgb_frame,
        int pixel_stride = 4,
        float min_depth_m = 0.25f,
        float max_depth_m = 12.0f,
        float min_confidence = 0.35f);
};

}  // namespace sensors
}  // namespace avscan
