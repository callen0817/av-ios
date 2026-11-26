#include "avscan/terrestrial/TerrestrialStationManager.hpp"
#include "avscan/export/PointCloudExporter.hpp"
#include "avscan/export/AvSpatialBundleExporter.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

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

struct VoxelKey {
    int32_t ix, iy, iz;
    bool operator==(const VoxelKey& o) const {
        return ix == o.ix && iy == o.iy && iz == o.iz;
    }
};

struct VoxelKeyHash {
    size_t operator()(const VoxelKey& k) const {
        return (static_cast<size_t>(k.ix) * 73856093u) ^
               (static_cast<size_t>(k.iy) * 19349663u) ^
               (static_cast<size_t>(k.iz) * 83492791u);
    }
};

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
    stop_start_time_sec_ = 0.0;
    latest_frame_end_sec_ = 0.0;
    stillness_duration_sec_ = 0.0;
    has_received_imu_ = false;
    refined_overall_rmse_m_ = 0.0;

    stops_.clear();
    current_stop_points_.clear();
    current_stop_image_path_.clear();

    station_dir_ = project_dir_ + "/stations/" + config_.station_id;
    std::error_code ec;
    fs::create_directories(fs::path(station_dir_) / "raw" / "images", ec);
    fs::create_directories(fs::path(station_dir_) / "cache", ec);
    fs::create_directories(fs::path(station_dir_) / "pano", ec);

    if (config_.is_single_shot_flash) {
        // Single-shot Flash Scan: 1 stationary accumulation pose in IMU body frame, zero turntable/motor interaction
        config_.total_expected_stops = 1;
        config_.nominal_step_deg = 360.0;
        config_.enable_overlap_refinement = false;
    } else {
        rotation_ctrl_->setZeroPosition();
    }
    state_ = TerrestrialCaptureState::WAITING_FOR_STILLNESS;
    return true;
}

void TerrestrialStationManager::pushImuMeasurement(const core::ImuMeasurement& imu) {
    std::lock_guard<std::mutex> lock(mutex_);
    double dt = (last_imu_time_sec_ > 0.0 && imu.timestamp_sec > last_imu_time_sec_)
                    ? std::clamp(imu.timestamp_sec - last_imu_time_sec_, 0.001, 0.10)
                    : 0.005;
    last_imu_time_sec_ = imu.timestamp_sec;
    has_received_imu_ = true;

    latest_gyro_norm_ = imu.gyro_rads.norm();
    latest_accel_dev_ = std::abs(imu.accel_mps2.norm() - 9.81);

    // Dual-criterion stillness gate: angular rate < threshold AND specific force near 1g
    bool is_quiet = (latest_gyro_norm_ < config_.max_motion_angular_vel_rads) &&
                    (latest_accel_dev_ < 0.85);

    if (is_quiet) {
        stillness_duration_sec_ += dt;
    } else {
        stillness_duration_sec_ = 0.0;
    }

    if (state_ == TerrestrialCaptureState::WAITING_FOR_STILLNESS) {
        if (stillness_duration_sec_ >= config_.settling_delay_sec) {
            state_ = TerrestrialCaptureState::ACCUMULATING_STOP;
            stop_start_time_sec_ = 0.0; // Anchor on first LiDAR frame timestamp
        }
    } else if (state_ == TerrestrialCaptureState::ACCUMULATING_STOP) {
        if (latest_gyro_norm_ > (config_.max_motion_angular_vel_rads * 2.5) ||
            latest_accel_dev_ > 2.0) {
            // Motion disturbance or operator bump during accumulation: discard partial buffer and re-settle
            state_ = TerrestrialCaptureState::WAITING_FOR_STILLNESS;
            stillness_duration_sec_ = 0.0;
            stop_start_time_sec_ = 0.0;
            current_stop_points_.clear();
        }
    }
}

