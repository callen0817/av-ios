#include "avscan/slam/FastLivo2Engine.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace avscan {
namespace slam {

namespace {

constexpr double kGravityMag = 9.80665;

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
            for (int j = k; j <= 6; ++j) M[i][j] -= factor * M[k][j];
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
    : config_(config), local_map_(config.voxel_size_m, 50, 60000) {
    reset();
}

void FastLivo2Engine::setFactoryCalibration(const core::AiryFactoryCalibration& calib) {
    const double d_rot_deg =
        (factory_calib_.T_imu_lidar.so3.inverse() * calib.T_imu_lidar.so3).log().norm() * math::kRadToDeg;
    factory_calib_ = calib;
    if (d_rot_deg > 5.0 && trajectory_.size() <= 15) {
        local_map_.clear();
    }
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
    state_.gravity_mps2 = {0.0, 0.0, -kGravityMag};
    state_.status = core::TrackingStatus::UNINITIALIZED;
    P_ = Mat18::Zero();
    for (int i = 0; i < 3; ++i) {
        P_(i + 0, i + 0)   = 1e-4;  // Orientation variance (rad^2)
        P_(i + 3, i + 3)   = 1e-4;  // Position variance (m^2)
        P_(i + 6, i + 6)   = 1e-2;  // Velocity variance (m^2/s^2)
        P_(i + 9, i + 9)   = 1e-4;  // Gyro bias variance
        P_(i + 12, i + 12) = 1e-3;  // Accel bias variance
        P_(i + 15, i + 15) = 1e-4;  // Gravity vector variance
    }
    local_map_.clear();
    init_imu_accum_.clear();
    has_last_imu_ = false;
    acc_scale_ = 1.0;
    trajectory_.clear();
    recent_pose_history_.clear();
    last_keyframe_pose_ = math::SE3d::Identity();
    has_first_keyframe_ = false;
}

FastLivo2Engine::Mat18 FastLivo2Engine::mul18x18(const Mat18& A, const Mat18& B) {
    Mat18 C = Mat18::Zero();
    for (int i = 0; i < 18; ++i) {
        for (int k = 0; k < 18; ++k) {
            const double aik = A.m[i][k];
            if (aik == 0.0) continue;
            for (int j = 0; j < 18; ++j) {
                C.m[i][j] += aik * B.m[k][j];
            }
        }
    }
    return C;
}

FastLivo2Engine::Mat18 FastLivo2Engine::mul18x18Transpose(const Mat18& A, const Mat18& B) {
    Mat18 C = Mat18::Zero();
    for (int i = 0; i < 18; ++i) {
        for (int j = 0; j < 18; ++j) {
            double s = 0.0;
            for (int k = 0; k < 18; ++k) {
                s += A.m[i][k] * B.m[j][k];
            }
            C.m[i][j] = s;
        }
    }
    return C;
}

bool FastLivo2Engine::invert18x18(const Mat18& A, Mat18& out_inv) {
    double M[18][36]{};
    for (int i = 0; i < 18; ++i) {
        for (int j = 0; j < 18; ++j) M[i][j] = A.m[i][j];
        M[i][18 + i] = 1.0;
    }
    for (int k = 0; k < 18; ++k) {
        int piv = k;
        double best = std::abs(M[k][k]);
        for (int i = k + 1; i < 18; ++i) {
            if (std::abs(M[i][k]) > best) {
                best = std::abs(M[i][k]);
                piv = i;
            }
        }
        if (best < 1e-14) return false;
        if (piv != k) {
            for (int j = 0; j < 36; ++j) std::swap(M[k][j], M[piv][j]);
        }
        const double inv_d = 1.0 / M[k][k];
        for (int j = 0; j < 36; ++j) M[k][j] *= inv_d;
        for (int i = 0; i < 18; ++i) {
            if (i == k) continue;
            const double f = M[i][k];
            if (f == 0.0) continue;
            for (int j = 0; j < 36; ++j) M[i][j] -= f * M[k][j];
        }
    }
    for (int i = 0; i < 18; ++i) {
        for (int j = 0; j < 18; ++j) {
            out_inv.m[i][j] = M[i][18 + j];
        }
    }
    return true;
}

bool FastLivo2Engine::initializeImuGravity(const std::vector<core::ImuMeasurement>& imus) {
    for (const auto& m : imus) {
        init_imu_accum_.push_back(m);
    }
    // Average at least 20 samples (~100 ms at 200 Hz) to resolve initial gravity alignment
    if (init_imu_accum_.size() < 20) {
        state_.status = core::TrackingStatus::INITIALIZING_IMU;
        return false;
    }

    math::Vec3d mean_acc(0.0, 0.0, 0.0);
    math::Vec3d mean_gyro(0.0, 0.0, 0.0);
    double max_gyro = 0.0;
    for (const auto& m : init_imu_accum_) {
        mean_acc += m.accel_mps2;
        mean_gyro += m.gyro_rads;
        max_gyro = std::max(max_gyro, m.gyro_rads.norm());
    }
    const double inv_n = 1.0 / static_cast<double>(init_imu_accum_.size());
    mean_acc *= inv_n;
    mean_gyro *= inv_n;

    const double acc_norm = mean_acc.norm();
    if (acc_norm > 5.0 && acc_norm < 15.0) {
        // Align world +Z with measured stationary specific force so gravity is [0, 0, -9.80665]
        math::Vec3d z_body_up = mean_acc / acc_norm;
        math::Vec3d x_seed = (std::abs(z_body_up.x) < 0.9) ? math::Vec3d(1.0, 0.0, 0.0) : math::Vec3d(0.0, 1.0, 0.0);
        math::Vec3d y_body = z_body_up.cross(x_seed).normalized();
        math::Vec3d x_body = y_body.cross(z_body_up).normalized();

        // Rows of R_wb are x_body, y_body, z_body_up so R_wb * z_body_up = [0, 0, +1]
        math::Mat3d R_wb;
        R_wb(0, 0) = x_body.x;    R_wb(0, 1) = x_body.y;    R_wb(0, 2) = x_body.z;
        R_wb(1, 0) = y_body.x;    R_wb(1, 1) = y_body.y;    R_wb(1, 2) = y_body.z;
        R_wb(2, 0) = z_body_up.x; R_wb(2, 1) = z_body_up.y; R_wb(2, 2) = z_body_up.z;

        state_.T_world_body = math::SE3d(R_wb, math::Vec3d(0.0, 0.0, 0.0));
        state_.gravity_mps2 = math::Vec3d(0.0, 0.0, -kGravityMag);

        // CRITICAL: Only seed gyro bias & accel scale if genuinely stationary (< 0.05 rad/s peak).
        // Never absorb hand motion during startup into permanent bias_gyro!
        if (max_gyro < 0.05) {
            acc_scale_ = kGravityMag / acc_norm;
            state_.bias_gyro_rads = mean_gyro;
            state_.bias_accel_mps2 = {0.0, 0.0, 0.0};
        } else {
            acc_scale_ = 1.0;
            state_.bias_gyro_rads = {0.0, 0.0, 0.0};
            state_.bias_accel_mps2 = {0.0, 0.0, 0.0};
            // Give the 18x18 ES-IEKF generous initial bias/gravity uncertainty so it converges rapidly online
            for (int i = 0; i < 3; ++i) {
                P_(i + 9, i + 9)   = 1e-3;
                P_(i + 12, i + 12) = 1e-2;
                P_(i + 15, i + 15) = 1e-2;
            }
        }
    }

    state_.velocity_mps = {0.0, 0.0, 0.0};
    state_.timestamp_sec = init_imu_accum_.back().timestamp_sec;
    last_imu_ = init_imu_accum_.back();
    has_last_imu_ = true;
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

    // Build continuous IMU sequence starting from previous sweep's tail measurement (matches FAST-LIVO2)
    std::vector<core::ImuMeasurement> v_imu;
    v_imu.reserve(bundle.imu_measurements.size() + 1);
    if (has_last_imu_) {
        v_imu.push_back(last_imu_);
    }
    for (const auto& m : bundle.imu_measurements) {
        if (v_imu.empty() || m.timestamp_sec > v_imu.back().timestamp_sec) {
            v_imu.push_back(m);
        }
    }

    const double prop_beg = (state_.timestamp_sec > 0.0) ? state_.timestamp_sec : t_scan_start;
    const double prop_end = t_scan_end;

    ImuPropagatedPose pose0;
    pose0.timestamp_sec = prop_beg;
    pose0.T_world_body = state_.T_world_body;
    pose0.velocity_world = state_.velocity_mps;
    out_imu_traj.push_back(pose0);

    // Conservative stationary ZUPT detection (only when truly still)
    bool is_stationary_zupt = false;
    if (bundle.imu_measurements.size() >= 6) {
        double max_gyro = 0.0;
        double max_acc_dev = 0.0;
        for (const auto& imu : bundle.imu_measurements) {
            math::Vec3d w = imu.gyro_rads - state_.bias_gyro_rads;
            math::Vec3d ab = imu.accel_mps2 * acc_scale_ - state_.bias_accel_mps2;
            math::Vec3d aw = state_.T_world_body.so3 * ab + state_.gravity_mps2;
            max_gyro = std::max(max_gyro, w.norm());
            max_acc_dev = std::max(max_acc_dev, aw.norm());
        }
        if (max_gyro < 0.035 && max_acc_dev < 0.35 && state_.velocity_mps.norm() < 0.20) {
            is_stationary_zupt = true;
            state_.velocity_mps = {0.0, 0.0, 0.0};
        }
    }

    const double cov_gyr = config_.gyro_noise_sigma * config_.gyro_noise_sigma;
    const double cov_acc = config_.accel_noise_sigma * config_.accel_noise_sigma;
    const double cov_bg  = config_.bias_gyro_rw_sigma * config_.bias_gyro_rw_sigma;
    const double cov_ba  = config_.bias_accel_rw_sigma * config_.bias_accel_rw_sigma;

    math::Vec3d last_omega(0.0, 0.0, 0.0);
    math::Vec3d last_acc_world(0.0, 0.0, 0.0);

    for (size_t i = 0; i + 1 < v_imu.size(); ++i) {
        const auto& head = v_imu[i];
        const auto& tail = v_imu[i + 1];
        if (tail.timestamp_sec <= prop_beg) continue;
        if (head.timestamp_sec >= prop_end) break;

        const double t0 = std::max(head.timestamp_sec, prop_beg);
        const double t1 = std::min(tail.timestamp_sec, prop_end);
        const double dt = std::clamp(t1 - t0, 1e-5, 0.05);

        math::Vec3d omega = (head.gyro_rads + tail.gyro_rads) * 0.5 - state_.bias_gyro_rads;
        math::Vec3d acc_body = (head.accel_mps2 + tail.accel_mps2) * (0.5 * acc_scale_) - state_.bias_accel_mps2;

        // Error-state transition matrix F_x (18x18) matching FAST-LIVO2 IMU_Processing.cpp
        Mat18 Fx = Mat18::Identity();
        const math::Mat3d Exp_neg = math::SO3d::exp(-omega * dt).R;
        const math::Mat3d& R_curr = state_.T_world_body.so3.R;
        const math::Mat3d a_hat = math::SO3d::hat(acc_body);
        const math::Mat3d minus_R_ahat_dt = (R_curr * a_hat) * (-dt);
        const math::Mat3d minus_R_dt = R_curr * (-dt);

        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                Fx(r + 0, c + 0)  = Exp_neg(r, c);
                Fx(r + 6, c + 0)  = minus_R_ahat_dt(r, c);
                Fx(r + 6, c + 12) = minus_R_dt(r, c);
            }
            Fx(r + 0, r + 9)  = -dt;
            Fx(r + 3, r + 6)  =  dt;
            Fx(r + 6, r + 15) =  dt;
        }

