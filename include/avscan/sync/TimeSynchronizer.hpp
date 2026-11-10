#pragma once

#include "avscan/core/Types.hpp"
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace avscan {
namespace sync {

struct SyncBundle {
    core::LidarFrame lidar_frame{};
    std::vector<core::ImuMeasurement> imu_measurements{};
    std::optional<core::RgbFrame> rgb_frame{};
    double estimated_cam_to_lidar_offset_sec{0.0};
    bool has_valid_imu_coverage{false};
};

struct SyncDiagnostics {
    uint64_t lidar_frames_received{0};
    uint64_t imu_samples_received{0};
    uint64_t camera_frames_received{0};
    uint64_t bundles_emitted{0};
    uint64_t non_monotonic_imu_drops{0};
    uint64_t non_monotonic_lidar_drops{0};
    double estimated_clock_offset_sec{0.0};  // t_lidar - t_host
    double calibrated_cam_offset_sec{0.0};   // fine temporal offset from calibration
    double clock_jitter_ms{0.0};
    bool clock_locked{false};
};

class TimeSynchronizer {
public:
    TimeSynchronizer();

    void setCalibratedCameraTimeOffset(double offset_sec);
    double getCalibratedCameraTimeOffset() const;

    // Ingests an IMU sample with sensor timestamp and host reception timestamp
    bool pushImu(const core::ImuMeasurement& imu, double host_recv_time_sec);

    // Ingests a completed 360-deg LiDAR sweep with sensor timestamps and host reception timestamp
    bool pushLidarFrame(const core::LidarFrame& frame, double host_recv_time_sec);

    // Ingests an iOS camera RGB frame (timestamped on host monotonic clock)
    bool pushRgbFrame(const core::RgbFrame& rgb_frame);

    // Attempts to pop the next time-aligned SyncBundle ready for FAST-LIVO2 processing
    bool popSyncBundle(SyncBundle& out_bundle);

    // Refines camera-to-LiDAR temporal offset via normalized cross-correlation of angular velocity norms
    static double estimateTemporalOffsetByCrossCorrelation(
        const std::vector< std::pair<double, double> >& imu_gyro_norms,
        const std::vector< std::pair<double, double> >& cam_rot_norms,
        double search_window_sec = 0.20,
        double step_sec = 0.001,
        double* out_peak_correlation = nullptr);

    SyncDiagnostics getDiagnostics() const;
    void reset();

private:
    void updateClockOffsetEstimate(double sensor_time_sec, double host_time_sec);

    mutable std::mutex mutex_;
    std::deque<core::ImuMeasurement> imu_buffer_;
    std::deque<core::LidarFrame> lidar_buffer_;
    std::deque<core::RgbFrame> rgb_buffer_;

    std::deque<double> offset_window_;
    double last_imu_timestamp_{-1.0};
    double last_lidar_timestamp_{-1.0};

    SyncDiagnostics diag_{};
};

}  // namespace sync
}  // namespace avscan