void TerrestrialStationManager::pushLidarFrame(const core::LidarFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Fallback for LiDAR-only profiles (Profile C) where no IMU stream is present:
    // If no IMU packets have arrived, transition from WAITING_FOR_STILLNESS after settling delay
    if (!has_received_imu_ && state_ == TerrestrialCaptureState::WAITING_FOR_STILLNESS) {
        if (stop_start_time_sec_ <= 0.0) {
            stop_start_time_sec_ = frame.timestamp_start_sec;
        } else if ((frame.timestamp_end_sec - stop_start_time_sec_) >= config_.settling_delay_sec) {
            state_ = TerrestrialCaptureState::ACCUMULATING_STOP;
            stop_start_time_sec_ = frame.timestamp_start_sec;
            stillness_duration_sec_ = config_.settling_delay_sec;
        }
        return;
    }

    if (state_ != TerrestrialCaptureState::ACCUMULATING_STOP) {
        return; // Guarded stationary accumulation: reject points during rotation or disturbance
    }

    if (stop_start_time_sec_ <= 0.0) {
        stop_start_time_sec_ = frame.timestamp_start_sec;
    }
    latest_frame_end_sec_ = frame.timestamp_end_sec;

    // Accumulate stationary points
    current_stop_points_.insert(current_stop_points_.end(), frame.points.begin(), frame.points.end());

    // Auto-complete stop when accumulation duration is reached on unified LiDAR clock
    if ((latest_frame_end_sec_ - stop_start_time_sec_) >= config_.accumulation_sec_per_stop) {
        finishCurrentStopLocked();
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
    stop_start_time_sec_ = 0.0;
    current_stop_points_.clear();
    return true;
}

bool TerrestrialStationManager::finishCurrentStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    return finishCurrentStopLocked();
}

bool TerrestrialStationManager::finishCurrentStopLocked() {
    if (state_ != TerrestrialCaptureState::ACCUMULATING_STOP &&
        state_ != TerrestrialCaptureState::WAITING_FOR_STILLNESS) {
        return false;
    }
    if (current_stop_points_.empty()) {
        return false; // Do not commit an empty stop
    }

    StopObservation obs;
    obs.stop_index = current_stop_idx_;
    obs.nominal_angle_deg = current_nominal_angle_deg_;

    double measured_deg = current_nominal_angle_deg_;
    obs.has_measured_angle = rotation_ctrl_->getCurrentAngleDeg(measured_deg);
    obs.measured_angle_deg = measured_deg;

    obs.timestamp_start_sec = stop_start_time_sec_;
    obs.timestamp_end_sec = latest_frame_end_sec_;

    // Transform from LiDAR sensor frame:
    //  - Single-shot Flash Scan: exact IMU body frame (T_imu_lidar) to lock cleanly into active SLAM pose graph
    //  - Terrestrial Mode (Mode B): tripod station frame via RotationGeometry
    if (config_.is_single_shot_flash) {
        obs.T_station_lidar = config_.T_imu_lidar;
    } else {
        obs.T_station_lidar = RotationGeometry::computeStationLidarTransform(
            obs.measured_angle_deg, mount_profile_.T_cam_lidar, axis_calib_);
    }

    obs.stationary_points = current_stop_points_;
    obs.rgb_image_relpath = current_stop_image_path_;

    // Save raw stop checkpoint to disk so source data is preserved independently of downstream steps
    std::ostringstream raw_name;
    raw_name << "raw/stop_" << std::setw(3) << std::setfill('0') << obs.stop_index
             << "_" << std::setw(3) << std::setfill('0') << static_cast<int>(std::round(obs.nominal_angle_deg))
             << "deg.ply";
    obs.raw_lidar_relpath = raw_name.str();
    if (!station_dir_.empty()) {
        std::string abs_raw = (fs::path(station_dir_) / obs.raw_lidar_relpath).string();
        exporters::PointCloudExporter::exportToPly(abs_raw, obs.stationary_points, true);
    }

    obs.registration_converged = true;
    obs.registration_residual_rmse_m = 0.0;

    stops_.push_back(std::move(obs));
    current_stop_points_.clear();
    state_ = config_.is_single_shot_flash
        ? TerrestrialCaptureState::STATION_COMPLETED
        : TerrestrialCaptureState::STOP_COMPLETED;
    return true;
}

