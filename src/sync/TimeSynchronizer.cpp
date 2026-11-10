#include "avscan/sync/TimeSynchronizer.hpp"
#include <algorithm>
#include <cmath>

namespace avscan {
namespace sync {

TimeSynchronizer::TimeSynchronizer() = default;

void TimeSynchronizer::setCalibratedCameraTimeOffset(double offset_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    diag_.calibrated_cam_offset_sec = offset_sec;
}

double TimeSynchronizer::getCalibratedCameraTimeOffset() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return diag_.calibrated_cam_offset_sec;
}

void TimeSynchronizer::updateClockOffsetEstimate(double sensor_time_sec, double host_time_sec) {
    if (host_time_sec <= 0.0 || sensor_time_sec <= 0.0) return;
    double raw_offset = sensor_time_sec - host_time_sec;
    offset_window_.push_back(raw_offset);
    if (offset_window_.size() > 100) {
        offset_window_.pop_front();
    }

    // Use upper 90th percentile of (sensor_time - host_recv_time) corresponding to minimum network transit delay
    std::vector<double> sorted(offset_window_.begin(), offset_window_.end());
    std::sort(sorted.begin(), sorted.end());
    size_t idx = (sorted.size() * 9) / 10;
    diag_.estimated_clock_offset_sec = sorted[idx];

    double mean = 0.0;
    for (double v : sorted) mean += v;
    mean /= static_cast<double>(sorted.size());
    double var = 0.0;
    for (double v : sorted) var += (v - mean) * (v - mean);
    var /= static_cast<double>(sorted.size());
    diag_.clock_jitter_ms = std::sqrt(var) * 1000.0;
    diag_.clock_locked = (offset_window_.size() >= 10 && diag_.clock_jitter_ms < 25.0);
}

bool TimeSynchronizer::pushImu(const core::ImuMeasurement& imu, double host_recv_time_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_imu_timestamp_ > 0.0 && imu.timestamp_sec <= last_imu_timestamp_) {
        diag_.non_monotonic_imu_drops++;
        return false;
    }
    last_imu_timestamp_ = imu.timestamp_sec;
    updateClockOffsetEstimate(imu.timestamp_sec, host_recv_time_sec);

    imu_buffer_.push_back(imu);
    while (imu_buffer_.size() > 4000) {
        imu_buffer_.pop_front();
    }
    diag_.imu_samples_received++;
    return true;
}

bool TimeSynchronizer::pushLidarFrame(const core::LidarFrame& frame, double host_recv_time_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame.points.empty() || frame.timestamp_end_sec < frame.timestamp_start_sec) {
        return false;
    }
    if (last_lidar_timestamp_ > 0.0 && frame.timestamp_start_sec < last_lidar_timestamp_) {
        diag_.non_monotonic_lidar_drops++;
        return false;
    }
    last_lidar_timestamp_ = frame.timestamp_start_sec;
    updateClockOffsetEstimate(frame.timestamp_end_sec, host_recv_time_sec);

    lidar_buffer_.push_back(frame);
    while (lidar_buffer_.size() > 20) {
        lidar_buffer_.pop_front();
    }
    diag_.lidar_frames_received++;
    return true;
}

bool TimeSynchronizer::pushRgbFrame(const core::RgbFrame& rgb_frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    core::RgbFrame aligned = rgb_frame;
    // Map camera timestamp into LiDAR clock domain using software clock offset + calibrated temporal offset
    aligned.timestamp_sec = rgb_frame.timestamp_sec + diag_.estimated_clock_offset_sec + diag_.calibrated_cam_offset_sec;
    rgb_buffer_.push_back(std::move(aligned));
    while (rgb_buffer_.size() > 60) {
        rgb_buffer_.pop_front();
    }
    diag_.camera_frames_received++;
    return true;
}

