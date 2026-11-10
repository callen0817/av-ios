#include "avscan/slam/FastLivo2Engine.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace avscan {
namespace slam {

namespace {

// Solves a 6x6 symmetric positive-definite linear system A * x = b via Cholesky / LDLT with damping
bool solve6x6LinearSystem(double A[6][6], const double b[6], double x[6]) {
    double M[6][7]{};
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) M[i][j] = A[i][j];
        M[i][6] = b[i];
    }

    for (int k = 0; k < 6; ++k) {
        int max_row = k;
        double max_val = std::abs(M[k][k]);
        for (int i = k + 1; i < 6; ++i) {
            if (std::abs(M[i][k]) > max_val) {
                max_val = std::abs(M[i][k]);
                max_row = i;
            }
        }
        if (max_val < 1e-12) return false;
        if (max_row != k) {
            for (int j = k; j <= 6; ++j) std::swap(M[k][j], M[max_row][j]);
        }

        for (int i = k + 1; i < 6; ++i) {
            double factor = M[i][k] / M[k][k];
            for (int j = k; j <= 6; ++j) {
                M[i][j] -= factor * M[k][j];
            }
        }
    }

    for (int i = 5; i >= 0; --i) {
        double sum = M[i][6];
        for (int j = i + 1; j < 6; ++j) sum -= M[i][j] * x[j];
        x[i] = sum / M[i][i];
    }
    return true;
}

}  // namespace

FastLivo2Engine::FastLivo2Engine(const FastLivo2Config& config)
    : config_(config), local_map_(config.voxel_size_m, 30, 40000) {
    reset();
}

void FastLivo2Engine::setFactoryCalibration(const core::AiryFactoryCalibration& calib) {
    factory_calib_ = calib;
}

void FastLivo2Engine::setExtrinsicCalibration(const core::ExtrinsicCalibrationProfile& profile) {
    extrinsic_profile_ = profile;
}

void FastLivo2Engine::resetPose(const math::SE3d& new_T_world_body) {
    state_.T_world_body = new_T_world_body;
    state_.velocity_mps = {0.0, 0.0, 0.0};
    state_.status = core::TrackingStatus::TRACKING_LIO;
}

void FastLivo2Engine::reset() {
    state_ = core::SlamState{};
    state_.status = core::TrackingStatus::UNINITIALIZED;
    // Initialize 18-state error covariance diagonal: [rot(3), pos(3), vel(3), bg(3), ba(3), grav(3)]
    for (int i = 0; i < 3; ++i) {
        P_diag_[i + 0]  = 1e-4;  // Orientation variance (rad^2)
        P_diag_[i + 3]  = 1e-4;  // Position variance (m^2)
        P_diag_[i + 6]  = 1e-2;  // Velocity variance (m^2/s^2)
        P_diag_[i + 9]  = 1e-5;  // Gyro bias variance
        P_diag_[i + 12] = 1e-3;  // Accel bias variance
        P_diag_[i + 15] = 1e-4;  // Gravity vector variance
    }
    local_map_.clear();
    init_imu_accum_.clear();
    trajectory_.clear();
    last_keyframe_pose_ = math::SE3d::Identity();
    has_first_keyframe_ = false;
}

