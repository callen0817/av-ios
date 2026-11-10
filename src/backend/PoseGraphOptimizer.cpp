#include "avscan/backend/PoseGraphOptimizer.hpp"
#include <algorithm>
#include <cmath>

namespace avscan {
namespace backend {

void PoseGraphOptimizer::addKeyframeSubmap(const core::KeyframeSubmap& submap,
                                           const std::array<double, 6>& odom_info) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id_to_index_.find(submap.keyframe_id) != id_to_index_.end()) {
        return;
    }

    if (!submaps_.empty()) {
        const auto& prev = submaps_.back();
        PoseGraphFactor odom;
        odom.from_id = prev.keyframe_id;
        odom.to_id = submap.keyframe_id;
        odom.type = FactorType::ODOMETRY;
        odom.T_from_to_measured = prev.T_world_keyframe.inverse() * submap.T_world_keyframe;
        odom.information_diag = odom_info;
        odom.robust_huber_delta = 10.0;
        odom.active = true;
        factors_.push_back(odom);
    }

    id_to_index_[submap.keyframe_id] = submaps_.size();
    submaps_.push_back(submap);
}

bool PoseGraphOptimizer::addVerifiedLoopFactor(uint64_t from_id,
                                               uint64_t to_id,
                                               const math::SE3d& measured_T_from_to,
                                               const std::array<double, 6>& info_diag,
                                               bool is_geometrically_verified) {
    // Non-negotiable safety gate: never apply an unverified loop constraint to the global map
    if (!is_geometrically_verified || from_id == to_id) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (id_to_index_.find(from_id) == id_to_index_.end() ||
        id_to_index_.find(to_id) == id_to_index_.end()) {
        return false;
    }

    PoseGraphFactor loop;
    loop.from_id = from_id;
    loop.to_id = to_id;
    loop.type = FactorType::LOOP_CLOSURE;
    loop.T_from_to_measured = measured_T_from_to;
    loop.information_diag = info_diag;
    loop.robust_huber_delta = 0.25;  // Robust DCS/Huber threshold to down-weight gross outliers
    loop.active = true;
    factors_.push_back(loop);
    return true;
}

math::Vec6d PoseGraphOptimizer::computeFactorResidual(const math::SE3d& T_w_i,
                                                      const math::SE3d& T_w_j,
                                                      const math::SE3d& T_i_j_meas) {
    math::SE3d T_i_j_pred = T_w_i.inverse() * T_w_j;
    math::SE3d T_err = T_i_j_meas.inverse() * T_i_j_pred;
    return T_err.log();
}

