#include "avscan/backend/GicpVerifier.hpp"
#include "avscan/slam/VoxelHashMap.hpp"
#include <algorithm>
#include <cmath>

namespace avscan {
namespace backend {

namespace {

bool solve6x6Gicp(double A[6][6], const double b[6], double x[6]) {
    double M[6][7]{};
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) M[i][j] = A[i][j];
        M[i][6] = b[i];
    }
    for (int k = 0; k < 6; ++k) {
        int pivot = k;
        double max_v = std::abs(M[k][k]);
        for (int i = k + 1; i < 6; ++i) {
            if (std::abs(M[i][k]) > max_v) {
                max_v = std::abs(M[i][k]);
                pivot = i;
            }
        }
        if (max_v < 1e-12) return false;
        if (pivot != k) {
            for (int j = k; j <= 6; ++j) std::swap(M[k][j], M[pivot][j]);
        }
        for (int i = k + 1; i < 6; ++i) {
            double f = M[i][k] / M[k][k];
            for (int j = k; j <= 6; ++j) M[i][j] -= f * M[k][j];
        }
    }
    for (int i = 5; i >= 0; --i) {
        double s = M[i][6];
        for (int j = i + 1; j < 6; ++j) s -= M[i][j] * x[j];
        x[i] = s / M[i][i];
    }
    return true;
}

}  // namespace

GicpVerifier::GicpVerifier(const GicpVerificationConfig& config) : config_(config) {}

GicpVerificationResult GicpVerifier::verifyLoopConstraint(
    const std::vector<core::LidarPointXYZIRT>& source_local_points,
    const std::vector<core::LidarPointXYZIRT>& target_local_points,
    const math::SE3d& initial_guess_T_target_source) const {
    GicpVerificationResult res;
    res.T_target_source = initial_guess_T_target_source;

    if (source_local_points.size() < 20 || target_local_points.size() < 20) {
        return res;
    }

    slam::VoxelHashMap target_map(config_.voxel_resolution_m, 30, 20000);
    target_map.insertPoints(target_local_points, 1);

    // Also store target voxel centroids for point-to-point fallback when planes are sparse
    const auto& target_voxels = target_map.getVoxels();
    if (target_voxels.empty()) {
        return res;
    }

    for (int iter = 0; iter < config_.max_iterations; ++iter) {
        res.iterations_run = iter + 1;
        double HTH[6][6]{};
        double HTr[6]{};
        size_t inliers = 0;
        double sum_sq_err = 0.0;

        for (const auto& pt : source_local_points) {
            math::Vec3d ps(pt.x, pt.y, pt.z);
            math::Vec3d pt_in_target = res.T_target_source.transform(ps);

            slam::PlaneCorrespondence corr;
            if (target_map.queryPlaneCorrespondence(ps, pt_in_target, config_.max_correspondence_dist_m, corr)) {
                math::Vec3d R_n = res.T_target_source.so3.R.transpose() * corr.plane_normal;
                math::Vec3d J_rot = ps.cross(R_n);
                const math::Vec3d& J_pos = corr.plane_normal;

                double J[6] = {J_pos.x, J_pos.y, J_pos.z, J_rot.x, J_rot.y, J_rot.z};
                double w = corr.huber_weight;
                double r = corr.signed_distance;

                for (int r_i = 0; r_i < 6; ++r_i) {
                    HTr[r_i] -= w * J[r_i] * r;
                    for (int c_i = 0; c_i < 6; ++c_i) {
                        HTH[r_i][c_i] += w * J[r_i] * J[c_i];
                    }
                }
                sum_sq_err += r * r;
                inliers++;
            } else {
                // Check voxel centroid correspondence within neighborhood
                slam::VoxelKey vk = target_map.pointToKey(pt_in_target);
                auto it = target_voxels.find(vk);
                if (it != target_voxels.end() && !it->second.points.empty()) {
                    math::Vec3d diff = pt_in_target - it->second.mean;
                    double d = diff.norm();
                    if (d < config_.max_correspondence_dist_m) {
                        math::Vec3d unit_dir = (d > 1e-6) ? (diff / d) : math::Vec3d(1, 0, 0);
                        math::Vec3d R_u = res.T_target_source.so3.R.transpose() * unit_dir;
                        math::Vec3d J_rot = ps.cross(R_u);
                        double J[6] = {unit_dir.x, unit_dir.y, unit_dir.z, J_rot.x, J_rot.y, J_rot.z};
                        double w = (d <= 0.10) ? 1.0 : (0.10 / d);
                        for (int r_i = 0; r_i < 6; ++r_i) {
                            HTr[r_i] -= w * J[r_i] * d;
                            for (int c_i = 0; c_i < 6; ++c_i) {
                                HTH[r_i][c_i] += w * J[r_i] * J[c_i];
                            }
                        }
                        sum_sq_err += d * d;
                        inliers++;
                    }
                }
            }
        }

        res.inlier_count = inliers;
        res.inlier_overlap_ratio = static_cast<double>(inliers) / static_cast<double>(source_local_points.size());
        if (inliers < 10) break;

        res.fitness_rmse_m = std::sqrt(sum_sq_err / static_cast<double>(inliers));

        // Levenberg-Marquardt damping
        for (int i = 0; i < 6; ++i) HTH[i][i] += 1e-3;

        double dx[6]{};
        if (!solve6x6Gicp(HTH, HTr, dx)) break;

        math::Vec3d dt(dx[0], dx[1], dx[2]);
        math::Vec3d dtheta(dx[3], dx[4], dx[5]);
        res.T_target_source.t += dt;
        res.T_target_source.so3 = res.T_target_source.so3 * math::SO3d::exp(dtheta);

        if (dt.norm() < 1e-4 && dtheta.norm() < 1e-4) {
            break;
        }
    }

    res.verified = (res.inlier_overlap_ratio >= config_.min_inlier_overlap_ratio &&
                    res.fitness_rmse_m <= config_.max_fitness_rmse_m &&
                    res.inlier_count >= 12);

    if (res.verified) {
        double info_scale = 1.0 / std::max(1e-4, res.fitness_rmse_m * res.fitness_rmse_m);
        for (int i = 0; i < 3; ++i) {
            res.information_diagonal[i] = std::clamp(info_scale, 10.0, 2000.0);
            res.information_diagonal[i + 3] = std::clamp(info_scale * 4.0, 40.0, 8000.0);
        }
    }
    return res;
}

}  // namespace backend
}  // namespace avscan