bool FastLivo2Engine::initializeImuGravity(const std::vector<core::ImuMeasurement>& imus) {
    for (const auto& m : imus) {
        init_imu_accum_.push_back(m);
    }
    if (init_imu_accum_.size() < 5) {
        state_.status = core::TrackingStatus::INITIALIZING_IMU;
        return false;
    }

    math::Vec3d mean_acc(0.0, 0.0, 0.0);
    math::Vec3d mean_gyro(0.0, 0.0, 0.0);
    for (const auto& m : init_imu_accum_) {
        mean_acc += m.accel_mps2;
        mean_gyro += m.gyro_rads;
    }
    double inv_n = 1.0 / static_cast<double>(init_imu_accum_.size());
    mean_acc *= inv_n;
    mean_gyro *= inv_n;

    double acc_norm = mean_acc.norm();
    if (acc_norm > 5.0 && acc_norm < 15.0) {
        // Align world Z axis with measured specific force so gravity is [0, 0, -9.81]
        math::Vec3d z_world = mean_acc.normalized();
        math::Vec3d x_seed = (std::abs(z_world.x) < 0.9) ? math::Vec3d(1.0, 0.0, 0.0) : math::Vec3d(0.0, 1.0, 0.0);
        math::Vec3d y_world = z_world.cross(x_seed).normalized();
        math::Vec3d x_world = y_world.cross(z_world).normalized();

        math::Mat3d R_wb;
        R_wb(0, 0) = x_world.x; R_wb(0, 1) = y_world.x; R_wb(0, 2) = z_world.x;
        R_wb(1, 0) = x_world.y; R_wb(1, 1) = y_world.y; R_wb(1, 2) = z_world.y;
        R_wb(2, 0) = x_world.z; R_wb(2, 1) = y_world.z; R_wb(2, 2) = z_world.z;

        state_.T_world_body = math::SE3d(R_wb.transpose(), math::Vec3d(0.0, 0.0, 0.0));
        state_.gravity_mps2 = math::Vec3d(0.0, 0.0, -9.81);
    }
    // Only seed gyro bias if sensor is nearly stationary during initialization
    if (mean_gyro.norm() < 0.15) {
        state_.bias_gyro_rads = mean_gyro;
    }

    state_.timestamp_sec = init_imu_accum_.back().timestamp_sec;
    state_.status = core::TrackingStatus::TRACKING_LIO;
    return true;
}

void FastLivo2Engine::propagateImuAndDeskew(const sync::SyncBundle& bundle,
                                            std::vector<ImuPropagatedPose>& out_imu_traj,
                                            core::LidarFrame& out_deskewed_body) {
    out_imu_traj.clear();
    out_deskewed_body = bundle.lidar_frame;

    const double t_scan_start = bundle.lidar_frame.timestamp_start_sec;
    const double t_scan_end = bundle.lidar_frame.timestamp_end_sec;

    ImuPropagatedPose pose0;
    pose0.timestamp_sec = (state_.timestamp_sec > 0.0) ? state_.timestamp_sec : t_scan_start;
    pose0.T_world_body = state_.T_world_body;
    pose0.velocity_world = state_.velocity_mps;
    out_imu_traj.push_back(pose0);

    double curr_t = pose0.timestamp_sec;
    for (const auto& imu : bundle.imu_measurements) {
        if (imu.timestamp_sec <= curr_t) continue;
        double dt = std::clamp(imu.timestamp_sec - curr_t, 1e-5, 0.10);
        curr_t = imu.timestamp_sec;

        math::Vec3d omega = imu.gyro_rads - state_.bias_gyro_rads;
        math::Vec3d acc_body = imu.accel_mps2 - state_.bias_accel_mps2;
        math::Vec3d acc_world = state_.T_world_body.so3 * acc_body + state_.gravity_mps2;

        // Propagate nominal state
        state_.T_world_body.t += state_.velocity_mps * dt + 0.5 * acc_world * (dt * dt);
        state_.velocity_mps += acc_world * dt;
        state_.T_world_body.so3 = state_.T_world_body.so3 * math::SO3d::exp(omega * dt);
        state_.timestamp_sec = curr_t;

        // Propagate 18-state covariance diagonal with continuous-time process noise Q
        for (int i = 0; i < 3; ++i) {
            P_diag_[i + 0]  += (config_.gyro_noise_sigma * config_.gyro_noise_sigma) * dt + P_diag_[i + 9] * dt * dt;
            P_diag_[i + 6]  += (config_.accel_noise_sigma * config_.accel_noise_sigma) * dt + P_diag_[i + 12] * dt * dt;
            P_diag_[i + 3]  += P_diag_[i + 6] * dt * dt;
            P_diag_[i + 9]  += (config_.bias_gyro_rw_sigma * config_.bias_gyro_rw_sigma) * dt;
            P_diag_[i + 12] += (config_.bias_accel_rw_sigma * config_.bias_accel_rw_sigma) * dt;
        }

        ImuPropagatedPose step;
        step.timestamp_sec = curr_t;
        step.T_world_body = state_.T_world_body;
        step.velocity_world = state_.velocity_mps;
        step.gyro_unbiased = omega;
        step.accel_world = acc_world;
        out_imu_traj.push_back(step);
    }

    // Ensure trajectory reaches exact scan end time
    if (t_scan_end > curr_t) {
        double dt = std::clamp(t_scan_end - curr_t, 0.0, 0.05);
        state_.T_world_body.t += state_.velocity_mps * dt;
        state_.timestamp_sec = t_scan_end;
        ImuPropagatedPose end_step = out_imu_traj.back();
        end_step.timestamp_sec = t_scan_end;
        end_step.T_world_body = state_.T_world_body;
        out_imu_traj.push_back(end_step);
    }

    // Backward motion deskewing: transform each point from its acquisition time t_i into sweep-end body frame
    const math::SE3d T_end_inv = state_.T_world_body.inverse();
    const math::SE3d& T_imu_lidar = factory_calib_.T_imu_lidar;
    const math::SE3d T_lidar_imu = T_imu_lidar.inverse();

    for (auto& pt : out_deskewed_body.points) {
        math::Vec3d p_lidar(pt.x, pt.y, pt.z);
        math::Vec3d p_imu = T_imu_lidar.transform(p_lidar);

        if (out_imu_traj.size() >= 2 && pt.timestamp_sec >= out_imu_traj.front().timestamp_sec) {
            auto it = std::lower_bound(
                out_imu_traj.begin(), out_imu_traj.end(), pt.timestamp_sec,
                [](const ImuPropagatedPose& s, double t) { return s.timestamp_sec < t; });
            math::SE3d T_wb_i = state_.T_world_body;
            if (it == out_imu_traj.begin()) {
                T_wb_i = it->T_world_body;
            } else if (it == out_imu_traj.end()) {
                T_wb_i = out_imu_traj.back().T_world_body;
            } else {
                const auto& prev = *(it - 1);
                double seg_dt = it->timestamp_sec - prev.timestamp_sec;
                double alpha = (seg_dt > 1e-7) ? ((pt.timestamp_sec - prev.timestamp_sec) / seg_dt) : 1.0;
                T_wb_i = math::SE3d::interpolate(prev.T_world_body, it->T_world_body, alpha);
            }
            math::Vec3d p_world_i = T_wb_i.transform(p_imu);
            math::Vec3d p_imu_end = T_end_inv.transform(p_world_i);
            math::Vec3d p_lidar_end = T_lidar_imu.transform(p_imu_end);
            pt.x = p_lidar_end.x;
            pt.y = p_lidar_end.y;
            pt.z = p_lidar_end.z;
        }
    }
}