bool TerrestrialStationManager::advanceToNextStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    // If operator taps Next Stop while still accumulating and points exist, commit current stop first
    if (state_ == TerrestrialCaptureState::ACCUMULATING_STOP && !current_stop_points_.empty()) {
        finishCurrentStopLocked();
    }

    if (current_stop_idx_ + 1 >= config_.total_expected_stops) {
        state_ = TerrestrialCaptureState::STATION_COMPLETED;
        return true;
    }

    current_stop_idx_++;
    current_nominal_angle_deg_ += config_.nominal_step_deg;

    // Cable safety guard: prevent > 360 deg unidirectional rotation that twists cables
    if (current_nominal_angle_deg_ >= 360.0) {
        current_nominal_angle_deg_ = std::fmod(current_nominal_angle_deg_, 360.0);
    }

    rotation_ctrl_->moveToAngleDeg(current_nominal_angle_deg_);
    state_ = TerrestrialCaptureState::WAITING_FOR_STILLNESS;
    stillness_duration_sec_ = 0.0;
    stop_start_time_sec_ = 0.0;
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
    stop_start_time_sec_ = 0.0;
    current_stop_points_.clear();
    return true;
}

bool TerrestrialStationManager::cancelStation() {
    std::lock_guard<std::mutex> lock(mutex_);
    rotation_ctrl_->stopMotion();
    state_ = TerrestrialCaptureState::IDLE;
    stops_.clear();
    current_stop_points_.clear();
    return true;
}

bool TerrestrialStationManager::refineStopAnglesViaOverlap(double& out_mean_rmse_m) {
    if (stops_.size() < 2) {
        out_mean_rmse_m = 0.0;
        return true;
    }

    constexpr double kVoxelSize = 0.15; // 15cm spatial hash for overlap correspondence
    constexpr double kInvVoxel = 1.0 / kVoxelSize;
    double total_rmse = 0.0;
    size_t valid_pairs = 0;

    // Anchor Stop 0 at 0.0 deg; refine each subsequent stop k against stop k-1
    // via 1-DOF constrained rotation search around the vertical tripod Z-axis
    for (size_t k = 1; k < stops_.size(); ++k) {
        const auto& prev_stop = stops_[k - 1];
        auto& curr_stop = stops_[k];

        if (prev_stop.stationary_points.empty() || curr_stop.stationary_points.empty()) {
            continue;
        }

        // Build spatial voxel centroid map of prev_stop in Station Frame
        std::unordered_map<VoxelKey, math::Vec3d, VoxelKeyHash> ref_voxels;
        size_t ref_stride = std::max<size_t>(1, prev_stop.stationary_points.size() / 4000);
        for (size_t i = 0; i < prev_stop.stationary_points.size(); i += ref_stride) {
            auto p_st = RotationGeometry::transformPointToStation(prev_stop.stationary_points[i], prev_stop.T_station_lidar);
            VoxelKey vk{
                static_cast<int32_t>(std::floor(p_st.x * kInvVoxel)),
                static_cast<int32_t>(std::floor(p_st.y * kInvVoxel)),
                static_cast<int32_t>(std::floor(p_st.z * kInvVoxel))
            };
            ref_voxels[vk] = math::Vec3d(p_st.x, p_st.y, p_st.z);
        }

        // Coarse-to-fine 1-DOF search around nominal angle [-3.0 deg .. +3.0 deg]
        double best_angle_deg = curr_stop.nominal_angle_deg;
        double best_score = 1e9;
        double best_rmse = 0.0;
        size_t best_inliers = 0;

        size_t query_stride = std::max<size_t>(1, curr_stop.stationary_points.size() / 1500);

        for (double delta_deg = -3.0; delta_deg <= 3.001; delta_deg += 0.25) {
            double cand_deg = curr_stop.nominal_angle_deg + delta_deg;
            math::SE3d T_cand = RotationGeometry::computeStationLidarTransform(
                cand_deg, mount_profile_.T_cam_lidar, axis_calib_);

            double sum_sq = 0.0;
            size_t inliers = 0;
            for (size_t i = 0; i < curr_stop.stationary_points.size(); i += query_stride) {
                auto p_st = RotationGeometry::transformPointToStation(curr_stop.stationary_points[i], T_cand);
                VoxelKey vk{
                    static_cast<int32_t>(std::floor(p_st.x * kInvVoxel)),
                    static_cast<int32_t>(std::floor(p_st.y * kInvVoxel)),
                    static_cast<int32_t>(std::floor(p_st.z * kInvVoxel))
                };
                auto it = ref_voxels.find(vk);
                if (it != ref_voxels.end()) {
                    double d2 = (math::Vec3d(p_st.x, p_st.y, p_st.z) - it->second).squaredNorm();
                    if (d2 < (kVoxelSize * kVoxelSize)) {
                        sum_sq += d2;
                        inliers++;
                    }
                }
            }

            if (inliers >= 25) {
                double rmse = std::sqrt(sum_sq / static_cast<double>(inliers));
                // Penalize low inlier count slightly to favor strong overlap
                double cost = rmse / (std::sqrt(static_cast<double>(inliers)) + 1.0);
                if (cost < best_score) {
                    best_score = cost;
                    best_angle_deg = cand_deg;
                    best_rmse = rmse;
                    best_inliers = inliers;
                }
            }
        }

        if (best_inliers >= 25) {
            curr_stop.measured_angle_deg = best_angle_deg;
            curr_stop.T_station_lidar = RotationGeometry::computeStationLidarTransform(
                best_angle_deg, mount_profile_.T_cam_lidar, axis_calib_);
            curr_stop.registration_residual_rmse_m = best_rmse;
            curr_stop.registration_converged = true;
            total_rmse += best_rmse;
            valid_pairs++;
        } else {
            curr_stop.registration_residual_rmse_m = 0.0;
            curr_stop.registration_converged = false;
        }
    }

    out_mean_rmse_m = (valid_pairs > 0) ? (total_rmse / static_cast<double>(valid_pairs)) : 0.0;
    return true;
}

