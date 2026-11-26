#include "avscan/sensors/IDepthProvider.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>

namespace avscan {
namespace sensors {

float DepthFrame::sampleMetricDepthMeters(int u, int v) const {
    if (!valid() || u < 0 || v < 0 || u >= width || v >= height) {
        return -1.0f;
    }
    float raw = depth_data[static_cast<size_t>(v * width + u)];
    if (!std::isfinite(raw) || raw <= 0.0f) {
        return -1.0f;
    }
    if (units == DepthUnits::METERS) {
        return is_metric_anchored ? static_cast<float>(raw * metric_scale + metric_shift) : raw;
    }
    if (units == DepthUnits::MILLIMETERS) {
        return raw * 0.001f;
    }
    // RELATIVE_INVERSE_DEPTH: requires metric anchor
    if (!is_metric_anchored) return -1.0f;
    double d = raw * metric_scale + metric_shift;
    return (d > 0.05 && std::isfinite(d)) ? static_cast<float>(d) : -1.0f;
}

DepthComputeTier LidarAnchoredDepthAligner::computeTierForHardware(const std::string& machine_or_chip) {
    std::string s = machine_or_chip;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // M-series iPads & Apple Silicon Macs / Jetson Orin
    if (s.find("ipad13,16") != std::string::npos || s.find("ipad13,17") != std::string::npos || // iPad Air 5 M1
        s.find("ipad14,") != std::string::npos || s.find("ipad16,") != std::string::npos ||     // iPad Air M2 / Pro M4
        s.find("m1") != std::string::npos || s.find("m2") != std::string::npos ||
        s.find("m3") != std::string::npos || s.find("m4") != std::string::npos ||
        s.find("orin") != std::string::npos || s.find("rtx") != std::string::npos) {
        return {"M-Series / Orin", 12.0, 5.0, 15.0, 518, 392};
    }
    // A17 Pro / A18 (iPhone 15 Pro / iPhone 16 series)
    if (s.find("iphone16,") != std::string::npos || s.find("iphone17,") != std::string::npos ||
        s.find("a17") != std::string::npos || s.find("a18") != std::string::npos) {
        return {"A17 Pro / A18", 8.0, 3.0, 10.0, 384, 288};
    }
    // A15 / A16 (iPhone 13/14/15 base, iPad mini 6)
    if (s.find("iphone14,") != std::string::npos || s.find("iphone15,") != std::string::npos ||
        s.find("a15") != std::string::npos || s.find("a16") != std::string::npos) {
        return {"A15 / A16", 4.0, 2.0, 5.0, 320, 240};
    }
    // A14 (iPad 10th Gen, iPad Air 4th Gen, iPhone 12)
    if (s.find("ipad13,18") != std::string::npos || s.find("ipad13,19") != std::string::npos ||
        s.find("ipad13,1") != std::string::npos || s.find("ipad13,2") != std::string::npos ||
        s.find("iphone13,") != std::string::npos || s.find("a14") != std::string::npos) {
        return {"A14 Bionic", 2.0, 1.0, 3.0, 256, 192};
    }
    return {"Generic", 3.0, 1.0, 6.0, 320, 240};
}

bool LidarAnchoredDepthAligner::anchorToLidarPoints(
    DepthFrame& io_depth_frame,
    const std::vector<core::LidarPointXYZIRT>& lidar_points_in_lidar_frame,
    const math::SE3d& T_cam_lidar,
    double* out_rmse_m,
    size_t* out_inliers) {
    if (!io_depth_frame.valid() || lidar_points_in_lidar_frame.empty()) {
        return false;
    }

    struct Corr {
        double d_raw;
        double z_lidar;
    };
    std::vector<Corr> corrs;
    corrs.reserve(256);

    for (const auto& pt : lidar_points_in_lidar_frame) {
        math::Vec3d p_cam = T_cam_lidar.transform(math::Vec3d(pt.x, pt.y, pt.z));
        if (p_cam.z < 0.3 || p_cam.z > 25.0) continue;
        if (!io_depth_frame.intrinsics.isInFieldOfView(p_cam, 4.0)) continue;

        math::Vec2d uv = io_depth_frame.intrinsics.project(p_cam, true);
        int u = static_cast<int>(std::round(uv.x));
        int v = static_cast<int>(std::round(uv.y));
        if (u < 0 || v < 0 || u >= io_depth_frame.width || v >= io_depth_frame.height) continue;

        size_t idx = static_cast<size_t>(v * io_depth_frame.width + u);
        float d_raw = io_depth_frame.depth_data[idx];
        if (!std::isfinite(d_raw) || d_raw <= 1e-4f) continue;
        if (!io_depth_frame.confidence_data.empty() &&
            io_depth_frame.confidence_data.size() == io_depth_frame.depth_data.size() &&
            io_depth_frame.confidence_data[idx] < 0.25f) {
            continue;
        }
        corrs.push_back({static_cast<double>(d_raw), p_cam.z});
    }

    if (corrs.size() < 12) {
        return false;
    }

    // Two-pass IRLS linear fit: z_lidar = scale * d_raw + shift
    double scale = 1.0;
    double shift = 0.0;
    for (int pass = 0; pass < 2; ++pass) {
        double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (const auto& c : corrs) {
            double w = 1.0;
            if (pass > 0) {
                double err = std::abs((scale * c.d_raw + shift) - c.z_lidar);
                w = (err <= 0.15) ? 1.0 : (0.15 / err);
            }
            sw += w;
            sx += w * c.d_raw;
            sy += w * c.z_lidar;
            sxx += w * c.d_raw * c.d_raw;
            sxy += w * c.d_raw * c.z_lidar;
        }
        double denom = sw * sxx - sx * sx;
        if (std::abs(denom) < 1e-9) {
            // Pure scale-only fallback if variance is degenerate
            if (sxx > 1e-9) {
                scale = sxy / sxx;
                shift = 0.0;
            } else {
                return false;
            }
        } else {
            scale = (sw * sxy - sx * sy) / denom;
            shift = (sy - scale * sx) / sw;
        }
    }

    if (!std::isfinite(scale) || !std::isfinite(shift) || scale <= 1e-4 || scale > 1000.0) {
        return false;
    }

    double sum_sq = 0.0;
    size_t inliers = 0;
    for (const auto& c : corrs) {
        double err = (scale * c.d_raw + shift) - c.z_lidar;
        if (std::abs(err) < 0.35) {
            sum_sq += err * err;
            inliers++;
        }
    }
    if (inliers < 10) return false;

    io_depth_frame.metric_scale = scale;
    io_depth_frame.metric_shift = shift;
    io_depth_frame.is_metric_anchored = true;
    io_depth_frame.T_cam_lidar = T_cam_lidar;
    if (out_rmse_m) *out_rmse_m = std::sqrt(sum_sq / static_cast<double>(inliers));
    if (out_inliers) *out_inliers = inliers;
    return true;
}

std::vector<core::LidarPointXYZIRT> LidarAnchoredDepthAligner::backProjectToPointCloud(
    const DepthFrame& depth_frame,
    const std::optional<core::RgbFrame>& rgb_frame,
    int pixel_stride,
    float min_depth_m,
    float max_depth_m,
    float min_confidence) {
    std::vector<core::LidarPointXYZIRT> pts;
    if (!depth_frame.valid()) return pts;

    int step = std::max(1, pixel_stride);
    const double fx = std::max(1.0, depth_frame.intrinsics.fx);
    const double fy = std::max(1.0, depth_frame.intrinsics.fy);
    const double cx = depth_frame.intrinsics.cx;
    const double cy = depth_frame.intrinsics.cy;

    const bool has_conf = (depth_frame.confidence_data.size() == depth_frame.depth_data.size());
    const bool has_rgb = rgb_frame.has_value() &&
                         rgb_frame->intrinsics.width > 0 && rgb_frame->intrinsics.height > 0 &&
                         rgb_frame->rgb_bytes.size() >= static_cast<size_t>(rgb_frame->intrinsics.width * rgb_frame->intrinsics.height * 3);

    for (int v = 0; v < depth_frame.height; v += step) {
        for (int u = 0; u < depth_frame.width; u += step) {
            size_t idx = static_cast<size_t>(v * depth_frame.width + u);
            if (has_conf && depth_frame.confidence_data[idx] < min_confidence) continue;

            float z = depth_frame.sampleMetricDepthMeters(u, v);
            if (z < min_depth_m || z > max_depth_m) continue;

            core::LidarPointXYZIRT p;
            p.x = (static_cast<double>(u) - cx) * z / fx;
            p.y = (static_cast<double>(v) - cy) * z / fy;
            p.z = static_cast<double>(z);
            p.range_m = std::sqrt(static_cast<float>(p.x * p.x + p.y * p.y + p.z * p.z));
            p.timestamp_sec = depth_frame.timestamp_sec;
            p.intensity = has_conf ? (depth_frame.confidence_data[idx] * 255.0f) : 180.0f;

            if (has_rgb) {
                int ru = std::clamp(static_cast<int>(std::round(static_cast<double>(u) * rgb_frame->intrinsics.width / depth_frame.width)),
                                    0, rgb_frame->intrinsics.width - 1);
                int rv = std::clamp(static_cast<int>(std::round(static_cast<double>(v) * rgb_frame->intrinsics.height / depth_frame.height)),
                                    0, rgb_frame->intrinsics.height - 1);
                size_t ridx = static_cast<size_t>((rv * rgb_frame->intrinsics.width + ru) * 3);
                p.r = rgb_frame->rgb_bytes[ridx + 0];
                p.g = rgb_frame->rgb_bytes[ridx + 1];
                p.b = rgb_frame->rgb_bytes[ridx + 2];
                p.colorized = true;
            }
            pts.push_back(p);
        }
    }
    return pts;
}

}  // namespace sensors
}  // namespace avscan