void FastLivo2Engine::runGeometricIekfUpdate(const core::LidarFrame& deskewed_body,
                                             EstimatorMetrics& io_metrics) {
    if (local_map_.numValidPlanes() < 5) {
        return;
    }

    const int stride = std::max(1, config_.point_downsample_stride);
    const math::SE3d& T_il = factory_calib_.T_imu_lidar;

    for (int iter = 0; iter < config_.max_iekf_iterations; ++iter) {
        double HTH[6][6]{};
        double HTr[6]{};
        size_t valid_corrs = 0;
        double sum_sq_err = 0.0;

        for (size_t i = 0; i < deskewed_body.points.size(); i += static_cast<size_t>(stride)) {
            const auto& pt = deskewed_body.points[i];
            math::Vec3d p_lidar(pt.x, pt.y, pt.z);
            math::Vec3d p_body = T_il.transform(p_lidar);
            math::Vec3d p_world = state_.T_world_body.transform(p_body);

            PlaneCorrespondence corr;
            if (!local_map_.queryPlaneCorrespondence(p_body, p_world, config_.max_plane_corr_dist_m, corr)) {
                continue;
            }

            // Jacobian of residual r = n^T (R_wb * p_body + t_wb - q) w.r.t. [delta_t (3), delta_theta (3)]
            // d(R_wb * exp(delta_theta^) * p_body) / d(delta_theta) = -R_wb * [p_body]_x
            math::Vec3d R_n = state_.T_world_body.so3.R.transpose() * corr.plane_normal;
            math::Vec3d J_rot = p_body.cross(R_n);
            const math::Vec3d& J_pos = corr.plane_normal;

            double J[6] = {J_pos.x, J_pos.y, J_pos.z, J_rot.x, J_rot.y, J_rot.z};
            double w = corr.huber_weight / corr.measurement_variance;
            double r = corr.signed_distance;

            for (int r_idx = 0; r_idx < 6; ++r_idx) {
                HTr[r_idx] -= w * J[r_idx] * r;
                for (int c_idx = 0; c_idx < 6; ++c_idx) {
                    HTH[r_idx][c_idx] += w * J[r_idx] * J[c_idx];
                }
            }

            sum_sq_err += r * r;
            valid_corrs++;
        }

        io_metrics.geometric_correspondences = valid_corrs;
        if (valid_corrs < 12) break;

        io_metrics.geometric_rmse_m = std::sqrt(sum_sq_err / static_cast<double>(valid_corrs));

        // Analyze translational observability eigenvalues (top-left 3x3 block of H^T R^-1 H)
        math::Mat3d H_trans;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                H_trans(r, c) = HTH[r][c];
        math::SymEigen3d eig_trans = math::SymEigen3d::compute(H_trans);
        io_metrics.min_observability_eigenvalue = eig_trans.eigenvalues[0];
        io_metrics.is_degenerate = (eig_trans.eigenvalues[0] < config_.degeneracy_eigenvalue_thresh);

        // Add prior covariance regularization P^-1
        for (int i = 0; i < 3; ++i) {
            HTH[i][i]     += 1.0 / std::max(1e-6, P_diag_[i + 3]);
            HTH[i + 3][i + 3] += 1.0 / std::max(1e-6, P_diag_[i + 0]);
        }

        double dx[6]{};
        if (!solve6x6LinearSystem(HTH, HTr, dx)) break;

        math::Vec3d dt(dx[0], dx[1], dx[2]);
        math::Vec3d dtheta(dx[3], dx[4], dx[5]);
        state_.T_world_body.t += dt;
        state_.T_world_body.so3 = state_.T_world_body.so3 * math::SO3d::exp(dtheta);

        if (dt.norm() < 1e-4 && dtheta.norm() < 1e-4) {
            // Update posterior covariance diagonal
            for (int i = 0; i < 3; ++i) {
                P_diag_[i + 3] = 1.0 / std::max(1.0, HTH[i][i]);
                P_diag_[i + 0] = 1.0 / std::max(1.0, HTH[i + 3][i + 3]);
            }
            break;
        }
    }
}