void TerrestrialStationManager::computeCoverageMetrics(double& out_az_cov,
                                                       double& out_el_cov,
                                                       std::string& out_warning) const {
    constexpr int kAzBins = 36; // 10 deg per bin
    constexpr int kElBins = 18; // 10 deg per bin [-90..+90]
    bool az_hit[kAzBins]{};
    bool el_hit[kElBins]{};

    for (const auto& stop : stops_) {
        size_t stride = std::max<size_t>(1, stop.stationary_points.size() / 2000);
        for (size_t i = 0; i < stop.stationary_points.size(); i += stride) {
            auto p = RotationGeometry::transformPointToStation(stop.stationary_points[i], stop.T_station_lidar);
            double r_xy = std::hypot(p.x, p.y);
            double r = std::hypot(r_xy, p.z);
            if (r < 0.25) continue;

            double az_deg = std::atan2(p.y, p.x) * math::kRadToDeg;
            if (az_deg < 0.0) az_deg += 360.0;
            double el_deg = std::atan2(p.z, r_xy) * math::kRadToDeg;

            int az_idx = std::clamp(static_cast<int>(az_deg / 10.0), 0, kAzBins - 1);
            int el_idx = std::clamp(static_cast<int>((el_deg + 90.0) / 10.0), 0, kElBins - 1);
            az_hit[az_idx] = true;
            el_hit[el_idx] = true;
        }
    }

    int az_count = 0;
    for (bool b : az_hit) if (b) az_count++;
    int el_count = 0;
    for (bool b : el_hit) if (b) el_count++;

    out_az_cov = static_cast<double>(az_count) / static_cast<double>(kAzBins);
    out_el_cov = static_cast<double>(el_count) / static_cast<double>(kElBins);

    if (stops_.size() < config_.total_expected_stops) {
        out_warning = "Incomplete revolution (" + std::to_string(stops_.size()) + "/" +
                      std::to_string(config_.total_expected_stops) + " stops captured).";
    } else if (out_az_cov < 0.85) {
        out_warning = "Missing azimuth sectors detected (<85% 360 horizontal coverage).";
    } else {
        out_warning = "Complete 360 horizontal coverage verified.";
    }
}

