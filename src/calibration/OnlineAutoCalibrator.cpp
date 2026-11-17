#include "avscan/calibration/OnlineAutoCalibrator.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace avscan {
namespace calibration {

math::SE3d OnlineAutoCalibrator::arkitToOpenCvCameraPose(const math::SE3d& T_world_cam_arkit) {
    // ARKit camera local frame: +X right, +Y up, -Z forward.
    // OpenCV optical frame:     +X right, +Y down, +Z forward.
    math::Mat3d R_arkit_to_cv = math::Mat3d::Identity();
    R_arkit_to_cv(1, 1) = -1.0;
    R_arkit_to_cv(2, 2) = -1.0;
    return T_world_cam_arkit * math::SE3d(math::SO3d(R_arkit_to_cv), math::Vec3d(0.0, 0.0, 0.0));
}

core::LidarFrame OnlineAutoCalibrator::downsampleLidarFrame(const core::LidarFrame& frame, size_t max_points) {
    if (frame.points.size() <= max_points || max_points == 0) {
        return frame;
    }
    core::LidarFrame out = frame;
    out.points.clear();
    out.points.reserve(max_points);
    const size_t step = std::max<size_t>(1, frame.points.size() / max_points);
    for (size_t i = 0; i < frame.points.size() && out.points.size() < max_points; i += step) {
        out.points.push_back(frame.points[i]);
    }
    return out;
}

bool OnlineAutoCalibrator::evaluateFeatureRichness(const sync::SyncBundle& bundle,
                                                   double min_edge_ratio,
                                                   size_t min_lidar_points) {
    if (bundle.lidar_frame.points.size() < min_lidar_points || !bundle.rgb_frame.has_value()) {
        return false;
    }
    const auto& rgb = bundle.rgb_frame.value();
    const int w = rgb.intrinsics.width;
    const int h = rgb.intrinsics.height;
    if (w < 16 || h < 16 || rgb.gray_bytes.size() < static_cast<size_t>(w * h)) {
        return false;
    }

    size_t sampled = 0;
    size_t edge_hits = 0;
    for (int y = 2; y < h - 2; y += 2) {
        for (int x = 2; x < w - 2; x += 2) {
            const double gx = static_cast<double>(rgb.gray_bytes[static_cast<size_t>(y * w + (x + 1))]) -
                              static_cast<double>(rgb.gray_bytes[static_cast<size_t>(y * w + (x - 1))]);
            const double gy = static_cast<double>(rgb.gray_bytes[static_cast<size_t>((y + 1) * w + x)]) -
                              static_cast<double>(rgb.gray_bytes[static_cast<size_t>((y - 1) * w + x)]);
            if (gx * gx + gy * gy > 100.0) {
                edge_hits++;
            }
            sampled++;
        }
    }
    if (sampled == 0) {
        return false;
    }
    const double edge_ratio = static_cast<double>(edge_hits) / static_cast<double>(sampled);
    return edge_ratio >= min_edge_ratio;
}

void OnlineAutoCalibrator::reset(const AutoCalibConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    state_ = AutoCalibState::INSUFFICIENT_FEATURES;
    cam_poses_.clear();
    has_prev_key_pose_ = false;
    has_initial_lock_ = false;
    env_feature_rich_ = false;
    continuous_refine_in_flight_ = false;
    start_time_sec_ = -1.0;
    last_lidar_time_sec_ = -1.0;
    last_vl_capture_time_sec_ = -1.0;
    last_continuous_refine_time_sec_ = -1.0;
    prev_T_world_lidar_ = math::SE3d::Identity();
    prev_T_world_cam_ = math::SE3d::Identity();
    axis_scatter_ = math::Mat3d::Zero();
    second_axis_eig_ = 0.0;
    confidence_score_ = 0.0;
    initial_locked_profile_ = core::ExtrinsicCalibrationProfile{};
    motion_pairs_.clear();
    vl_pairs_.clear();
    continuous_vl_pairs_.clear();
    imu_gyro_norms_.clear();
    cam_rot_norms_.clear();
}

void OnlineAutoCalibrator::pushCameraPose(double host_timestamp_sec, const math::SE3d& T_world_cam) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_initial_lock_) {
        return;
    }
    math::SE3d pose = config_.convert_arkit_to_opencv
        ? arkitToOpenCvCameraPose(T_world_cam)
        : T_world_cam;

    if (!cam_poses_.empty() && host_timestamp_sec <= cam_poses_.back().timestamp_sec) {
        return;
    }
    cam_poses_.push_back({host_timestamp_sec, pose});
    while (cam_poses_.size() > 300) {
        cam_poses_.pop_front();
    }
}