bool TimeSynchronizer::popSyncBundle(SyncBundle& out_bundle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lidar_buffer_.empty()) {
        return false;
    }

    const core::LidarFrame& front_lidar = lidar_buffer_.front();
    // Wait until IMU buffer reaches end of scan (unless IMU buffer has at least 2 samples and no newer IMU is pending)
    if (!imu_buffer_.empty() && imu_buffer_.back().timestamp_sec < front_lidar.timestamp_end_sec &&
        lidar_buffer_.size() < 2) {
        return false;
    }

    out_bundle = SyncBundle{};
    out_bundle.lidar_frame = front_lidar;
    lidar_buffer_.pop_front();
    out_bundle.estimated_cam_to_lidar_offset_sec =
        diag_.estimated_clock_offset_sec + diag_.calibrated_cam_offset_sec;

    // Collect IMU measurements spanning [t_start - 0.015s, t_end + 0.005s]
    for (const auto& imu : imu_buffer_) {
        if (imu.timestamp_sec >= (out_bundle.lidar_frame.timestamp_start_sec - 0.015) &&
            imu.timestamp_sec <= (out_bundle.lidar_frame.timestamp_end_sec + 0.005)) {
            out_bundle.imu_measurements.push_back(imu);
        }
    }
    out_bundle.has_valid_imu_coverage = (out_bundle.imu_measurements.size() >= 2);

    // Prune older IMU samples
    while (!imu_buffer_.empty() &&
           imu_buffer_.front().timestamp_sec < (out_bundle.lidar_frame.timestamp_start_sec - 0.05)) {
        imu_buffer_.pop_front();
    }

    // Match closest camera frame within +/- 40 ms of scan midpoint
    double mid_time = 0.5 * (out_bundle.lidar_frame.timestamp_start_sec + out_bundle.lidar_frame.timestamp_end_sec);
    double best_dt = 0.045;
    int best_idx = -1;
    for (size_t i = 0; i < rgb_buffer_.size(); ++i) {
        double dt = std::abs(rgb_buffer_[i].timestamp_sec - mid_time);
        if (dt < best_dt) {
            best_dt = dt;
            best_idx = static_cast<int>(i);
        }
    }
    if (best_idx >= 0) {
        out_bundle.rgb_frame = rgb_buffer_[static_cast<size_t>(best_idx)];
        // Drop camera frames older than current scan
        while (!rgb_buffer_.empty() &&
               rgb_buffer_.front().timestamp_sec < out_bundle.lidar_frame.timestamp_start_sec) {
            rgb_buffer_.pop_front();
        }
    }

    diag_.bundles_emitted++;
    return true;
}

double TimeSynchronizer::estimateTemporalOffsetByCrossCorrelation(
    const std::vector< std::pair<double, double> >& imu_gyro_norms,
    const std::vector< std::pair<double, double> >& cam_rot_norms,
    double search_window_sec,
    double step_sec,
    double* out_peak_correlation) {
    if (imu_gyro_norms.size() < 5 || cam_rot_norms.size() < 5 || step_sec <= 0.0) {
        if (out_peak_correlation) *out_peak_correlation = 0.0;
        return 0.0;
    }

    auto interpImu = [&](double t) -> double {
        if (t <= imu_gyro_norms.front().first) return imu_gyro_norms.front().second;
        if (t >= imu_gyro_norms.back().first) return imu_gyro_norms.back().second;
        auto it = std::lower_bound(
            imu_gyro_norms.begin(), imu_gyro_norms.end(), t,
            [](const std::pair<double, double>& a, double val) { return a.first < val; });
        if (it == imu_gyro_norms.begin()) return it->second;
        auto prev = it - 1;
        double dt = it->first - prev->first;
        double alpha = (dt > 1e-9) ? ((t - prev->first) / dt) : 0.0;
        return prev->second + alpha * (it->second - prev->second);
    };

    double best_offset = 0.0;
    double best_ncc = -1.0;

    for (double dt = -search_window_sec; dt <= search_window_sec + 1e-9; dt += step_sec) {
        std::vector<double> x_vals;
        std::vector<double> y_vals;
        x_vals.reserve(cam_rot_norms.size());
        y_vals.reserve(cam_rot_norms.size());

        for (const auto& c : cam_rot_norms) {
            double t_lidar = c.first + dt;
            if (t_lidar >= imu_gyro_norms.front().first && t_lidar <= imu_gyro_norms.back().first) {
                x_vals.push_back(interpImu(t_lidar));
                y_vals.push_back(c.second);
            }
        }
        if (x_vals.size() < 5) continue;

        double mx = 0.0, my = 0.0;
        for (size_t i = 0; i < x_vals.size(); ++i) {
            mx += x_vals[i];
            my += y_vals[i];
        }
        mx /= static_cast<double>(x_vals.size());
        my /= static_cast<double>(y_vals.size());

        double num = 0.0, dx2 = 0.0, dy2 = 0.0;
        for (size_t i = 0; i < x_vals.size(); ++i) {
            double vx = x_vals[i] - mx;
            double vy = y_vals[i] - my;
            num += vx * vy;
            dx2 += vx * vx;
            dy2 += vy * vy;
        }
        double denom = std::sqrt(dx2 * dy2);
        if (denom > 1e-9) {
            double ncc = num / denom;
            if (ncc > best_ncc) {
                best_ncc = ncc;
                best_offset = dt;
            }
        }
    }

    if (out_peak_correlation) {
        *out_peak_correlation = std::max(0.0, best_ncc);
    }
    return best_offset;
}

SyncDiagnostics TimeSynchronizer::getDiagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return diag_;
}

void TimeSynchronizer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    imu_buffer_.clear();
    lidar_buffer_.clear();
    rgb_buffer_.clear();
    offset_window_.clear();
    last_imu_timestamp_ = -1.0;
    last_lidar_timestamp_ = -1.0;
    diag_ = SyncDiagnostics{};
}

}  // namespace sync
}  // namespace avscan