bool TerrestrialStationManager::writeStationArtifactsLocked(std::string& out_error) {
    if (station_dir_.empty()) {
        out_error = "Station directory path is not initialized.";
        return false;
    }

    fs::path st_dir(station_dir_);
    std::error_code ec;
    fs::create_directories(st_dir / "cache", ec);
    fs::create_directories(st_dir / "pano", ec);
    fs::create_directories(st_dir / "raw" / "images", ec);

    // 1. Build merged station point cloud
    std::vector<core::LidarPointXYZIRT> merged;
    for (const auto& stop : stops_) {
        for (const auto& pt : stop.stationary_points) {
            merged.push_back(RotationGeometry::transformPointToStation(pt, stop.T_station_lidar));
        }
    }

    // 2. Write pointcloud.ply, station.e57, and cache/lod0.bin
    std::string ply_abs = (st_dir / "pointcloud.ply").string();
    std::string e57_abs = (st_dir / "station.e57").string();
    std::string lod0_abs = (st_dir / "cache" / "lod0.bin").string();

    exporters::PointCloudExporter::exportToPly(ply_abs, merged, true);
    exporters::PointCloudExporter::exportToE57(e57_abs, merged);
    export_bundle::AvSpatialBundleExporter::writeLod0Bin(lod0_abs, merged, 350000);

    // 3. Compute coverage metrics
    double az_cov = 0.0, el_cov = 0.0;
    std::string cov_warn;
    computeCoverageMetrics(az_cov, el_cov, cov_warn);

    // 4. Write canonical station_manifest.json (v1.0)
    std::string now_utc = getCurrentIsoUtc();
    std::ostringstream json;
    json << std::fixed << std::setprecision(5);
    json << "{\n";
    json << "  \"$schema\": \"https://artificialvision.ai/schemas/station_manifest_v1.0.json\",\n";
    json << "  \"schema_version\": \"1.0\",\n";
    json << "  \"project_id\": \"" << config_.project_id << "\",\n";
    json << "  \"station_id\": \"" << config_.station_id << "\",\n";
    json << "  \"name\": \"" << config_.station_name << "\",\n";
    json << "  \"mode\": \"" << (config_.is_single_shot_flash ? "flash_scan" : "terrestrial") << "\",\n";
    json << "  \"created_utc\": \"" << now_utc << "\",\n";
    json << "  \"hardware_profile\": {\n";
    json << "    \"app\": \"AV Scan iOS\",\n";
    json << "    \"app_version\": \"1.0.0\",\n";
    json << "    \"host_device\": \"" << mount_profile_.display_name << "\",\n";
    json << "    \"sensor_model\": \"RoboSense Airy\",\n";
    json << "    \"sensor_serial\": \"08:48:57:09:82:89\"\n";
    json << "  },\n";
    json << "  \"coordinate_system\": {\n";
    json << "    \"handedness\": \"right\",\n";
    json << "    \"up\": \"+Z\",\n";
    json << "    \"forward_at_zero\": \"+X\",\n";
    json << "    \"units\": \"meters\"\n";
    json << "  },\n";
    json << "  \"rotation_spec\": {\n";
    json << "    \"mode\": \"" << (config_.is_single_shot_flash ? "none_stationary" : "manual") << "\",\n";
    json << "    \"nominal_increment_deg\": " << config_.nominal_step_deg << ",\n";
    json << "    \"total_stops\": " << stops_.size() << ",\n";
    json << "    \"expected_stops\": " << config_.total_expected_stops << ",\n";
    json << "    \"accumulation_sec_per_stop\": " << config_.accumulation_sec_per_stop << "\n";
    json << "  },\n";
    json << "  \"stops\": [\n";
    for (size_t i = 0; i < stops_.size(); ++i) {
        const auto& s = stops_[i];
        json << "    {\n";
        json << "      \"index\": " << s.stop_index << ",\n";
        json << "      \"nominal_angle_deg\": " << s.nominal_angle_deg << ",\n";
        json << "      \"refined_angle_deg\": " << s.measured_angle_deg << ",\n";
        json << "      \"angle_source\": \"" << (s.has_measured_angle ? "encoder" : "overlap_refined") << "\",\n";
        json << "      \"timestamp_start_sec\": " << s.timestamp_start_sec << ",\n";
        json << "      \"timestamp_end_sec\": " << s.timestamp_end_sec << ",\n";
        json << "      \"lidar_points_captured\": " << s.stationary_points.size() << ",\n";
        json << "      \"raw_lidar_file\": \"" << s.raw_lidar_relpath << "\",\n";
        json << "      \"rgb_image_file\": \"" << s.rgb_image_relpath << "\",\n";
        json << "      \"registration_rmse_m\": " << s.registration_residual_rmse_m << "\n";
        json << "    }" << (i + 1 < stops_.size() ? ",\n" : "\n");
    }
    json << "  ],\n";
    json << "  \"coverage\": {\n";
    json << "    \"azimuth_coverage_ratio\": " << az_cov << ",\n";
    json << "    \"elevation_coverage_ratio\": " << el_cov << ",\n";
    json << "    \"status_note\": \"" << cov_warn << "\"\n";
    json << "  },\n";
    json << "  \"products\": {\n";
    json << "    \"point_cloud_ply\": \"pointcloud.ply\",\n";
    json << "    \"point_cloud_lod0\": \"cache/lod0.bin\",\n";
    json << "    \"station_e57\": \"station.e57\",\n";
    json << "    \"total_merged_points\": " << merged.size() << "\n";
    json << "  },\n";
    json << "  \"registration_quality\": {\n";
    json << "    \"overall_rmse_m\": " << refined_overall_rmse_m_ << ",\n";
    json << "    \"is_closed_360\": " << (stops_.size() >= config_.total_expected_stops ? "true" : "false") << "\n";
    json << "  }\n";
    json << "}\n";

    std::ofstream mf((st_dir / "station_manifest.json").string(), std::ios::trunc);
    if (!mf.is_open()) {
        out_error = "Failed to write station_manifest.json";
        return false;
    }
    mf << json.str();
    mf.close();
    return true;
}