bool OnlineAutoCalibrator::interpolateCameraPoseLocked(double lidar_time_sec,
                                                       double cam_to_lidar_offset_sec,
                                                       math::SE3d& out_T_world_cam) const {
    if (cam_poses_.size() < 2) {
        return false;
    }

    const double target_cam_time = lidar_time_sec - cam_to_lidar_offset_sec;
    if (target_cam_time < cam_poses_.front().timestamp_sec - 0.02 ||
        target_cam_time > cam_poses_.back().timestamp_sec + 0.02) {
        return false;
    }

    if (target_cam_time <= cam_poses_.front().timestamp_sec) {
        out_T_world_cam = cam_poses_.front().pose;
        return true;
    }
    if (target_cam_time >= cam_poses_.back().timestamp_sec) {
        out_T_world_cam = cam_poses_.back().pose;
        return true;
    }

    auto it = std::lower_bound(
        cam_poses_.begin(), cam_poses_.end(), target_cam_time,
        [](const StampedPose& sp, double t) { return sp.timestamp_sec < t; });
    if (it == cam_poses_.begin()) {
        out_T_world_cam = it->pose;
        return true;
    }
    auto prev = it - 1;
    const double dt = it->timestamp_sec - prev->timestamp_sec;
    const double alpha = (dt > 1e-9) ? std::clamp((target_cam_time - prev->timestamp_sec) / dt, 0.0, 1.0) : 0.0;

    math::Vec3d t_interp = prev->pose.t * (1.0 - alpha) + it->pose.t * alpha;
    math::Quatd q0 = prev->pose.so3.unit_quaternion();
    math::Quatd q1 = it->pose.so3.unit_quaternion();
    double dot = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
    if (dot < 0.0) {
        q1.x = -q1.x; q1.y = -q1.y; q1.z = -q1.z; q1.w = -q1.w;
    }
    math::Quatd q_interp(
        q0.x * (1.0 - alpha) + q1.x * alpha,
        q0.y * (1.0 - alpha) + q1.y * alpha,
        q0.z * (1.0 - alpha) + q1.z * alpha,
        q0.w * (1.0 - alpha) + q1.w * alpha);
    q_interp.normalize();

    out_T_world_cam = math::SE3d(q_interp, t_interp);
    return true;
}