        // Propagate full 18x18 covariance P = Fx * P * Fx^T + Q
        P_ = mul18x18Transpose(mul18x18(Fx, P_), Fx);
        for (int r = 0; r < 3; ++r) {
            P_(r + 0, r + 0)   += cov_gyr * dt;
            P_(r + 6, r + 6)   += cov_acc * dt;
            P_(r + 3, r + 3)   += cov_acc * (dt * dt * dt) * 0.25 + 1e-7 * dt;
            P_(r + 9, r + 9)   += cov_bg * dt;
            P_(r + 12, r + 12) += cov_ba * dt;
        }
        // Symmetrize
        for (int r = 0; r < 18; ++r) {
            for (int c = r + 1; c < 18; ++c) {
                double avg = 0.5 * (P_(r, c) + P_(c, r));
                P_(r, c) = P_(c, r) = avg;
            }
        }

        if (!is_stationary_zupt) {
            state_.T_world_body.so3 = state_.T_world_body.so3 * math::SO3d::exp(omega * dt);
            math::Vec3d acc_world = state_.T_world_body.so3 * acc_body + state_.gravity_mps2;
            state_.T_world_body.t += state_.velocity_mps * dt + acc_world * (0.5 * dt * dt);
            state_.velocity_mps += acc_world * dt;
            const double v_norm = state_.velocity_mps.norm();
            if (v_norm > 6.0) {
                state_.velocity_mps *= (6.0 / v_norm);
            }
            last_omega = omega;
            last_acc_world = acc_world;
        }
        state_.timestamp_sec = t1;