float FastLivo2Engine::sampleBilinearGray(const core::RgbFrame& img, double u, double v) {
    if (img.gray_bytes.empty() || u < 0.0 || v < 0.0 ||
        u >= static_cast<double>(img.intrinsics.width - 1) ||
        v >= static_cast<double>(img.intrinsics.height - 1)) {
        return 0.0f;
    }
    int x0 = static_cast<int>(std::floor(u));
    int y0 = static_cast<int>(std::floor(v));
    double ax = u - x0;
    double ay = v - y0;
    int w = img.intrinsics.width;

    float i00 = static_cast<float>(img.gray_bytes[static_cast<size_t>(y0 * w + x0)]);
    float i10 = static_cast<float>(img.gray_bytes[static_cast<size_t>(y0 * w + x0 + 1)]);
    float i01 = static_cast<float>(img.gray_bytes[static_cast<size_t>((y0 + 1) * w + x0)]);
    float i11 = static_cast<float>(img.gray_bytes[static_cast<size_t>((y0 + 1) * w + x0 + 1)]);

    return static_cast<float>((1.0 - ax) * (1.0 - ay) * i00 +
                              ax * (1.0 - ay) * i10 +
                              (1.0 - ax) * ay * i01 +
                              ax * ay * i11);
}

void FastLivo2Engine::sampleBilinearRgb(const core::RgbFrame& img, double u, double v,
                                        uint8_t& r, uint8_t& g, uint8_t& b) {
    if (img.rgb_bytes.size() < static_cast<size_t>(img.intrinsics.width * img.intrinsics.height * 3)) {
        uint8_t gray = static_cast<uint8_t>(std::clamp(sampleBilinearGray(img, u, v), 0.0f, 255.0f));
        r = gray; g = gray; b = gray;
        return;
    }
    int x = std::clamp(static_cast<int>(std::round(u)), 0, img.intrinsics.width - 1);
    int y = std::clamp(static_cast<int>(std::round(v)), 0, img.intrinsics.height - 1);
    size_t idx = static_cast<size_t>((y * img.intrinsics.width + x) * 3);
    r = img.rgb_bytes[idx + 0];
    g = img.rgb_bytes[idx + 1];
    b = img.rgb_bytes[idx + 2];
}

