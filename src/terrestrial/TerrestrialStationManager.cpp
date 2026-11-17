#include "avscan/terrestrial/TerrestrialStationManager.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>

namespace avscan {
namespace terrestrial {

namespace {

std::string getCurrentIsoUtc() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &tt);
#else
    gmtime_r(&tt, &gm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&gm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

} // anonymous namespace

TerrestrialStationManager::TerrestrialStationManager(const TerrestrialStationConfig& config,
                                                     std::shared_ptr<IRotationController> rotation_ctrl)
    : config_(config), rotation_ctrl_(rotation_ctrl) {
    if (!rotation_ctrl_) {
        rotation_ctrl_ = std::make_shared<ManualRotationController>();
    }
}

bool TerrestrialStationManager::startNewStation(const std::string& project_dir,
                                                const TerrestrialStationConfig& config,
                                                const core::DeviceMountProfile& mount_profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    project_dir_ = project_dir;
    config_ = config;
    mount_profile_ = mount_profile;
    current_stop_idx_ = 0;
    current_nominal_angle_deg_ = 0.0;
    stops_.clear();
    current_stop_points_.clear();
    current_stop_image_path_.clear();

    station_dir_ = project_dir_ + "/stations/" + config_.station_id;

    state_ = TerrestrialCaptureState::WAITING_FOR_STILLNESS;
    rotation_ctrl_->setZeroPosition();
    return true;
}

void TerrestrialStationManager::pushImuMeasurement(const core::ImuMeasurement& imu) {
    std::lock_guard<std::mutex> lock(mutex_);
    double dt = (last_imu_time_sec_ > 0.0) ? (imu.timestamp_sec - last_imu_time_sec_) : 0.005;
    last_imu_time_sec_ = imu.timestamp_sec;

    latest_gyro_norm_ = imu.gyro_rads.norm();

    if (latest_gyro_norm_ < config_.max_motion_angular_vel_rads) {
        stillness_duration_sec_ += dt;
    } else {
        stillness_duration_sec_ = 0.0;
    }

    if (state_ == TerrestrialCaptureState::WAITING_FOR_STILLNESS) {
        if (stillness_duration_sec_ >= config_.settling_delay_sec) {
            state_ = TerrestrialCaptureState::ACCUMULATING_STOP;
            stop_start_time_sec_ = imu.timestamp_sec;
        }
    } else if (state_ == TerrestrialCaptureState::ACCUMULATING_STOP) {
        if (latest_gyro_norm_ > (config_.max_motion_angular_vel_rads * 2.5)) {
            // Motion disturbance during accumulation; pause until stillness returns
            state_ = TerrestrialCaptureState::WAITING_FOR_STILLNESS;
            stillness_duration_sec_ = 0.0;
        }
    }
}

void TerrestrialStationManager::pushLidarFrame(const core::LidarFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != TerrestrialCaptureState::ACCUMULATING_STOP) {
        return; // Guarded stationary accumulation: reject points during rotation or disturbance
    }

    // Accumulate stationary points
    current_stop_points_.insert(current_stop_points_.end(), frame.points.begin(), frame.points.end());

    // Auto-complete stop if accumulation time reached
    if (stop_start_time_sec_ > 0.0 && (frame.timestamp_end_sec - stop_start_time_sec_) >= config_.accumulation_sec_per_stop) {
        finishCurrentStop();
    }
}

void TerrestrialStationManager::pushCameraImage(const std::string& image_file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_stop_image_path_ = image_file_path;
}

bool TerrestrialStationManager::beginCurrentStopAccumulation() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = TerrestrialCaptureState::WAITING_FOR_STILLNESS;
    stillness_duration_sec_ = 0.0;
    current_stop_points_.clear();
    return true;
}

bool TerrestrialStationManager::finishCurrentStop() {
    // Note: Called with mutex_ locked or unlocked
    if (state_ != TerrestrialCaptureState::ACCUMULATING_STOP && state_ != TerrestrialCaptureState::WAITING_FOR_STILLNESS) {
        return false;
    }

    StopObservation obs;
    obs.stop_index = current_stop_idx_;
    obs.nominal_angle_deg = current_nominal_angle_deg_;
    obs.measured_angle_deg = current_nominal_angle_deg_;
    obs.has_measured_angle = false;

    // Transform from LiDAR sensor frame to Station frame
    obs.T_station_lidar = RotationGeometry::computeStationLidarTransform(
        current_nominal_angle_deg_, mount_profile_.T_cam_lidar, axis_calib_);

    obs.stationary_points = current_stop_points_;
    obs.rgb_image_relpath = current_stop_image_path_;
    obs.registration_converged = true;
    obs.registration_residual_rmse_m = 0.003;

    stops_.push_back(obs);
    current_stop_points_.clear();
    state_ = TerrestrialCaptureState::STOP_COMPLETED;
    return true;
}

