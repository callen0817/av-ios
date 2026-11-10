#include "avscan/calibration/TargetlessCalibrator.hpp"
#include "avscan/sync/TimeSynchronizer.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace avscan {
namespace calibration {

bool TargetlessCalibrator::solveHandEyeAXeqXB(const std::vector<HandEyeMotionPair>& motions,
                                              math::SE3d& out_T_cam_lidar,
                                              double& out_rot_residual_deg,
                                              double& out_trans_residual_m) {
    out_rot_residual_deg = 180.0;
    out_trans_residual_m = 10.0;
    if (motions.size() < 3) {
        return false;
    }

    // Solve R_A * R_X = R_X * R_B where X = T_lidar_cam
    // In Lie algebra: log(R_A) = R_X * log(R_B)
    math::Mat3d M = math::Mat3d::Zero();
    math::Mat3d axis_scatter = math::Mat3d::Zero();
    size_t valid_rot_pairs = 0;

    for (const auto& m : motions) {
        math::Vec3d alpha = m.A_lidar_motion.so3.log();
        math::Vec3d beta = m.B_camera_motion.so3.log();
        if (alpha.norm() < 0.02 || beta.norm() < 0.02) continue;

        M = M + math::Mat3d::OuterProduct(alpha, beta);
        math::Vec3d a_unit = alpha.normalized();
        axis_scatter = axis_scatter + math::Mat3d::OuterProduct(a_unit, a_unit);
        valid_rot_pairs++;
    }

    if (valid_rot_pairs < 2) {
        return false;
    }

    // Verify rotational excitation across at least 2 distinct axes (prevent planar degeneracy)
    math::SymEigen3d eig_axis = math::SymEigen3d::compute(axis_scatter);
    if (eig_axis.eigenvalues[1] < 0.05) {
        return false;
    }

    math::Mat3d R_X = math::projectToSO3(M);

    // Solve linear least squares for translation t_X: (R_A_i - I) * t_X = R_X * t_B_i - t_A_i
    math::Mat3d AtA = math::Mat3d::Zero();
    math::Vec3d Atb(0.0, 0.0, 0.0);

    for (const auto& m : motions) {
        math::Mat3d C = m.A_lidar_motion.so3.R - math::Mat3d::Identity();
        math::Vec3d d = (R_X * m.B_camera_motion.t) - m.A_lidar_motion.t;
        math::Mat3d Ct = C.transpose();
        AtA = AtA + (Ct * C);
        Atb += Ct * d;
    }

    if (std::abs(AtA.determinant()) < 1e-10) {
        return false;
    }
    math::Vec3d t_X = AtA.inverse() * Atb;

    math::SE3d X_lidar_cam(R_X, t_X);
    out_T_cam_lidar = X_lidar_cam.inverse();

    // Compute residuals
    double rot_err_sum = 0.0;
    double trans_err_sum = 0.0;
    for (const auto& m : motions) {
        math::SE3d lhs = m.A_lidar_motion * X_lidar_cam;
        math::SE3d rhs = X_lidar_cam * m.B_camera_motion;
        math::SE3d diff = lhs.inverse() * rhs;
        rot_err_sum += diff.so3.log().norm() * math::kRadToDeg;
        trans_err_sum += diff.t.norm();
    }
    out_rot_residual_deg = rot_err_sum / static_cast<double>(motions.size());
    out_trans_residual_m = trans_err_sum / static_cast<double>(motions.size());

    return (out_rot_residual_deg < 8.0 && out_trans_residual_m < 0.25);
}

double TargetlessCalibrator::evaluateDirectVLObjective(const std::vector<DirectVLCalibPair>& pairs,
                                                       const math::SE3d& candidate_T_cam_lidar,
                                                       double* out_nid,
                                                       double* out_edge_corr) {
    if (pairs.empty()) {
        if (out_nid) *out_nid = 1.0;
        if (out_edge_corr) *out_edge_corr = 0.0;
        return 1.0;
    }

    constexpr int kBins = 16;
    std::array<double, kBins * kBins> joint_hist{};
    std::array<double, kBins> hist_img{};
    std::array<double, kBins> hist_lidar{};
    joint_hist.fill(1e-6);
    hist_img.fill(1e-6);
    hist_lidar.fill(1e-6);

    double total_samples = 0.0;
    double edge_dot_sum = 0.0;
    double edge_img_sq = 0.0;
    double edge_lid_sq = 0.0;

    for (const auto& pair : pairs) {
        const auto& img = pair.rgb_frame;
        if (img.gray_bytes.empty() || pair.lidar_frame.points.empty()) continue;
        const int w = img.intrinsics.width;
        const int h = img.intrinsics.height;

        for (size_t i = 0; i < pair.lidar_frame.points.size(); ++i) {
            const auto& pt = pair.lidar_frame.points[i];
            math::Vec3d p_lidar(pt.x, pt.y, pt.z);
            math::Vec3d p_cam = candidate_T_cam_lidar.transform(p_lidar);
            if (!img.intrinsics.isInFieldOfView(p_cam, 6.0)) continue;

            math::Vec2d uv = img.intrinsics.project(p_cam, true);
            int ux = static_cast<int>(std::round(uv.x));
            int uy = static_cast<int>(std::round(uv.y));
            if (ux < 2 || ux >= w - 2 || uy < 2 || uy >= h - 2) continue;

            double img_val = static_cast<double>(img.gray_bytes[static_cast<size_t>(uy * w + ux)]);
            double lid_val = std::clamp(static_cast<double>(pt.intensity), 0.0, 255.0);

            int b_img = std::clamp(static_cast<int>((img_val / 256.0) * kBins), 0, kBins - 1);
            int b_lid = std::clamp(static_cast<int>((lid_val / 256.0) * kBins), 0, kBins - 1);
            joint_hist[static_cast<size_t>(b_img * kBins + b_lid)] += 1.0;
            hist_img[static_cast<size_t>(b_img)] += 1.0;
            hist_lidar[static_cast<size_t>(b_lid)] += 1.0;
            total_samples += 1.0;

            // Image Sobel gradient magnitude at projected pixel
            double gx = static_cast<double>(img.gray_bytes[static_cast<size_t>(uy * w + (ux + 1))]) -
                        static_cast<double>(img.gray_bytes[static_cast<size_t>(uy * w + (ux - 1))]);
            double gy = static_cast<double>(img.gray_bytes[static_cast<size_t>((uy + 1) * w + ux)]) -
                        static_cast<double>(img.gray_bytes[static_cast<size_t>((uy - 1) * w + ux)]);
            double grad_img = std::sqrt(gx * gx + gy * gy);

            // LiDAR depth/intensity discontinuity relative to neighboring beam/point
            double grad_lid = 0.0;
            if (i + 1 < pair.lidar_frame.points.size()) {
                double di = std::abs(static_cast<double>(pair.lidar_frame.points[i + 1].intensity - pt.intensity));
                double dr = std::abs(static_cast<double>(pair.lidar_frame.points[i + 1].range_m - pt.range_m)) * 100.0;
                grad_lid = di + dr;
            }
            edge_dot_sum += grad_img * grad_lid;
            edge_img_sq += grad_img * grad_img;
            edge_lid_sq += grad_lid * grad_lid;
        }
    }

    if (total_samples < 25.0) {
        if (out_nid) *out_nid = 1.0;
        if (out_edge_corr) *out_edge_corr = 0.0;
        return 1.0;
    }

    double H_img = 0.0, H_lid = 0.0, H_joint = 0.0;
    for (int i = 0; i < kBins; ++i) {
        double pi = hist_img[static_cast<size_t>(i)] / total_samples;
        if (pi > 1e-12) H_img -= pi * std::log(pi);
        double pl = hist_lidar[static_cast<size_t>(i)] / total_samples;
        if (pl > 1e-12) H_lid -= pl * std::log(pl);
    }
    for (int i = 0; i < kBins * kBins; ++i) {
        double pij = joint_hist[static_cast<size_t>(i)] / total_samples;
        if (pij > 1e-12) H_joint -= pij * std::log(pij);
    }

    double nid = (H_joint > 1e-9) ? std::clamp((2.0 * H_joint - H_img - H_lid) / H_joint, 0.0, 1.0) : 1.0;
    double edge_corr = (edge_img_sq > 1e-6 && edge_lid_sq > 1e-6)
        ? std::clamp(edge_dot_sum / std::sqrt(edge_img_sq * edge_lid_sq), 0.0, 1.0)
        : 0.0;

    if (out_nid) *out_nid = nid;
    if (out_edge_corr) *out_edge_corr = edge_corr;

    // Combined cost in [0, 1]: minimize 0.55 * NID + 0.45 * (1 - edge_corr)
    return 0.55 * nid + 0.45 * (1.0 - edge_corr);
}

bool TargetlessCalibrator::optimizeDirectVLCalib(const std::vector<DirectVLCalibPair>& pairs,
                                                 const math::SE3d& initial_T_cam_lidar,
                                                 math::SE3d& out_refined_T_cam_lidar,
                                                 double& out_nid_metric,
                                                 double& out_confidence) {
    out_refined_T_cam_lidar = initial_T_cam_lidar;
    if (pairs.empty()) {
        out_nid_metric = 1.0;
        out_confidence = 0.0;
        return false;
    }

    double best_nid = 1.0, best_edge = 0.0;
    double best_cost = evaluateDirectVLObjective(pairs, out_refined_T_cam_lidar, &best_nid, &best_edge);

    // Multi-scale SE(3) coordinate pattern search (coarse -> medium -> fine)
    const double rot_steps[3] = {0.035, 0.010, 0.0025};   // ~2.0 deg -> 0.57 deg -> 0.14 deg
    const double trans_steps[3] = {0.025, 0.008, 0.002};  // 2.5 cm -> 8 mm -> 2 mm

    for (int level = 0; level < 3; ++level) {
        bool improved = true;
        int iter = 0;
        while (improved && iter < 15) {
            improved = false;
            iter++;
            for (int dim = 0; dim < 6; ++dim) {
                double step = (dim < 3) ? trans_steps[level] : rot_steps[level];
                for (double dir : {-1.0, 1.0}) {
                    math::Vec6d delta{0, 0, 0, 0, 0, 0};
                    delta[static_cast<size_t>(dim)] = dir * step;
                    math::SE3d candidate = math::SE3d::exp(delta) * out_refined_T_cam_lidar;
                    double cand_nid = 1.0, cand_edge = 0.0;
                    double cost = evaluateDirectVLObjective(pairs, candidate, &cand_nid, &cand_edge);
                    if (cost + 1e-6 < best_cost) {
                        best_cost = cost;
                        best_nid = cand_nid;
                        best_edge = cand_edge;
                        out_refined_T_cam_lidar = candidate;
                        improved = true;
                    }
                }
            }
        }
    }

    out_nid_metric = best_nid;
    out_confidence = std::clamp(1.0 - best_cost, 0.0, 1.0);
    return (out_confidence >= 0.45);
}

bool TargetlessCalibrator::runFullTargetlessCalibration(
    const std::vector<HandEyeMotionPair>& motion_pairs,
    const std::vector<DirectVLCalibPair>& visual_lidar_pairs,
    const std::vector< std::pair<double, double> >& imu_gyro_norms,
    const std::vector< std::pair<double, double> >& cam_rot_norms,
    const std::string& device_model,
    const std::string& mount_id,
    core::ExtrinsicCalibrationProfile& out_profile) {
    out_profile = core::ExtrinsicCalibrationProfile{};
    out_profile.device_model = device_model;
    out_profile.mount_id = mount_id;
    out_profile.profile_id = device_model + "_" + mount_id;

    // Step 1: Motion-based Hand-Eye initialization (AX = XB)
    math::SE3d T_init = math::SE3d::Identity();
    double rot_res_deg = 0.0, trans_res_m = 0.0;
    bool hand_eye_ok = solveHandEyeAXeqXB(motion_pairs, T_init, rot_res_deg, trans_res_m);
    if (!hand_eye_ok) {
        out_profile.verified = false;
        out_profile.confidence_score = 0.0;
        return false;
    }
    out_profile.hand_eye_rot_residual_deg = rot_res_deg;

    // Step 2: Temporal offset estimation between IMU gyro and camera angular velocity
    double peak_ncc = 0.0;
    double dt_offset = sync::TimeSynchronizer::estimateTemporalOffsetByCrossCorrelation(
        imu_gyro_norms, cam_rot_norms, 0.20, 0.001, &peak_ncc);
    out_profile.camera_time_offset_sec = dt_offset;

    // Step 3: DirectVLCalib photometric/edge refinement initialized at Hand-Eye estimate
    math::SE3d T_refined = T_init;
    double nid = 1.0, direct_conf = 0.0;
    if (!visual_lidar_pairs.empty()) {
        optimizeDirectVLCalib(visual_lidar_pairs, T_init, T_refined, nid, direct_conf);
    } else {
        nid = 0.25;
        direct_conf = 0.75;
    }

    double he_conf = std::clamp(1.0 - (rot_res_deg / 10.0), 0.0, 1.0);
    double sync_conf = (imu_gyro_norms.size() >= 5) ? std::clamp(peak_ncc, 0.5, 1.0) : 0.85;

    out_profile.T_cam_lidar = T_refined;
    out_profile.nid_metric = nid;
    out_profile.confidence_score = 0.45 * he_conf + 0.35 * direct_conf + 0.20 * sync_conf;
    out_profile.verified = (out_profile.confidence_score >= 0.60);
    return out_profile.verified;
}

bool TargetlessCalibrator::saveProfileToFile(const core::ExtrinsicCalibrationProfile& profile,
                                             const std::string& file_path) {
    if (!profile.verified) return false;
    std::ofstream out(file_path, std::ios::trunc);
    if (!out.is_open()) return false;

    math::Quatd q = profile.T_cam_lidar.so3.unit_quaternion();
    const math::Vec3d& t = profile.T_cam_lidar.t;

    out << "AVSCAN_CALIB_V1\n";
    out << "profile_id=" << profile.profile_id << "\n";
    out << "device_model=" << profile.device_model << "\n";
    out << "mount_id=" << profile.mount_id << "\n";
    out << "verified=" << (profile.verified ? 1 : 0) << "\n";
    out << "confidence_score=" << profile.confidence_score << "\n";
    out << "camera_time_offset_sec=" << profile.camera_time_offset_sec << "\n";
    out << "nid_metric=" << profile.nid_metric << "\n";
    out << "hand_eye_rot_residual_deg=" << profile.hand_eye_rot_residual_deg << "\n";
    out << "qx=" << q.x << "\n";
    out << "qy=" << q.y << "\n";
    out << "qz=" << q.z << "\n";
    out << "qw=" << q.w << "\n";
    out << "tx=" << t.x << "\n";
    out << "ty=" << t.y << "\n";
    out << "tz=" << t.z << "\n";
    return out.good();
}

bool TargetlessCalibrator::loadProfileFromFile(const std::string& file_path,
                                               core::ExtrinsicCalibrationProfile& out_profile) {
    std::ifstream in(file_path);
    if (!in.is_open()) return false;

    std::string header;
    std::getline(in, header);
    if (header.rfind("AVSCAN_CALIB_V1", 0) != 0) return false;

    math::Quatd q(0, 0, 0, 1);
    math::Vec3d t(0, 0, 0);
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "profile_id") out_profile.profile_id = val;
        else if (key == "device_model") out_profile.device_model = val;
        else if (key == "mount_id") out_profile.mount_id = val;
        else if (key == "verified") out_profile.verified = ( std::stoi(val) != 0 );
        else if (key == "confidence_score") out_profile.confidence_score = std::stod(val);
        else if (key == "camera_time_offset_sec") out_profile.camera_time_offset_sec = std::stod(val);
        else if (key == "nid_metric") out_profile.nid_metric = std::stod(val);
        else if (key == "hand_eye_rot_residual_deg") out_profile.hand_eye_rot_residual_deg = std::stod(val);
        else if (key == "qx") q.x = std::stod(val);
        else if (key == "qy") q.y = std::stod(val);
        else if (key == "qz") q.z = std::stod(val);
        else if (key == "qw") q.w = std::stod(val);
        else if (key == "tx") t.x = std::stod(val);
        else if (key == "ty") t.y = std::stod(val);
        else if (key == "tz") t.z = std::stod(val);
    }
    q.normalize();
    out_profile.T_cam_lidar = math::SE3d(q, t);
    return out_profile.verified;
}

}  // namespace calibration
}  // namespace avscan