void FastLivo2Engine::runDirectVisualUpdate(const core::RgbFrame& rgb_frame,
                                            const core::LidarFrame& /*deskewed_body*/,
                                            EstimatorMetrics& io_metrics) {
    if (!config_.enable_visual_fusion || !extrinsic_profile_.verified ||
        extrinsic_profile_.confidence_score < 0.50 || rgb_frame.gray_bytes.empty()) {
        return;
    }

    auto patches = local_map_.getActiveVisualPatches(state_.T_world_body.t, 12.0);
    if (patches.size() < 4) return;

    // T_cam_world = T_cam_lidar * T_lidar_body * T_body_world
    const math::SE3d T_lidar_body = factory_calib_.T_imu_lidar.inverse();
    const math::SE3d T_cam_body = extrinsic_profile_.T_cam_lidar * T_lidar_body;

    for (int iter = 0; iter < 3; ++iter) {
        math::SE3d T_cam_world = T_cam_body * state_.T_world_body.inverse();
        double HTH[6][6]{};
        double HTr[6]{};
        size_t valid_patches = 0;
        double sum_photo_err2 = 0.0;

        for (const auto& patch : patches) {
            math::Vec3d p_cam = T_cam_world.transform(patch.center_world);
            if (!rgb_frame.intrinsics.isInFieldOfView(p_cam, 10.0)) continue;

            math::Vec2d uv = rgb_frame.intrinsics.project(p_cam, true);
            // Exposure ratio compensation between current frame and reference patch frame
            double exp_scale = (patch.ref_exposure_sec > 1e-5 && rgb_frame.exposure_duration_sec > 1e-5)
                ? std::clamp(patch.ref_exposure_sec / rgb_frame.exposure_duration_sec, 0.5, 2.0)
                : 1.0;

            float I_curr = static_cast<float>(sampleBilinearGray(rgb_frame, uv.x, uv.y) * exp_scale);
            float I_ref = patch.patch_intensities[27];  // Center pixel of 8x8 patch
            double residual = static_cast<double>(I_curr - I_ref);
            if (std::abs(residual) > 85.0) continue;

            // Image gradient at (u, v)
            double gx = 0.5 * (sampleBilinearGray(rgb_frame, uv.x + 1.0, uv.y) -
                               sampleBilinearGray(rgb_frame, uv.x - 1.0, uv.y));
            double gy = 0.5 * (sampleBilinearGray(rgb_frame, uv.x, uv.y + 1.0) -
                               sampleBilinearGray(rgb_frame, uv.x, uv.y - 1.0));
            if (gx * gx + gy * gy < 25.0) continue;

            double inv_z = 1.0 / p_cam.z;
            double inv_z2 = inv_z * inv_z;
            double fx = rgb_frame.intrinsics.fx;
            double fy = rgb_frame.intrinsics.fy;

            // d(u,v)/d(p_cam) multiplied by image gradient [gx, gy]
            math::Vec3d J_pcam(
                gx * fx * inv_z,
                gy * fy * inv_z,
                -(gx * fx * p_cam.x + gy * fy * p_cam.y) * inv_z2
            );

            // Transform Jacobian into body state perturbation [dt_wb, dtheta_wb]
            math::Vec3d p_body = state_.T_world_body.inverse().transform(patch.center_world);
            math::Vec3d J_pbody = -(T_cam_body.so3.R.transpose() * J_pcam);
            math::Vec3d J_pos = state_.T_world_body.so3.R * J_pbody;
            math::Vec3d J_rot = p_body.cross(J_pbody);

            double J[6] = {J_pos.x, J_pos.y, J_pos.z, J_rot.x, J_rot.y, J_rot.z};
            constexpr double kPhotoVar = 100.0;  // (10 gray levels)^2
            double w = 1.0 / kPhotoVar;

            for (int r = 0; r < 6; ++r) {
                HTr[r] -= w * J[r] * residual;
                for (int c = 0; c < 6; ++c) {
                    HTH[r][c] += w * J[r] * J[c];
                }
            }
            sum_photo_err2 += residual * residual;
            valid_patches++;
        }

        io_metrics.visual_patch_correspondences = valid_patches;
        if (valid_patches < 4) break;

        io_metrics.photometric_rmse = std::sqrt(sum_photo_err2 / static_cast<double>(valid_patches));
        for (int i = 0; i < 6; ++i) HTH[i][i] += 100.0;

        double dx[6]{};
        if (!solve6x6LinearSystem(HTH, HTr, dx)) break;

        state_.T_world_body.t += math::Vec3d(dx[0], dx[1], dx[2]);
        state_.T_world_body.so3 = state_.T_world_body.so3 * math::SO3d::exp(math::Vec3d(dx[3], dx[4], dx[5]));
        io_metrics.visual_update_applied = true;
    }
}