        ImuPropagatedPose step;
        step.timestamp_sec = t1;
        step.T_world_body = state_.T_world_body;
        step.velocity_world = state_.velocity_mps;
        step.gyro_unbiased = is_stationary_zupt ? math::Vec3d(0.0, 0.0, 0.0) : omega;
        step.accel_world = is_stationary_zupt ? math::Vec3d(0.0, 0.0, 0.0) : last_acc_world;
        out_imu_traj.push_back(step);
    }

    if (!v_imu.empty()) {
        last_imu_ = v_imu.back();
        has_last_imu_ = true;
    }

    // Advance trajectory cleanly to exact sweep-end timestamp
    if (prop_end > state_.timestamp_sec) {
        const double dt = std::clamp(prop_end - state_.timestamp_sec, 0.0, 0.05);
        if (!is_stationary_zupt) {
            state_.T_world_body.so3 = state_.T_world_body.so3 * math::SO3d::exp(last_omega * dt);
            state_.T_world_body.t += state_.velocity_mps * dt + last_acc_world * (0.5 * dt * dt);
            state_.velocity_mps += last_acc_world * dt;
        }
        state_.timestamp_sec = prop_end;
        ImuPropagatedPose end_step;
        end_step.timestamp_sec = prop_end;
        end_step.T_world_body = state_.T_world_body;
        end_step.velocity_world = state_.velocity_mps;
        end_step.gyro_unbiased = last_omega;
        end_step.accel_world = last_acc_world;
        out_imu_traj.push_back(end_step);
    }

    // Fast per-segment backward motion deskewing directly into sweep-end IMU BODY frame!
    // Precompute segment Lie deltas once per ~20 IMU intervals (10x faster than per-point SE3::interpolate).
    const math::SE3d T_end_inv = state_.T_world_body.inverse();
    const math::SE3d& T_imu_lidar = factory_calib_.T_imu_lidar;
    const size_t n_traj = out_imu_traj.size();

    struct SegPrecomp {
        double t0{0.0}, inv_dt{0.0};
        math::Mat3d R_end_prev{};
        math::Vec3d t_end_prev{};
        math::Vec3d d_omega{};
        math::Vec3d d_trans_end{};
    };
    std::vector<SegPrecomp> segs;
    if (n_traj >= 2) {
        segs.resize(n_traj - 1);
        for (size_t s = 0; s + 1 < n_traj; ++s) {
            const auto& p0 = out_imu_traj[s];
            const auto& p1 = out_imu_traj[s + 1];
            const double sdt = p1.timestamp_sec - p0.timestamp_sec;
            segs[s].t0 = p0.timestamp_sec;
            segs[s].inv_dt = (sdt > 1e-7) ? (1.0 / sdt) : 0.0;
            const math::SE3d T_end_p0 = T_end_inv * p0.T_world_body;
            const math::SE3d T_end_p1 = T_end_inv * p1.T_world_body;
            segs[s].R_end_prev = T_end_p0.so3.R;
            segs[s].t_end_prev = T_end_p0.t;
            segs[s].d_omega = (T_end_p0.so3.inverse() * T_end_p1.so3).log();
            segs[s].d_trans_end = T_end_p1.t - T_end_p0.t;
        }
    }

    size_t seg_hint = 0;
    for (auto& pt : out_deskewed_body.points) {
        const math::Vec3d p_imu_i = T_imu_lidar.transform(math::Vec3d(pt.x, pt.y, pt.z));

        if (!segs.empty() && pt.timestamp_sec >= out_imu_traj.front().timestamp_sec) {
            // Advance monotonic hint or binary search when points arrive slightly out of order across beams
            if (pt.timestamp_sec < out_imu_traj[seg_hint].timestamp_sec) {
                seg_hint = 0;
            }
            while (seg_hint + 1 < segs.size() && pt.timestamp_sec > out_imu_traj[seg_hint + 1].timestamp_sec) {
                seg_hint++;
            }
            const auto& sg = segs[seg_hint];
            const double alpha = std::clamp((pt.timestamp_sec - sg.t0) * sg.inv_dt, 0.0, 1.0);
            const math::Vec3d w = sg.d_omega * alpha;
            // 2nd-order Rodrigues approximation (exact to <1e-7 rad over 5 ms IMU step)
            const math::Vec3d w_x_p = w.cross(p_imu_i);
            const math::Vec3d p_rot = p_imu_i + w_x_p + w.cross(w_x_p) * 0.5;
            const math::Vec3d p_body_end = (sg.R_end_prev * p_rot) + sg.t_end_prev + sg.d_trans_end * alpha;
            pt.x = p_body_end.x;
            pt.y = p_body_end.y;
            pt.z = p_body_end.z;
        } else {
            pt.x = p_imu_i.x;
            pt.y = p_imu_i.y;
            pt.z = p_imu_i.z;
        }
    }
}