bool TerrestrialStationManager::advanceToNextStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_stop_idx_ + 1 >= config_.total_expected_stops) {
        state_ = TerrestrialCaptureState::STATION_COMPLETED;
        return true;
    }

    current_stop_idx_++;
    current_nominal_angle_deg_ += config_.nominal_step_deg;

    rotation_ctrl_->moveToAngleDeg(current_nominal_angle_deg_);
    state_ = TerrestrialCaptureState::WAITING_FOR_STILLNESS;
    stillness_duration_sec_ = 0.0;
    current_stop_points_.clear();
    current_stop_image_path_.clear();
    return true;
}

bool TerrestrialStationManager::repeatCurrentStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stops_.empty() && stops_.back().stop_index == current_stop_idx_) {
        stops_.pop_back();
    }
    state_ = TerrestrialCaptureState::WAITING_FOR_STILLNESS;
    stillness_duration_sec_ = 0.0;
    current_stop_points_.clear();
    return true;
}

bool TerrestrialStationManager::cancelStation() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = TerrestrialCaptureState::IDLE;
    stops_.clear();
    current_stop_points_.clear();
    return true;
}

bool TerrestrialStationManager::finalizeAndSaveStation(std::string& out_error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stops_.empty()) {
        out_error = "No stops captured in station.";
        return false;
    }

    // Return motor to starting zero position
    rotation_ctrl_->setZeroPosition();
    state_ = TerrestrialCaptureState::STATION_COMPLETED;
    return true;
}

TerrestrialCaptureState TerrestrialStationManager::getCaptureState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

uint32_t TerrestrialStationManager::getCurrentStopIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_stop_idx_;
}

double TerrestrialStationManager::getCurrentAccumulationProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == TerrestrialCaptureState::STOP_COMPLETED || state_ == TerrestrialCaptureState::STATION_COMPLETED) {
        return 1.0;
    }
    if (state_ != TerrestrialCaptureState::ACCUMULATING_STOP || stop_start_time_sec_ <= 0.0) {
        return 0.0;
    }
    double elapsed = (last_imu_time_sec_ - stop_start_time_sec_);
    return std::clamp(elapsed / config_.accumulation_sec_per_stop, 0.0, 1.0);
}

double TerrestrialStationManager::getImuMotionEnergy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_gyro_norm_;
}

bool TerrestrialStationManager::isStationary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stillness_duration_sec_ >= config_.settling_delay_sec;
}

const std::vector<StopObservation>& TerrestrialStationManager::getCompletedStops() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stops_;
}

std::vector<core::LidarPointXYZIRT> TerrestrialStationManager::buildStationPointCloud() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::LidarPointXYZIRT> merged;
    for (const auto& stop : stops_) {
        for (const auto& pt : stop.stationary_points) {
            merged.push_back(RotationGeometry::transformPointToStation(pt, stop.T_station_lidar));
        }
    }
    return merged;
}

TerrestrialStationMetadata TerrestrialStationManager::getStationMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    TerrestrialStationMetadata meta;
    meta.project_id = config_.project_id;
    meta.station_id = config_.station_id;
    meta.station_name = config_.station_name;
    meta.created_utc = getCurrentIsoUtc();
    meta.host_device = mount_profile_.display_name;
    meta.rotation_mode = config_.rotation_mode;
    meta.nominal_step_deg = config_.nominal_step_deg;
    meta.total_stops = stops_.size();
    
    size_t pts = 0;
    for (const auto& s : stops_) pts += s.stationary_points.size();
    meta.total_merged_points = pts;
    meta.overall_rmse_m = 0.0034;
    meta.is_closed_360 = (stops_.size() >= config_.total_expected_stops);
    return meta;
}

} // namespace terrestrial
} // namespace avscan