bool OnlineAutoCalibrator::pushLidarBundleAndPose(const sync::SyncBundle& bundle,
                                                  const math::SE3d& T_world_lidar) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == AutoCalibState::IDLE || state_ == AutoCalibState::SOLVING) {
        return false;
    }

    const double lidar_t = bundle.lidar_frame.timestamp_end_sec;
    if (start_time_sec_ < 0.0) {
        start_time_sec_ = lidar_t;
    }
    last_lidar_time_sec_ = lidar_t;

    env_feature_rich_ = evaluateFeatureRichness(
        bundle, config_.min_edge_pixel_ratio, config_.min_lidar_points);

    // Post-initial-lock mode: monitor environment features & trigger continuous refinement
    // without ever stalling or disabling colorization.
    if (has_initial_lock_) {
        if (!env_feature_rich_) {
            state_ = AutoCalibState::LOCKED_YELLOW_FALLBACK;
            continuous_vl_pairs_.clear();
            return false;
        }

        if (continuous_refine_in_flight_) {
            return false;
        }

        if (bundle.rgb_frame.has_value() && !bundle.rgb_frame->gray_bytes.empty() &&
            (last_vl_capture_time_sec_ < 0.0 ||
             (lidar_t - last_vl_capture_time_sec_) >= config_.min_vl_interval_sec)) {
            DirectVLCalibPair vlp;
            vlp.lidar_frame = downsampleLidarFrame(bundle.lidar_frame, config_.max_points_per_vl_frame);
            vlp.rgb_frame = bundle.rgb_frame.value();
            continuous_vl_pairs_.push_back(std::move(vlp));
            if (continuous_vl_pairs_.size() > 3) {
                continuous_vl_pairs_.erase(continuous_vl_pairs_.begin());
            }
            last_vl_capture_time_sec_ = lidar_t;
        }

        const bool due = (last_continuous_refine_time_sec_ < 0.0) ||
                         ((lidar_t - last_continuous_refine_time_sec_) >= config_.continuous_refine_interval_sec);
        return due && (continuous_vl_pairs_.size() >= 3);
    }

    // Pre-initial-lock mode: check environment first, then collect multi-axis motion
    if (!env_feature_rich_) {
        state_ = AutoCalibState::INSUFFICIENT_FEATURES;
        return false;
    }

    state_ = AutoCalibState::COLLECTING_MOTION;

    for (const auto& imu : bundle.imu_measurements) {
        if (imu_gyro_norms_.empty() || imu.timestamp_sec > imu_gyro_norms_.back().first) {
            imu_gyro_norms_.emplace_back(imu.timestamp_sec, imu.gyro_rads.norm());
        }
    }
    if (imu_gyro_norms_.size() > 600) {
        imu_gyro_norms_.erase(imu_gyro_norms_.begin(), imu_gyro_norms_.begin() + 200);
    }

    math::SE3d T_world_cam;
    if (!interpolateCameraPoseLocked(lidar_t, bundle.estimated_cam_to_lidar_offset_sec, T_world_cam)) {
        return false;
    }

    if (!has_prev_key_pose_) {
        prev_T_world_lidar_ = T_world_lidar;
        prev_T_world_cam_ = T_world_cam;
        has_prev_key_pose_ = true;
        return false;
    }

    if (bundle.rgb_frame.has_value() && !bundle.rgb_frame->gray_bytes.empty() &&
        (last_vl_capture_time_sec_ < 0.0 || (lidar_t - last_vl_capture_time_sec_) >= config_.min_vl_interval_sec)) {
        DirectVLCalibPair vlp;
        vlp.lidar_frame = downsampleLidarFrame(bundle.lidar_frame, config_.max_points_per_vl_frame);
        vlp.rgb_frame = bundle.rgb_frame.value();
        vl_pairs_.push_back(std::move(vlp));
        if (vl_pairs_.size() > config_.max_vl_pairs) {
            vl_pairs_.erase(vl_pairs_.begin());
        }
        last_vl_capture_time_sec_ = lidar_t;
    }

    math::SE3d A = prev_T_world_lidar_.inverse() * T_world_lidar;
    math::SE3d B = prev_T_world_cam_.inverse() * T_world_cam;

    const double rot_a = A.so3.log().norm();
    const double rot_b = B.so3.log().norm();
    const double trans_a = A.t.norm();

    if (rot_a >= config_.min_step_rot_rad && rot_b >= config_.min_step_rot_rad) {
        HandEyeMotionPair pair;
        pair.A_lidar_motion = A;
        pair.B_camera_motion = B;
        pair.timestamp_sec = lidar_t;
        motion_pairs_.push_back(pair);

        if (motion_pairs_.size() > config_.max_motion_pairs) {
            motion_pairs_.erase(motion_pairs_.begin());
        }

        math::Vec3d a_unit = A.so3.log().normalized();
        axis_scatter_ = axis_scatter_ + math::Mat3d::OuterProduct(a_unit, a_unit);
        math::SymEigen3d eig = math::SymEigen3d::compute(axis_scatter_);
        second_axis_eig_ = eig.eigenvalues[1];

        cam_rot_norms_.emplace_back(lidar_t, rot_b);

        prev_T_world_lidar_ = T_world_lidar;
        prev_T_world_cam_ = T_world_cam;
    } else if (trans_a >= config_.min_step_trans_m && rot_a < 0.01) {
        prev_T_world_lidar_ = T_world_lidar;
        prev_T_world_cam_ = T_world_cam;
    }

    return (motion_pairs_.size() >= config_.min_motion_pairs) &&
           (second_axis_eig_ >= config_.min_multi_axis_eig) &&
           (vl_pairs_.size() >= config_.min_vl_pairs);
}