void FastLivo2Engine::runGeometricIekfUpdate(const std::vector<math::Vec3d>& feats_body,
                                             EstimatorMetrics& io_metrics) {
    io_metrics.evaluated_features_count = feats_body.size();
    if (local_map_.numValidPlanes() < 5 || feats_body.empty()) {
        return;
    }

    const core::SlamState x_prop = state_;
    Mat18 P_inv;
    if (!invert18x18(P_, P_inv)) {
        return;
    }

    Mat18 A_final_inv;
    bool updated = false;

    for (int iter = 0; iter < config_.max_iekf_iterations; ++iter) {
        double HTH6[6][6]{};
        double HTr6[6]{};
        size_t valid_corrs = 0;
        double sum_sq_err = 0.0;
        const double corr_thresh = (iter == 0) ? std::max(0.45, config_.max_plane_corr_dist_m)
                                               : config_.max_plane_corr_dist_m;

        for (const auto& p_body : feats_body) {
            const math::Vec3d p_world = state_.T_world_body.transform(p_body);
            PlaneCorrespondence corr;
            if (!local_map_.queryPlaneCorrespondence(p_body, p_world, corr_thresh, corr)) {
                continue;
            }

            // State ordering matches 18-state covariance: [rot(0..2), pos(3..5)]
            const math::Vec3d R_n = state_.T_world_body.so3.R.transpose() * corr.plane_normal;
            const math::Vec3d J_rot = p_body.cross(R_n);
            const math::Vec3d& J_pos = corr.plane_normal;

            const double J[6] = {J_rot.x, J_rot.y, J_rot.z, J_pos.x, J_pos.y, J_pos.z};
            const double R_var = std::max(0.001, corr.measurement_variance);
            const double w = corr.huber_weight / R_var;
            const double z = -corr.signed_distance;

            for (int r = 0; r < 6; ++r) {
                HTr6[r] += w * J[r] * z;
                for (int c = 0; c < 6; ++c) {
                    HTH6[r][c] += w * J[r] * J[c];
                }
            }
            sum_sq_err += corr.signed_distance * corr.signed_distance;
            valid_corrs++;
        }

        io_metrics.geometric_correspondences = valid_corrs;
        if (valid_corrs < 12) break;
        io_metrics.geometric_rmse_m = std::sqrt(sum_sq_err / static_cast<double>(valid_corrs));

        // Translational observability analysis (pos block is indices [3..5])
        math::Mat3d H_trans;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                H_trans(r, c) = HTH6[r + 3][c + 3];
            }
        }
        const math::SymEigen3d eig_trans = math::SymEigen3d::compute(H_trans);
        io_metrics.min_observability_eigenvalue = eig_trans.eigenvalues[0];
        io_metrics.is_degenerate = (eig_trans.eigenvalues[0] < config_.degeneracy_eigenvalue_thresh);

        // Box-minus state difference vec = x_prop [-] state_
        double vec[18]{};
        const math::Vec3d drot_prop = (state_.T_world_body.so3.inverse() * x_prop.T_world_body.so3).log();
        const math::Vec3d dpos_prop = x_prop.T_world_body.t - state_.T_world_body.t;
        const math::Vec3d dvel_prop = x_prop.velocity_mps - state_.velocity_mps;
        const math::Vec3d dbg_prop  = x_prop.bias_gyro_rads - state_.bias_gyro_rads;
        const math::Vec3d dba_prop  = x_prop.bias_accel_mps2 - state_.bias_accel_mps2;
        const math::Vec3d dg_prop   = x_prop.gravity_mps2 - state_.gravity_mps2;
        for (int i = 0; i < 3; ++i) {
            vec[i + 0]  = drot_prop[i];
            vec[i + 3]  = dpos_prop[i];
            vec[i + 6]  = dvel_prop[i];
            vec[i + 9]  = dbg_prop[i];
            vec[i + 12] = dba_prop[i];
            vec[i + 15] = dg_prop[i];
        }

        // Exact ES-IEKF update: (P^-1 + H^T R^-1 H) dx = H^T R^-1 z + P^-1 vec
        Mat18 A = P_inv;
        for (int r = 0; r < 6; ++r) {
            for (int c = 0; c < 6; ++c) {
                A(r, c) += HTH6[r][c];
            }
        }
        for (int i = 0; i < 18; ++i) A(i, i) += 1e-9;

        double rhs[18]{};
        for (int r = 0; r < 18; ++r) {
            double s = (r < 6) ? HTr6[r] : 0.0;
            for (int c = 0; c < 18; ++c) {
                s += P_inv(r, c) * vec[c];
            }
            rhs[r] = s;
        }

        Mat18 A_inv;
        if (!invert18x18(A, A_inv)) break;

        double dx[18]{};
        for (int r = 0; r < 18; ++r) {
            double s = 0.0;
            for (int c = 0; c < 18; ++c) s += A_inv(r, c) * rhs[c];
            dx[r] = s;
        }

        math::Vec3d dtheta(dx[0], dx[1], dx[2]);
        math::Vec3d dt(dx[3], dx[4], dx[5]);
        if (dtheta.norm() > 0.35) dtheta *= (0.35 / dtheta.norm());
        if (dt.norm() > 0.50) dt *= (0.50 / dt.norm());

        state_.T_world_body.so3 = state_.T_world_body.so3 * math::SO3d::exp(dtheta);
        state_.T_world_body.t  += dt;
        state_.velocity_mps    += math::Vec3d(dx[6], dx[7], dx[8]);
        state_.bias_gyro_rads  += math::Vec3d(dx[9], dx[10], dx[11]);
        state_.bias_accel_mps2 += math::Vec3d(dx[12], dx[13], dx[14]);
        state_.gravity_mps2    += math::Vec3d(dx[15], dx[16], dx[17]);

        // Clamp biases & keep gravity norm locked to physical 9.80665 m/s^2
        for (int i = 0; i < 3; ++i) {
            state_.bias_gyro_rads[i] = std::clamp(state_.bias_gyro_rads[i], -0.25, 0.25);
            state_.bias_accel_mps2[i] = std::clamp(state_.bias_accel_mps2[i], -1.5, 1.5);
        }
        const double gn = state_.gravity_mps2.norm();
        if (gn > 1.0) {
            state_.gravity_mps2 *= (kGravityMag / gn);
        }

        A_final_inv = A_inv;
        updated = true;

        if (dtheta.norm() * math::kRadToDeg < 0.01 && dt.norm() < 0.0005) {
            break;
        }
    }

    if (updated) {
        P_ = A_final_inv;
        for (int i = 0; i < 18; ++i) {
            for (int j = i + 1; j < 18; ++j) {
                double avg = 0.5 * (P_(i, j) + P_(j, i));
                P_(i, j) = P_(j, i) = avg;
            }
        }
    } else {
        state_.velocity_mps *= 0.5;
    }
}