void FastLivo2Engine::colorizeAndExtractVisualPatches(const core::RgbFrame& rgb_frame,
                                                      core::LidarFrame& io_deskewed_body,
                                                      core::LidarFrame& io_world_frame) {
    if (!extrinsic_profile_.verified || rgb_frame.gray_bytes.empty()) return;

    const math::SE3d T_lidar_body = factory_calib_.T_imu_lidar.inverse();
    const math::SE3d T_cam_body = extrinsic_profile_.T_cam_lidar * T_lidar_body;
    const math::SE3d T_cam_world = T_cam_body * state_.T_world_body.inverse();

    size_t patches_added = 0;
    for (size_t i = 0; i < io_deskewed_body.points.size() && i < io_world_frame.points.size(); ++i) {
        auto& pt_body = io_deskewed_body.points[i];
        auto& pt_world = io_world_frame.points[i];

        math::Vec3d pw(pt_world.x, pt_world.y, pt_world.z);
        math::Vec3d p_cam = T_cam_world.transform(pw);
        // Restrict strictly to camera overlapping field of view (hemispherical points outside FOV remain geometry-only)
        if (!rgb_frame.intrinsics.isInFieldOfView(p_cam, 8.0)) {
            continue;
        }

        math::Vec2d uv = rgb_frame.intrinsics.project(p_cam, true);
        if (config_.enable_rgb_colorization) {
            uint8_t r = 200, g = 200, b = 200;
            sampleBilinearRgb(rgb_frame, uv.x, uv.y, r, g, b);
            pt_body.r = r; pt_body.g = g; pt_body.b = b; pt_body.colorized = true;
            pt_world.r = r; pt_world.g = g; pt_world.b = b; pt_world.colorized = true;
        }

        // Extract up to 24 high-gradient 8x8 visual patches per frame
        if (patches_added < 24 && (i % 8 == 0)) {
            double gx = sampleBilinearGray(rgb_frame, uv.x + 1.0, uv.y) - sampleBilinearGray(rgb_frame, uv.x - 1.0, uv.y);
            double gy = sampleBilinearGray(rgb_frame, uv.x, uv.y + 1.0) - sampleBilinearGray(rgb_frame, uv.x, uv.y - 1.0);
            if (gx * gx + gy * gy > 100.0) {
                VisualPatch patch;
                patch.center_world = pw;
                patch.ref_T_cam_world = T_cam_world;
                patch.ref_intrinsics = rgb_frame.intrinsics;
                patch.ref_exposure_sec = rgb_frame.exposure_duration_sec;
                patch.last_updated_frame = rgb_frame.frame_id;
                patch.valid = true;
                for (int py = 0; py < 8; ++py) {
                    for (int px = 0; px < 8; ++px) {
                        patch.patch_intensities[static_cast<size_t>(py * 8 + px)] =
                            sampleBilinearGray(rgb_frame, uv.x + (px - 3), uv.y + (py - 3));
                    }
                }
                local_map_.updateVisualPatch(pw, patch);
                patches_added++;
            }
        }
    }
}