bool OnlineAutoCalibrator::beginSolve(std::vector<HandEyeMotionPair>& out_motions,
                                      std::vector<DirectVLCalibPair>& out_vl_pairs,
                                      std::vector<std::pair<double, double>>& out_imu_norms,
                                      std::vector<std::pair<double, double>>& out_cam_norms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_initial_lock_ || state_ != AutoCalibState::COLLECTING_MOTION) {
        return false;
    }
    if (motion_pairs_.size() < 3 || second_axis_eig_ < config_.min_multi_axis_eig) {
        return false;
    }

    state_ = AutoCalibState::SOLVING;
    out_motions = motion_pairs_;
    out_vl_pairs = vl_pairs_;
    out_imu_norms = imu_gyro_norms_;
    out_cam_norms = cam_rot_norms_;
    return true;
}

bool OnlineAutoCalibrator::completeSolve(const std::vector<HandEyeMotionPair>& motions,
                                         const std::vector<DirectVLCalibPair>& vl_pairs,
                                         const std::vector<std::pair<double, double>>& imu_norms,
                                         const std::vector<std::pair<double, double>>& cam_norms,
                                         const std::string& device_model,
                                         const std::string& mount_id,
                                         core::ExtrinsicCalibrationProfile& out_profile) {
    bool ok = TargetlessCalibrator::runFullTargetlessCalibration(
        motions, vl_pairs, imu_norms, cam_norms, device_model, mount_id, out_profile);

    std::lock_guard<std::mutex> lock(mutex_);
    if (ok && out_profile.verified) {
        has_initial_lock_ = true;
        initial_locked_profile_ = out_profile;
        last_continuous_refine_time_sec_ = last_lidar_time_sec_;
        state_ = AutoCalibState::LOCKED_GREEN;
        confidence_score_ = out_profile.confidence_score;
        return true;
    }

    // Never stall permanently: trim oldest pairs and continue collecting
    if (motion_pairs_.size() > 6) {
        motion_pairs_.erase(motion_pairs_.begin(), motion_pairs_.begin() + 3);
    }
    state_ = env_feature_rich_ ? AutoCalibState::COLLECTING_MOTION : AutoCalibState::INSUFFICIENT_FEATURES;
    confidence_score_ = out_profile.confidence_score;
    return false;
}

bool OnlineAutoCalibrator::beginContinuousRefine(std::vector<DirectVLCalibPair>& out_vl_pairs,
                                                 core::ExtrinsicCalibrationProfile& out_initial_profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_initial_lock_ || continuous_refine_in_flight_ || continuous_vl_pairs_.size() < 3) {
        return false;
    }
    continuous_refine_in_flight_ = true;
    out_vl_pairs = continuous_vl_pairs_;
    out_initial_profile = initial_locked_profile_;
    continuous_vl_pairs_.clear();
    return true;
}