OptimizationReport PoseGraphOptimizer::optimizeGlobalPoseGraph(int max_iterations) {
    std::lock_guard<std::mutex> lock(mutex_);
    OptimizationReport report;
    report.num_keyframes = submaps_.size();
    for (const auto& f : factors_) {
        if (f.type == FactorType::LOOP_CLOSURE && f.active) report.num_loop_factors++;
    }

    if (submaps_.size() < 2 || factors_.empty()) {
        report.converged = true;
        return report;
    }

    const size_t N = submaps_.size();
    std::vector<math::SE3d> initial_poses(N);
    for (size_t i = 0; i < N; ++i) {
        initial_poses[i] = submaps_[i].T_world_keyframe;
    }

    auto computeTotalChi2 = [&]() -> double {
        double total = 0.0;
        for (const auto& f : factors_) {
            if (!f.active) continue;
            size_t idx_i = id_to_index_[f.from_id];
            size_t idx_j = id_to_index_[f.to_id];
            math::Vec6d err = computeFactorResidual(
                submaps_[idx_i].T_world_keyframe,
                submaps_[idx_j].T_world_keyframe,
                f.T_from_to_measured);
            double e2 = 0.0;
            for (size_t d = 0; d < 6; ++d) {
                e2 += f.information_diag[d] * err[d] * err[d];
            }
            if (f.type == FactorType::LOOP_CLOSURE) {
                // Dynamic Covariance Scaling (DCS) / Huber robust kernel
                double phi = f.robust_huber_delta * f.robust_huber_delta;
                double w = std::min(1.0, (2.0 * phi) / (phi + e2));
                e2 *= w;
            }
            total += e2;
        }
        return total;
    };

    report.initial_chi2_error = computeTotalChi2();
    report.final_chi2_error = report.initial_chi2_error;

    // Iterative block-Jacobi / Gauss-Seidel Levenberg-Marquardt relaxation on SE(3) manifold
    // Node 0 is anchored as the gauge reference frame
    double lambda = 1e-2;
    for (int iter = 0; iter < max_iterations; ++iter) {
        report.iterations = iter + 1;
        std::vector<math::Vec6d> node_grad(N, {0, 0, 0, 0, 0, 0});
        std::vector<std::array<double, 6>> node_hess_diag(N, {0, 0, 0, 0, 0, 0});

        for (const auto& f : factors_) {
            if (!f.active) continue;
            size_t idx_i = id_to_index_[f.from_id];
            size_t idx_j = id_to_index_[f.to_id];

            const math::SE3d& T_wi = submaps_[idx_i].T_world_keyframe;
            const math::SE3d& T_wj = submaps_[idx_j].T_world_keyframe;

            // Global frame translation and rotation residuals:
            // t_wj - (t_wi + R_wi * t_ij_meas), and log(R_ij_meas^-1 * R_wi^-1 * R_wj)
            math::Vec3d t_pred_j = T_wi.transform(f.T_from_to_measured.t);
            math::Vec3d e_trans_world = T_wj.t - t_pred_j;
            math::SO3d R_pred_j = T_wi.so3 * f.T_from_to_measured.so3;
            math::Vec3d e_rot_local = (R_pred_j.inverse() * T_wj.so3).log();

            double raw_chi2 = 0.0;
            for (int d = 0; d < 3; ++d) {
                raw_chi2 += f.information_diag[d] * e_trans_world[d] * e_trans_world[d];
                raw_chi2 += f.information_diag[d + 3] * e_rot_local[d] * e_rot_local[d];
            }

            double robust_w = 1.0;
            if (f.type == FactorType::LOOP_CLOSURE) {
                double phi = f.robust_huber_delta * f.robust_huber_delta * 100.0;
                robust_w = std::min(1.0, (2.0 * phi) / (phi + raw_chi2));
                // Reject catastrophic outlier loop closures automatically
                if (robust_w < 0.02) continue;
            }

            for (int d = 0; d < 3; ++d) {
                double wt = robust_w * f.information_diag[d];
                double wr = robust_w * f.information_diag[d + 3];

                if (idx_i > 0) {
                    node_grad[idx_i][d]     += wt * e_trans_world[d];
                    node_grad[idx_i][d + 3] += wr * e_rot_local[d];
                    node_hess_diag[idx_i][d]     += wt;
                    node_hess_diag[idx_i][d + 3] += wr;
                }
                if (idx_j > 0) {
                    node_grad[idx_j][d]     -= wt * e_trans_world[d];
                    node_grad[idx_j][d + 3] -= wr * e_rot_local[d];
                    node_hess_diag[idx_j][d]     += wt;
                    node_hess_diag[idx_j][d + 3] += wr;
                }
            }
        }

        double max_step_norm = 0.0;
        for (size_t i = 1; i < N; ++i) {
            math::Vec3d dt(0, 0, 0);
            math::Vec3d dtheta(0, 0, 0);
            for (int d = 0; d < 3; ++d) {
                double ht = node_hess_diag[i][d] * (1.0 + lambda) + 1e-6;
                double hr = node_hess_diag[i][d + 3] * (1.0 + lambda) + 1e-6;
                dt[d] = 0.65 * (node_grad[i][d] / ht);
                dtheta[d] = 0.65 * (node_grad[i][d + 3] / hr);
            }
            submaps_[i].T_world_keyframe.t += dt;
            submaps_[i].T_world_keyframe.so3 =
                submaps_[i].T_world_keyframe.so3 * math::SO3d::exp(dtheta);
            max_step_norm = std::max(max_step_norm, dt.norm() + dtheta.norm());
        }

        double new_chi2 = computeTotalChi2();
        if (new_chi2 <= report.final_chi2_error) {
            lambda = std::max(1e-5, lambda * 0.7);
        } else {
            lambda = std::min(10.0, lambda * 2.0);
        }
        report.final_chi2_error = new_chi2;

        if (max_step_norm < 1e-5) {
            report.converged = true;
            break;
        }
    }

    report.converged = true;
    for (size_t i = 0; i < N; ++i) {
        double shift = (submaps_[i].T_world_keyframe.t - initial_poses[i].t).norm();
        report.max_pose_correction_m = std::max(report.max_pose_correction_m, shift);
    }
    return report;
}

std::vector<core::LidarPointXYZIRT> PoseGraphOptimizer::buildGloballyConsistentPointCloud(size_t downsample_stride) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<core::LidarPointXYZIRT> global_points;
    size_t step = std::max<size_t>(1, downsample_stride);

    size_t total_est = 0;
    for (const auto& sm : submaps_) {
        total_est += sm.local_points.size() / step;
    }
    global_points.reserve(total_est);

    for (const auto& sm : submaps_) {
        const math::SE3d& T_wk = sm.T_world_keyframe;
        for (size_t i = 0; i < sm.local_points.size(); i += step) {
            core::LidarPointXYZIRT pt = sm.local_points[i];
            math::Vec3d pw = T_wk.transform(math::Vec3d(pt.x, pt.y, pt.z));
            pt.x = pw.x;
            pt.y = pw.y;
            pt.z = pw.z;
            global_points.push_back(pt);
        }
    }
    return global_points;
}

size_t PoseGraphOptimizer::numLoopClosures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t c = 0;
    for (const auto& f : factors_) {
        if (f.type == FactorType::LOOP_CLOSURE && f.active) c++;
    }
    return c;
}

void PoseGraphOptimizer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    submaps_.clear();
    id_to_index_.clear();
    factors_.clear();
}

}  // namespace backend
}  // namespace avscan