bool FastLivo2Engine::processBundle(const sync::SyncBundle& bundle,
                                    core::LidarFrame& out_deskewed_body_frame,
                                    core::LidarFrame& out_world_frame,
                                    EstimatorMetrics& out_metrics) {
    auto t0 = std::chrono::steady_clock::now();
    out_metrics = EstimatorMetrics{};

    if (bundle.lidar_frame.points.empty()) {
        return false;
    }

    if (state_.status == core::TrackingStatus::UNINITIALIZED ||
        state_.status == core::TrackingStatus::INITIALIZING_IMU) {
        if (!initializeImuGravity(bundle.imu_measurements)) {
            if (!bundle.imu_measurements.empty()) return false;
            // Fallback to static initialization if operating in LiDAR-only replay mode
            state_.timestamp_sec = bundle.lidar_frame.timestamp_end_sec;
            state_.status = core::TrackingStatus::TRACKING_LIO;
        }
    }

    std::vector<ImuPropagatedPose> imu_traj;
    propagateImuAndDeskew(bundle, imu_traj, out_deskewed_body_frame);
    out_metrics.deskewed_points_count = out_deskewed_body_frame.points.size();

    runGeometricIekfUpdate(out_deskewed_body_frame, out_metrics);

    if (bundle.rgb_frame.has_value()) {
        runDirectVisualUpdate(bundle.rgb_frame.value(), out_deskewed_body_frame, out_metrics);
    }

    // Transform deskewed points into world coordinate frame
    out_world_frame = out_deskewed_body_frame;
    const math::SE3d T_world_lidar = state_.T_world_body * factory_calib_.T_imu_lidar;
    for (auto& pt : out_world_frame.points) {
        math::Vec3d pw = T_world_lidar.transform(math::Vec3d(pt.x, pt.y, pt.z));
        pt.x = pw.x;
        pt.y = pw.y;
        pt.z = pw.z;
    }

    if (bundle.rgb_frame.has_value()) {
        colorizeAndExtractVisualPatches(bundle.rgb_frame.value(), out_deskewed_body_frame, out_world_frame);
    }

    // Update local voxel map
    local_map_.insertPoints(out_world_frame.points, bundle.lidar_frame.frame_id);
    local_map_.pruneDistantOrStaleVoxels(state_.T_world_body.t, 45.0, bundle.lidar_frame.frame_id);

    // Determine keyframe emission criteria
    if (!has_first_keyframe_) {
        last_keyframe_pose_ = state_.T_world_body;
        has_first_keyframe_ = true;
        out_metrics.keyframe_emitted = true;
    } else {
        math::SE3d dT = last_keyframe_pose_.inverse() * state_.T_world_body;
        double trans_dist = dT.t.norm();
        double rot_deg = dT.so3.log().norm() * math::kRadToDeg;
        if (trans_dist >= config_.keyframe_translation_thresh_m ||
            rot_deg >= config_.keyframe_rotation_thresh_deg) {
            last_keyframe_pose_ = state_.T_world_body;
            out_metrics.keyframe_emitted = true;
        }
    }

    state_.position_uncertainty_m = std::sqrt(P_diag_[3] + P_diag_[4] + P_diag_[5]);
    state_.orientation_uncertainty_deg = std::sqrt(P_diag_[0] + P_diag_[1] + P_diag_[2]) * math::kRadToDeg;
    if (out_metrics.is_degenerate) {
        state_.status = core::TrackingStatus::DEGENERATE_GEOMETRY;
    } else if (out_metrics.visual_update_applied) {
        state_.status = core::TrackingStatus::TRACKING_LIVO;
    } else {
        state_.status = core::TrackingStatus::TRACKING_LIO;
    }

    trajectory_.push_back(state_);
    out_metrics.frames_processed = trajectory_.size();

    auto t1 = std::chrono::steady_clock::now();
    out_metrics.processing_time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return true;
}

}  // namespace slam
}  // namespace avscan