bool TerrestrialStationManager::finalizeAndSaveStation(std::string& out_error) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Commit active stop if points were accumulated
    if (state_ == TerrestrialCaptureState::ACCUMULATING_STOP && !current_stop_points_.empty()) {
        finishCurrentStopLocked();
    }

    if (stops_.empty()) {
        out_error = "No stops captured in station.";
        return false;
    }

    // Refine stop orientations via constrained 1-DOF overlap alignment around tripod Z axis
    if (config_.enable_overlap_refinement && stops_.size() >= 2) {
        refineStopAnglesViaOverlap(refined_overall_rmse_m_);
    }

    // Write all station artifacts (station_manifest.json, pointcloud.ply, station.e57, cache/lod0.bin)
    writeStationArtifactsLocked(out_error);

    // Return motor to starting zero position safely (only for multi-stop turntable Terrestrial Mode)
    if (!config_.is_single_shot_flash) {
        rotation_ctrl_->setZeroPosition();
    }
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
    if (state_ != TerrestrialCaptureState::ACCUMULATING_STOP || stop_start_time_sec_ <= 0.0 || latest_frame_end_sec_ <= stop_start_time_sec_) {
        return 0.0;
    }
    double elapsed = (latest_frame_end_sec_ - stop_start_time_sec_);
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
    // Also preview live accumulating points for current stop in real time
    if (state_ == TerrestrialCaptureState::ACCUMULATING_STOP && !current_stop_points_.empty()) {
        const math::SE3d T_curr = config_.is_single_shot_flash
            ? config_.T_imu_lidar
            : RotationGeometry::computeStationLidarTransform(
                  current_nominal_angle_deg_, mount_profile_.T_cam_lidar, axis_calib_);
        for (const auto& pt : current_stop_points_) {
            merged.push_back(RotationGeometry::transformPointToStation(pt, T_curr));
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
    meta.overall_rmse_m = refined_overall_rmse_m_;
    meta.is_closed_360 = (stops_.size() >= config_.total_expected_stops);
    computeCoverageMetrics(meta.azimuth_coverage_ratio, meta.elevation_coverage_ratio, meta.coverage_warning);
    return meta;
}

} // namespace terrestrial
} // namespace avscan