float FastLivo2Engine::sampleBilinearGray(const core::RgbFrame& img, double u, double v) {
    if (img.gray_bytes.empty() || u < 0.0 || v < 0.0 ||
        u >= static_cast<double>(img.intrinsics.width - 1) ||
        v >= static_cast<double>(img.intrinsics.height - 1)) {
        return 0.0f;
    }
    const int x0 = static_cast<int>(std::floor(u));
    const int y0 = static_cast<int>(std::floor(v));
    const double ax = u - x0;
    const double ay = v - y0;
    const int w = img.intrinsics.width;

    const float i00 = static_cast<float>(img.gray_bytes[static_cast<size_t>(y0 * w + x0)]);
    const float i10 = static_cast<float>(img.gray_bytes[static_cast<size_t>(y0 * w + x0 + 1)]);
    const float i01 = static_cast<float>(img.gray_bytes[static_cast<size_t>((y0 + 1) * w + x0)]);
    const float i11 = static_cast<float>(img.gray_bytes[static_cast<size_t>((y0 + 1) * w + x0 + 1)]);

    return static_cast<float>((1.0 - ax) * (1.0 - ay) * i00 +
                              ax * (1.0 - ay) * i10 +
                              (1.0 - ax) * ay * i01 +
                              ax * ay * i11);
}

void FastLivo2Engine::sampleBilinearRgb(const core::RgbFrame& img, double u, double v,
                                        uint8_t& r, uint8_t& g, uint8_t& b) {
    const int w = img.intrinsics.width;
    const int h = img.intrinsics.height;
    if (w < 2 || h < 2 || img.rgb_bytes.size() < static_cast<size_t>(w * h * 3)) {
        uint8_t gray = static_cast<uint8_t>(std::clamp(sampleBilinearGray(img, u, v), 0.0f, 255.0f));
        r = gray; g = gray; b = gray;
        return;
    }
    const double uc = std::clamp(u, 0.0, static_cast<double>(w - 1) - 1e-4);
    const double vc = std::clamp(v, 0.0, static_cast<double>(h - 1) - 1e-4);
    const int x0 = static_cast<int>(std::floor(uc));
    const int y0 = static_cast<int>(std::floor(vc));
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const double ax = uc - x0;
    const double ay = vc - y0;
    const double w00 = (1.0 - ax) * (1.0 - ay);
    const double w10 = ax * (1.0 - ay);
    const double w01 = (1.0 - ax) * ay;
    const double w11 = ax * ay;

    const size_t i00 = static_cast<size_t>((y0 * w + x0) * 3);
    const size_t i10 = static_cast<size_t>((y0 * w + x1) * 3);
    const size_t i01 = static_cast<size_t>((y1 * w + x0) * 3);
    const size_t i11 = static_cast<size_t>((y1 * w + x1) * 3);

    auto interpChan = [&](size_t c) -> uint8_t {
        double val = w00 * img.rgb_bytes[i00 + c] + w10 * img.rgb_bytes[i10 + c] +
                     w01 * img.rgb_bytes[i01 + c] + w11 * img.rgb_bytes[i11 + c];
        return static_cast<uint8_t>(std::clamp(std::round(val), 0.0, 255.0));
    };
    r = interpChan(0);
    g = interpChan(1);
    b = interpChan(2);
}

math::SE3d FastLivo2Engine::interpolateBodyPoseAtTime(
    double target_time_sec,
    const std::vector<ImuPropagatedPose>& sweep_traj) const {
    if (sweep_traj.size() >= 2 &&
        target_time_sec >= sweep_traj.front().timestamp_sec &&
        target_time_sec <= sweep_traj.back().timestamp_sec) {
        auto it = std::lower_bound(
            sweep_traj.begin(), sweep_traj.end(), target_time_sec,
            [](const ImuPropagatedPose& s, double t) { return s.timestamp_sec < t; });
        if (it == sweep_traj.begin()) return it->T_world_body;
        if (it == sweep_traj.end()) return sweep_traj.back().T_world_body;
        const auto& prev = *(it - 1);
        const double sdt = it->timestamp_sec - prev.timestamp_sec;
        const double alpha = (sdt > 1e-7) ? ((target_time_sec - prev.timestamp_sec) / sdt) : 1.0;
        return math::SE3d::interpolate(prev.T_world_body, it->T_world_body, alpha);
    }

    if (recent_pose_history_.size() >= 2) {
        if (target_time_sec <= recent_pose_history_.front().first) return recent_pose_history_.front().second;
        if (target_time_sec >= recent_pose_history_.back().first) return recent_pose_history_.back().second;
        auto it = std::lower_bound(
            recent_pose_history_.begin(), recent_pose_history_.end(), target_time_sec,
            [](const std::pair<double, math::SE3d>& p, double t) { return p.first < t; });
        if (it == recent_pose_history_.begin()) return it->second;
        if (it == recent_pose_history_.end()) return recent_pose_history_.back().second;
        const auto& prev = *(it - 1);
        const double sdt = it->first - prev.first;
        const double alpha = (sdt > 1e-7) ? ((target_time_sec - prev.first) / sdt) : 1.0;
        return math::SE3d::interpolate(prev.second, it->second, alpha);
    }
    return state_.T_world_body;
}