bool OnlineAutoCalibrator::completeContinuousRefine(const std::vector<DirectVLCalibPair>& vl_pairs,
                                                    core::ExtrinsicCalibrationProfile& out_profile_to_apply) {
    core::ExtrinsicCalibrationProfile init_prof;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        init_prof = initial_locked_profile_;
    }

    math::SE3d refined_T = init_prof.T_cam_lidar;
    double nid = init_prof.nid_metric;
    double conf = 0.0;
    bool ok = TargetlessCalibrator::optimizeDirectVLCalib(
        vl_pairs, init_prof.T_cam_lidar, refined_T, nid, conf);

    std::lock_guard<std::mutex> lock(mutex_);
    continuous_refine_in_flight_ = false;
    last_continuous_refine_time_sec_ = last_lidar_time_sec_;

    if (ok && conf >= 0.45 && env_feature_rich_) {
        out_profile_to_apply = init_prof;
        out_profile_to_apply.T_cam_lidar = refined_T;
        out_profile_to_apply.nid_metric = nid;
        out_profile_to_apply.confidence_score = std::max(init_prof.confidence_score, conf);
        out_profile_to_apply.verified = true;
        confidence_score_ = out_profile_to_apply.confidence_score;
        state_ = AutoCalibState::LOCKED_GREEN;
        return true;
    }

    // Revert to initial lock when features are insufficient for continuous lock
    out_profile_to_apply = initial_locked_profile_;
    out_profile_to_apply.verified = true;
    confidence_score_ = initial_locked_profile_.confidence_score;
    state_ = AutoCalibState::LOCKED_YELLOW_FALLBACK;
    return false;
}

AutoCalibStatus OnlineAutoCalibrator::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AutoCalibStatus st;
    st.state = state_;
    st.has_initial_lock = has_initial_lock_;
    st.environment_feature_rich = env_feature_rich_;
    st.motion_pairs_collected = motion_pairs_.size();
    st.motion_pairs_required = config_.min_motion_pairs;
    st.vl_pairs_collected = vl_pairs_.size();
    st.second_axis_excitation = second_axis_eig_;
    st.elapsed_sec = (start_time_sec_ >= 0.0 && last_lidar_time_sec_ >= start_time_sec_)
        ? (last_lidar_time_sec_ - start_time_sec_)
        : 0.0;
    st.confidence_score = confidence_score_;

    std::ostringstream oss;
    switch (state_) {
        case AutoCalibState::IDLE:
            st.popup_color = AutoCalibPopupColor::NONE;
            st.popup_title = "IDLE";
            oss << "Ready to scan";
            break;
        case AutoCalibState::INSUFFICIENT_FEATURES:
            st.popup_color = AutoCalibPopupColor::RED;
            st.popup_title = "LOCKING FAILURE";
            oss << "Insufficient features for initial lock — point at textured edges";
            break;
        case AutoCalibState::COLLECTING_MOTION:
            st.popup_color = AutoCalibPopupColor::WHITE;
            st.popup_title = "LOCKING";
            oss << "Sufficient features — tilt pitch & pan yaw ("
                << st.motion_pairs_collected << "/" << st.motion_pairs_required << ")";
            break;
        case AutoCalibState::SOLVING:
            st.popup_color = AutoCalibPopupColor::WHITE;
            st.popup_title = "LOCKING";
            oss << "Solving initial extrinsics...";
            break;
        case AutoCalibState::LOCKED_GREEN:
            st.popup_color = AutoCalibPopupColor::GREEN;
            st.popup_title = "LOCKED";
            oss.precision(2);
            oss << std::fixed << "Confirmed (conf " << st.confidence_score << ") — live color active";
            break;
        case AutoCalibState::LOCKED_YELLOW_FALLBACK:
            st.popup_color = AutoCalibPopupColor::YELLOW;
            st.popup_title = "LOCKED";
            oss << "Low features — using initial lock for colorization";
            break;
    }
    st.status_message = oss.str();
    return st;
}

bool OnlineAutoCalibrator::isLocked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_initial_lock_;
}

}  // namespace calibration
}  // namespace avscan