void FastLivo2Engine::runDirectVisualUpdate(const core::RgbFrame& rgb_frame,
                                            const core::LidarFrame& /*deskewed_body*/,
                                            EstimatorMetrics& io_metrics) {
    if (!config_.enable_visual_fusion || !extrinsic_profile_.verified ||
        extrinsic_profile_.confidence_score < 0.65 || rgb_frame.gray_bytes.empty()) {
        return;
    }

    auto patches = local_map_.getActiveVisualPatches(state_.T_world_body.t, 12.0);
    if (patches.size() < 4) return;

    const math::SE3d T_lidar_body = factory_calib_.T_imu_lidar.inverse();
    const math::SE3d T_cam_body = extrinsic_profile_.T_cam_lidar * T_lidar_body;

    for (int iter = 0; iter < 3; ++iter) {
        const math::SE3d T_cam_world = T_cam_body * state_.T_world_body.inverse();
        double HTH[6][6]{};
        double HTr[6]{};
        size_t valid_patches = 0;
        double sum_photo_err2 = 0.0;

        for (const auto& patch : patches) {
            const math::Vec3d p_cam = T_cam_world.transform(patch.center_world);
            if (!rgb_frame.intrinsics.isInFieldOfView(p_cam, 10.0)) continue;

            const math::Vec2d uv = rgb_frame.intrinsics.project(p_cam, true);
            const double exp_scale = (patch.ref_exposure_sec > 1e-5 && rgb_frame.exposure_duration_sec > 1e-5)
                ? std::clamp(patch.ref_exposure_sec / rgb_frame.exposure_duration_sec, 0.5, 2.0)
                : 1.0;

            const float I_curr = static_cast<float>(sampleBilinearGray(rgb_frame, uv.x, uv.y) * exp_scale);
            const float I_ref = patch.patch_intensities[27];
            const double residual = static_cast<double>(I_curr - I_ref);
            if (std::abs(residual) > 85.0) continue;

            const double gx = 0.5 * (sampleBilinearGray(rgb_frame, uv.x + 1.0, uv.y) -
                                     sampleBilinearGray(rgb_frame, uv.x - 1.0, uv.y));
            const double gy = 0.5 * (sampleBilinearGray(rgb_frame, uv.x, uv.y + 1.0) -
                                     sampleBilinearGray(rgb_frame, uv.x, uv.y - 1.0));
            if (gx * gx + gy * gy < 25.0) continue;

            const double inv_z = 1.0 / p_cam.z;
            const double inv_z2 = inv_z * inv_z;
            const double fx = rgb_frame.intrinsics.fx;
            const double fy = rgb_frame.intrinsics.fy;

            const math::Vec3d J_pcam(
                gx * fx * inv_z,
                gy * fy * inv_z,
                -(gx * fx * p_cam.x + gy * fy * p_cam.y) * inv_z2
            );
            const math::Vec3d p_body = state_.T_world_body.inverse().transform(patch.center_world);
            const math::Vec3d J_pbody = -(T_cam_body.so3.R.transpose() * J_pcam);
            const math::Vec3d J_pos = state_.T_world_body.so3.R * J_pbody;
            const math::Vec3d J_rot = p_body.cross(J_pbody);

            const double J[6] = {J_pos.x, J_pos.y, J_pos.z, J_rot.x, J_rot.y, J_rot.z};
            const double w = 1.0 / 100.0;

            for (int r = 0; r < 6; ++r) {
                HTr[r] -= w * J[r] * residual;
                for (int c = 0; c < 6; ++c) HTH[r][c] += w * J[r] * J[c];
            }
            sum_photo_err2 += residual * residual;
            valid_patches++;
        }

        io_metrics.visual_patch_correspondences = valid_patches;
        if (valid_patches < 4) break;

        io_metrics.photometric_rmse = std::sqrt(sum_photo_err2 / static_cast<double>(valid_patches));
        for (int i = 0; i < 6; ++i) HTH[i][i] += 200.0;

        double dx[6]{};
        if (!solve6x6LinearSystem(HTH, HTr, dx)) break;

        math::Vec3d dt(dx[0], dx[1], dx[2]);
        math::Vec3d dtheta(dx[3], dx[4], dx[5]);
        if (dt.norm() > 0.05) dt *= (0.05 / dt.norm());
        if (dtheta.norm() > 0.03) dtheta *= (0.03 / dtheta.norm());

        state_.T_world_body.t += dt;
        state_.T_world_body.so3 = state_.T_world_body.so3 * math::SO3d::exp(dtheta);
        io_metrics.visual_update_applied = true;
    }
}

void FastLivo2Engine::colorizeAndExtractVisualPatches(const core::RgbFrame& rgb_frame,
                                                      const std::vector<ImuPropagatedPose>& corrected_traj,
                                                      core::LidarFrame& io_deskewed_body,
                                                      core::LidarFrame& io_world_frame) {
    const size_t n_pts = std::min(io_deskewed_body.points.size(), io_world_frame.points.size());
    auto applyIntensityFallback = [&](size_t idx) {
        auto& pb = io_deskewed_body.points[idx];
        auto& pw = io_world_frame.points[idx];
        if (!pb.colorized) {
            uint8_t shade = static_cast<uint8_t>(std::clamp(55.0f + pb.intensity * 0.55f, 45.0f, 195.0f));
            pb.r = shade; pb.g = shade; pb.b = shade;
            pw.r = shade; pw.g = shade; pw.b = shade;
        }
    };

    const int img_w = rgb_frame.intrinsics.width;
    const int img_h = rgb_frame.intrinsics.height;
    // Never bake unverified / broken extrinsics into the point cloud; keep clean intensity shading until verified
    if (!config_.enable_rgb_colorization || !extrinsic_profile_.verified ||
        img_w < 16 || img_h < 16 ||
        (rgb_frame.gray_bytes.empty() && rgb_frame.rgb_bytes.empty())) {
        for (size_t i = 0; i < n_pts; ++i) applyIntensityFallback(i);
        return;
    }

    // Whip-pan angular-rate gate: skip RGB colorization during rapid camera rotation (> 1.25 rad/s)
    // so rolling-shutter motion blur never stains walls (LIO geometry tracking continues unabated).
    double omega_rgb_rads = 0.0;
    if (!corrected_traj.empty()) {
        double best_dt = 1e9;
        for (const auto& s : corrected_traj) {
            const double d = std::abs(s.timestamp_sec - rgb_frame.timestamp_sec);
            if (d < best_dt) {
                best_dt = d;
                omega_rgb_rads = s.gyro_unbiased.norm();
            }
        }
    }
    if (omega_rgb_rads > 1.25) {
        for (size_t i = 0; i < n_pts; ++i) applyIntensityFallback(i);
        return;
    }

    // SLERP-interpolate exact body pose at RGB acquisition timestamp (eliminates 50 ms lag smear!)
    const math::SE3d T_world_body_rgb = interpolateBodyPoseAtTime(rgb_frame.timestamp_sec, corrected_traj);
    const math::SE3d T_lidar_body = factory_calib_.T_imu_lidar.inverse();
    const math::SE3d T_cam_body = extrinsic_profile_.T_cam_lidar * T_lidar_body;
    const math::SE3d T_cam_world_rgb = T_cam_body * T_world_body_rgb.inverse();

    // Reject extreme lens border vignetting / outer distortion (~3% edge margin, min 6 px)
    const double fov_margin_px = std::max(6.0, 0.03 * static_cast<double>(std::min(img_w, img_h)));
    const double half_w = 0.5 * static_cast<double>(img_w);
    const double half_h = 0.5 * static_cast<double>(img_h);
    const double motion_factor = std::clamp(1.0 - (omega_rgb_rads / 1.5), 0.25, 1.0);

    // Pass 1: Project all points and populate coarse Z-buffer (2x2 pixel cells) for occlusion rejection
    const int zb_w = (img_w + 1) / 2;
    const int zb_h = (img_h + 1) / 2;
    std::vector<float> z_buffer(static_cast<size_t>(zb_w * zb_h), std::numeric_limits<float>::infinity());

    struct ProjInfo {
        float u{-1.0f};
        float v{-1.0f};
        float z{-1.0f};
        bool in_fov{false};
    };
    std::vector<ProjInfo> proj(n_pts);

    for (size_t i = 0; i < n_pts; ++i) {
        const auto& pt_world = io_world_frame.points[i];
        const math::Vec3d p_cam = T_cam_world_rgb.transform(math::Vec3d(pt_world.x, pt_world.y, pt_world.z));
        if (p_cam.z <= 0.20 || p_cam.z > 35.0) continue;
        if (!rgb_frame.intrinsics.isInFieldOfView(p_cam, fov_margin_px)) continue;

        const math::Vec2d uv = rgb_frame.intrinsics.project(p_cam, true);
        proj[i].u = static_cast<float>(uv.x);
        proj[i].v = static_cast<float>(uv.y);
        proj[i].z = static_cast<float>(p_cam.z);
        proj[i].in_fov = true;

        const int zx = std::clamp(static_cast<int>(uv.x) / 2, 0, zb_w - 1);
        const int zy = std::clamp(static_cast<int>(uv.y) / 2, 0, zb_h - 1);
        float& cell = z_buffer[static_cast<size_t>(zy * zb_w + zx)];
        if (proj[i].z < cell) cell = proj[i].z;
    }

    // Pass 2: Occlusion-gated true-bilinear RGB sampling + view-quality encoding + optional visual patches
    constexpr float kOcclusionTolMeters = 0.08f;
    size_t patches_added = 0;

    for (size_t i = 0; i < n_pts; ++i) {
        if (!proj[i].in_fov) {
            applyIntensityFallback(i);
            continue;
        }
        const int zx = std::clamp(static_cast<int>(proj[i].u) / 2, 0, zb_w - 1);
        const int zy = std::clamp(static_cast<int>(proj[i].v) / 2, 0, zb_h - 1);
        const float min_z = z_buffer[static_cast<size_t>(zy * zb_w + zx)];
        if (proj[i].z > min_z + kOcclusionTolMeters) {
            applyIntensityFallback(i);
            continue;
        }

        uint8_t r = 200, g = 200, b = 200;
        sampleBilinearRgb(rgb_frame, proj[i].u, proj[i].v, r, g, b);

        // Compute 6-bit RGB observation quality [1..63] (prefers centered, close-to-mid-range, low-motion views)
        // and pack into upper 6 bits of status_flags without altering sizeof(LidarPointXYZIRT).
        const double nx = (static_cast<double>(proj[i].u) - half_w) / std::max(1.0, half_w);
        const double ny = (static_cast<double>(proj[i].v) - half_h) / std::max(1.0, half_h);
        const double center_w = std::clamp(1.0 - 0.55 * (nx * nx + ny * ny), 0.20, 1.0);
        const double range_w = 1.0 / (1.0 + 0.12 * std::max(0.0, static_cast<double>(proj[i].z) - 1.5));
        const uint8_t q6 = static_cast<uint8_t>(
            std::clamp(std::round(63.0 * center_w * range_w * motion_factor), 1.0, 63.0));

        auto& pt_body = io_deskewed_body.points[i];
        auto& pt_world = io_world_frame.points[i];
        pt_body.r = r; pt_body.g = g; pt_body.b = b; pt_body.colorized = true;
        pt_body.status_flags = static_cast<uint8_t>((pt_body.status_flags & 0x03u) | (q6 << 2));
        pt_world.r = r; pt_world.g = g; pt_world.b = b; pt_world.colorized = true;
        pt_world.status_flags = pt_body.status_flags;

        if (config_.enable_visual_fusion && !rgb_frame.gray_bytes.empty() && patches_added < 24 && (i % 8 == 0)) {
            const double gx = sampleBilinearGray(rgb_frame, proj[i].u + 1.0, proj[i].v) -
                              sampleBilinearGray(rgb_frame, proj[i].u - 1.0, proj[i].v);
            const double gy = sampleBilinearGray(rgb_frame, proj[i].u, proj[i].v + 1.0) -
                              sampleBilinearGray(rgb_frame, proj[i].u, proj[i].v - 1.0);
            if (gx * gx + gy * gy > 100.0) {
                VisualPatch patch;
                patch.center_world = math::Vec3d(pt_world.x, pt_world.y, pt_world.z);
                patch.ref_T_cam_world = T_cam_world_rgb;
                patch.ref_intrinsics = rgb_frame.intrinsics;
                patch.ref_exposure_sec = rgb_frame.exposure_duration_sec;
                patch.last_updated_frame = rgb_frame.frame_id;
                patch.valid = true;
                for (int py = 0; py < 8; ++py) {
                    for (int px = 0; px < 8; ++px) {
                        patch.patch_intensities[static_cast<size_t>(py * 8 + px)] =
                            sampleBilinearGray(rgb_frame, proj[i].u + (px - 3), proj[i].v + (py - 3));
                    }
                }
                local_map_.updateVisualPatch(patch.center_world, patch);
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
            state_.timestamp_sec = bundle.lidar_frame.timestamp_end_sec;
            state_.status = core::TrackingStatus::TRACKING_LIO;
        }
    }

    // 1. Propagate 18x18 state + covariance & deskew sweep into sweep-end IMU BODY frame
    std::vector<ImuPropagatedPose> imu_traj;
    propagateImuAndDeskew(bundle, imu_traj, out_deskewed_body_frame);
    out_metrics.deskewed_points_count = out_deskewed_body_frame.points.size();
    const math::SE3d T_wb_prop_end = state_.T_world_body;

    // 2. Spatial voxel-grid downsampling of deskewed body points (uniform surface weighting across range)
    std::vector<math::Vec3d> feats_body;
    if (out_deskewed_body_frame.points.size() > 300 && config_.surf_downsample_voxel_m > 0.01) {
        const double inv_ds = 1.0 / config_.surf_downsample_voxel_m;
        std::unordered_map<VoxelKey, std::pair<math::Vec3d, int>, VoxelKeyHash> ds_map;
        ds_map.reserve(out_deskewed_body_frame.points.size() / 8);
        for (const auto& pt : out_deskewed_body_frame.points) {
            VoxelKey k{
                static_cast<int64_t>(std::floor(pt.x * inv_ds)),
                static_cast<int64_t>(std::floor(pt.y * inv_ds)),
                static_cast<int64_t>(std::floor(pt.z * inv_ds))
            };
            auto& acc = ds_map[k];
            acc.first += math::Vec3d(pt.x, pt.y, pt.z);
            acc.second += 1;
        }
        feats_body.reserve(ds_map.size());
        for (const auto& kv : ds_map) {
            feats_body.push_back(kv.second.first / static_cast<double>(kv.second.second));
        }
    } else {
        const int stride = std::max(1, config_.point_downsample_stride);
        feats_body.reserve(out_deskewed_body_frame.points.size() / static_cast<size_t>(stride) + 1);
        for (size_t i = 0; i < out_deskewed_body_frame.points.size(); i += static_cast<size_t>(stride)) {
            const auto& pt = out_deskewed_body_frame.points[i];
            feats_body.emplace_back(pt.x, pt.y, pt.z);
        }
    }

    // 3. Full 18-State Coupled Geometric ES-IEKF Update
    runGeometricIekfUpdate(feats_body, out_metrics);

    // 4. Optional Direct Visual Update (disabled by default, matching av-scanar img_en: 0)
    if (config_.enable_visual_fusion && bundle.rgb_frame.has_value()) {
        runDirectVisualUpdate(bundle.rgb_frame.value(), out_deskewed_body_frame, out_metrics);
    }

    // Apply IEKF correction rigidly to the sweep's IMU trajectory for sub-ms SLERP colorization
    const math::SE3d T_corr = state_.T_world_body * T_wb_prop_end.inverse();
    for (auto& step : imu_traj) {
        step.T_world_body = T_corr * step.T_world_body;
    }
    recent_pose_history_.emplace_back(state_.timestamp_sec, state_.T_world_body);
    while (recent_pose_history_.size() > 50) {
        recent_pose_history_.pop_front();
    }

    // 5. Transform deskewed BODY points directly into WORLD frame via T_world_body (consistent everywhere!)
    out_world_frame = out_deskewed_body_frame;
    for (auto& pt : out_world_frame.points) {
        const math::Vec3d pw = state_.T_world_body.transform(math::Vec3d(pt.x, pt.y, pt.z));
        pt.x = pw.x;
        pt.y = pw.y;
        pt.z = pw.z;
    }

    // 6. Colorize points with SLERP-interpolated camera pose + Z-buffer occlusion rejection
    if (bundle.rgb_frame.has_value()) {
        colorizeAndExtractVisualPatches(bundle.rgb_frame.value(), imu_traj, out_deskewed_body_frame, out_world_frame);
    } else {
        for (size_t i = 0; i < out_deskewed_body_frame.points.size(); ++i) {
            auto& pb = out_deskewed_body_frame.points[i];
            auto& pw = out_world_frame.points[i];
            if (!pb.colorized) {
                uint8_t shade = static_cast<uint8_t>(std::clamp(55.0f + pb.intensity * 0.55f, 45.0f, 195.0f));
                pb.r = shade; pb.g = shade; pb.b = shade;
                pw.r = shade; pw.g = shade; pw.b = shade;
            }
        }
    }

    // 7. Insert downsampled world points into local plane voxel map (fast & uniform!)
    std::vector<core::LidarPointXYZIRT> map_insert_pts;
    map_insert_pts.reserve(feats_body.size());
    for (const auto& pb : feats_body) {
        const math::Vec3d pw = state_.T_world_body.transform(pb);
        core::LidarPointXYZIRT p;
        p.x = pw.x;
        p.y = pw.y;
        p.z = pw.z;
        map_insert_pts.push_back(p);
    }
    local_map_.insertPoints(map_insert_pts, bundle.lidar_frame.frame_id);
    local_map_.pruneDistantOrStaleVoxels(state_.T_world_body.t, 45.0, bundle.lidar_frame.frame_id);

    // 8. Keyframe emission criteria (require >= 2 warmup sweeps so Keyframe 0 is settled & registered)
    if (!has_first_keyframe_) {
        if (trajectory_.size() >= 2) {
            last_keyframe_pose_ = state_.T_world_body;
            has_first_keyframe_ = true;
            out_metrics.keyframe_emitted = true;
        }
    } else {
        const math::SE3d dT = last_keyframe_pose_.inverse() * state_.T_world_body;
        const double trans_dist = dT.t.norm();
        const double rot_deg = dT.so3.log().norm() * math::kRadToDeg;
        if (trans_dist >= config_.keyframe_translation_thresh_m ||
            rot_deg >= config_.keyframe_rotation_thresh_deg) {
            last_keyframe_pose_ = state_.T_world_body;
            out_metrics.keyframe_emitted = true;
        }
    }

    state_.position_uncertainty_m = std::sqrt(std::max(0.0, P_(3, 3) + P_(4, 4) + P_(5, 5)));
    state_.orientation_uncertainty_deg = std::sqrt(std::max(0.0, P_(0, 0) + P_(1, 1) + P_(2, 2))) * math::kRadToDeg;
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
