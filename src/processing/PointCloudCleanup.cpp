#include "avscan/processing/PointCloudCleanup.hpp"
#include "avscan/slam/VoxelHashMap.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace avscan {
namespace processing {

namespace {

struct Pt3f {
    float x, y, z;
};

inline bool isFinitePoint(const core::LidarPointXYZIRT& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

inline slam::VoxelKey makeKey(double x, double y, double z, double inv_cell) {
    return {
        static_cast<int64_t>(std::floor(x * inv_cell)),
        static_cast<int64_t>(std::floor(y * inv_cell)),
        static_cast<int64_t>(std::floor(z * inv_cell))
    };
}

inline uint8_t decodeRgbQuality6(const core::LidarPointXYZIRT& pt) {
    return static_cast<uint8_t>((pt.status_flags >> 2) & 0x3Fu);
}

inline double decodeRgbWeight(const core::LidarPointXYZIRT& pt) {
    if (!pt.colorized) return 0.0;
    const uint8_t q6 = decodeRgbQuality6(pt);
    const double q_norm = (q6 > 0) ? (static_cast<double>(q6) / 63.0) : 0.5;
    return std::max(0.04, q_norm * q_norm);
}

}  // namespace

std::vector<core::LidarPointXYZIRT> PointCloudCleanup::filterDynamicTransients(
    const std::vector<core::KeyframeSubmap>& submaps,
    double voxel_m,
    uint32_t min_pass_through,
    size_t* out_removed_count) {
    if (out_removed_count) *out_removed_count = 0;
    if (submaps.empty()) return {};

    // Fast path: with fewer than 4 keyframes we cannot reliably distinguish dynamic walkers from static geometry
    if (submaps.size() < 4) {
        size_t total = 0;
        for (const auto& sm : submaps) total += sm.local_points.size();
        std::vector<core::LidarPointXYZIRT> out;
        out.reserve(total);
        for (const auto& sm : submaps) {
            const auto& T = sm.T_world_keyframe;
            for (const auto& pt : sm.local_points) {
                if (!isFinitePoint(pt)) continue;
                math::Vec3d pw = T.transform(math::Vec3d(pt.x, pt.y, pt.z));
                core::LidarPointXYZIRT w = pt;
                w.x = pw.x; w.y = pw.y; w.z = pw.z;
                out.push_back(w);
            }
        }
        return out;
    }

    const double v = std::max(0.08, voxel_m);
    const double inv_v = 1.0 / v;

    struct RayCellStats {
        uint32_t hit_keyframes{0};
        uint32_t pass_through_rays{0};
        uint64_t last_kf_id{std::numeric_limits<uint64_t>::max()};
    };
    std::unordered_map<slam::VoxelKey, RayCellStats, slam::VoxelKeyHash> grid;
    grid.reserve(120000);

    // Pass 1: Record unique keyframe hits per coarse voxel
    for (const auto& sm : submaps) {
        const auto& T = sm.T_world_keyframe;
        for (const auto& pt : sm.local_points) {
            if (!isFinitePoint(pt)) continue;
            const math::Vec3d pw = T.transform(math::Vec3d(pt.x, pt.y, pt.z));
            const slam::VoxelKey hit_k = makeKey(pw.x, pw.y, pw.z, inv_v);
            auto& st = grid[hit_k];
            if (st.last_kf_id != sm.keyframe_id) {
                st.hit_keyframes++;
                st.last_kf_id = sm.keyframe_id;
            }
        }
    }

    // Extract compact candidate map containing ONLY single-keyframe voxels (shrinks Pass 2 table by 4x-5x!)
    struct CandidateStats {
        uint64_t only_kf_id{0};
        uint32_t pass_through_rays{0};
    };
    std::unordered_map<slam::VoxelKey, CandidateStats, slam::VoxelKeyHash> candidates;
    candidates.reserve(grid.size() / 3 + 16);
    for (const auto& kv : grid) {
        if (kv.second.hit_keyframes <= 1) {
            candidates.emplace(kv.first, CandidateStats{kv.second.last_kf_id, 0u});
        }
    }
    grid.clear();

    // Pass 2: Uniform interior ray-marching from every keyframe into single-keyframe candidates only.
    if (!candidates.empty()) {
        for (const auto& sm : submaps) {
            const auto& T = sm.T_world_keyframe;
            const math::Vec3d origin = T.t;
            const size_t n = sm.local_points.size();
            for (size_t i = 0; i < n; i += 4) {
                const auto& pt = sm.local_points[i];
                if (!isFinitePoint(pt)) continue;
                const math::Vec3d pw = T.transform(math::Vec3d(pt.x, pt.y, pt.z));
                const math::Vec3d ray = pw - origin;
                const double len = ray.norm();
                if (len <= v * 3.0) continue;

                const slam::VoxelKey hit_k = makeKey(pw.x, pw.y, pw.z, inv_v);
                const math::Vec3d dir = ray * (1.0 / len);
                const double d_start = v * 1.5;
                const double d_end = len - v * 1.5;
                const double span = d_end - d_start;
                if (span <= 0.0) continue;

                // Step at <= 0.9*v (capped at 20 steps per ray) so no intermediate voxel along the ray is skipped
                const double step = std::max(v * 0.9, span / 20.0);
                slam::VoxelKey prev_sk = hit_k;
                for (double d = d_start; d <= d_end; d += step) {
                    const math::Vec3d sample = origin + dir * d;
                    const slam::VoxelKey sk = makeKey(sample.x, sample.y, sample.z, inv_v);
                    if (sk == hit_k || sk == prev_sk) continue;
                    prev_sk = sk;
                    auto it_s = candidates.find(sk);
                    if (it_s != candidates.end() && it_s->second.only_kf_id != sm.keyframe_id) {
                        it_s->second.pass_through_rays++;
                    }
                }
            }
        }
    }

    size_t total_est = 0;
    for (const auto& sm : submaps) total_est += sm.local_points.size();
    std::vector<core::LidarPointXYZIRT> out;
    out.reserve(total_est);

    size_t removed = 0;
    const uint32_t pt_thresh = std::max<uint32_t>(2, min_pass_through);
    for (const auto& sm : submaps) {
        const auto& T = sm.T_world_keyframe;
        for (const auto& pt : sm.local_points) {
            if (!isFinitePoint(pt)) continue;
            const math::Vec3d pw = T.transform(math::Vec3d(pt.x, pt.y, pt.z));
            if (!candidates.empty()) {
                const slam::VoxelKey k = makeKey(pw.x, pw.y, pw.z, inv_v);
                auto it = candidates.find(k);
                // Only remove points strictly proven transient: hit in <= 1 keyframe AND >= pt_thresh rays
                // passed cleanly through that voxel from other viewpoints (never deletes static edge rooms!).
                if (it != candidates.end() && it->second.pass_through_rays >= pt_thresh) {
                    removed++;
                    continue;
                }
            }
            core::LidarPointXYZIRT w = pt;
            w.x = pw.x; w.y = pw.y; w.z = pw.z;
            out.push_back(w);
        }
    }
    if (out_removed_count) *out_removed_count = removed;
    return out;
}

std::vector<core::LidarPointXYZIRT> PointCloudCleanup::fuseVoxelCentroidsWeightedRgb(
    const std::vector<core::LidarPointXYZIRT>& points,
    double voxel_size_m) {
    if (points.empty()) return {};
    if (voxel_size_m <= 1e-4) return points;

    const double inv_v = 1.0 / voxel_size_m;

    struct VoxelAccum {
        double sum_x{0.0}, sum_y{0.0}, sum_z{0.0};
        double sum_intensity{0.0};
        uint32_t count{0};
        double sum_wr{0.0}, sum_wg{0.0}, sum_wb{0.0}, sum_w{0.0};
        uint8_t max_q6{0};
        uint8_t low_status{0};
        uint16_t ring{0};
        double timestamp_sec{0.0};
        float range_m{0.0f};
    };

    std::unordered_map<slam::VoxelKey, VoxelAccum, slam::VoxelKeyHash> accum;
    accum.reserve(std::min<size_t>(points.size(), 1500000));

    for (const auto& pt : points) {
        if (!isFinitePoint(pt)) continue;
        const slam::VoxelKey vk = makeKey(pt.x, pt.y, pt.z, inv_v);
        auto& a = accum[vk];
        if (a.count == 0) {
            a.ring = pt.ring;
            a.timestamp_sec = pt.timestamp_sec;
            a.range_m = pt.range_m;
            a.low_status = static_cast<uint8_t>(pt.status_flags & 0x03u);
        }
        a.sum_x += pt.x;
        a.sum_y += pt.y;
        a.sum_z += pt.z;
        a.sum_intensity += static_cast<double>(pt.intensity);
        a.count++;

        if (pt.colorized) {
            const double w = decodeRgbWeight(pt);
            a.sum_wr += w * static_cast<double>(pt.r);
            a.sum_wg += w * static_cast<double>(pt.g);
            a.sum_wb += w * static_cast<double>(pt.b);
            a.sum_w += w;
            a.max_q6 = std::max(a.max_q6, decodeRgbQuality6(pt));
        }
    }

    std::vector<core::LidarPointXYZIRT> out;
    out.reserve(accum.size());
    for (const auto& kv : accum) {
        const auto& a = kv.second;
        if (a.count == 0) continue;
        const double inv_c = 1.0 / static_cast<double>(a.count);
        core::LidarPointXYZIRT pt;
        pt.x = a.sum_x * inv_c;
        pt.y = a.sum_y * inv_c;
        pt.z = a.sum_z * inv_c;
        pt.intensity = static_cast<float>(a.sum_intensity * inv_c);
        pt.ring = a.ring;
        pt.timestamp_sec = a.timestamp_sec;
        pt.range_m = a.range_m;
        pt.status_flags = static_cast<uint8_t>(a.low_status | (a.max_q6 << 2));
        if (a.sum_w > 0.0) {
            pt.r = static_cast<uint8_t>(std::clamp(std::round(a.sum_wr / a.sum_w), 0.0, 255.0));
            pt.g = static_cast<uint8_t>(std::clamp(std::round(a.sum_wg / a.sum_w), 0.0, 255.0));
            pt.b = static_cast<uint8_t>(std::clamp(std::round(a.sum_wb / a.sum_w), 0.0, 255.0));
            pt.colorized = true;
        } else {
            uint8_t shade = static_cast<uint8_t>(std::clamp(55.0f + pt.intensity * 0.55f, 45.0f, 195.0f));
            pt.r = shade; pt.g = shade; pt.b = shade;
            pt.colorized = false;
        }
        out.push_back(pt);
    }
    return out;
}

std::vector<core::LidarPointXYZIRT> PointCloudCleanup::removeSpatialHashOutliers(
    const std::vector<core::LidarPointXYZIRT>& points,
    bool enable_ror,
    double ror_radius_m,
    int ror_min_neighbors,
    bool enable_sor,
    int sor_k,
    double sor_std_mul,
    size_t* out_ror_removed,
    size_t* out_sor_removed) {
    if (out_ror_removed) *out_ror_removed = 0;
    if (out_sor_removed) *out_sor_removed = 0;

    const size_t N = points.size();
    if (N <= 12 || (!enable_ror && !enable_sor)) {
        return points;
    }

    const double ror_r = (enable_ror && ror_radius_m > 0.01) ? ror_radius_m : 0.10;
    const double cell_size = std::max(0.05, ror_r);
    const double inv_cell = 1.0 / cell_size;
    const double ror_r_sq = ror_r * ror_r;
    const double sor_r_sq = (cell_size * 1.65) * (cell_size * 1.65);

    const float cell_f = static_cast<float>(cell_size);
    const float ror_r_sq_f = static_cast<float>(ror_r_sq);
    const float sor_r_sq_f = static_cast<float>(sor_r_sq);

    // Compact 12-byte XYZ array (5x cache density vs 64-byte LidarPointXYZIRT) + flat linked-list spatial hash
    std::vector<Pt3f> pts_f(N);
    std::unordered_map<slam::VoxelKey, int32_t, slam::VoxelKeyHash> cell_head;
    cell_head.reserve(N / 4 + 16);
    std::vector<int32_t> next_pt(N, -1);

    for (size_t i = 0; i < N; ++i) {
        const double px = points[i].x;
        const double py = points[i].y;
        const double pz = points[i].z;
        pts_f[i] = Pt3f{static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz)};
        const slam::VoxelKey k = makeKey(px, py, pz, inv_cell);
        auto it = cell_head.find(k);
        if (it == cell_head.end()) {
            cell_head.emplace(k, static_cast<int32_t>(i));
        } else {
            next_pt[i] = it->second;
            it->second = static_cast<int32_t>(i);
        }
    }

    std::vector<uint8_t> keep(N, 1);
    std::vector<float> sor_mean_dist(N, 0.0f);
    const int min_nb = std::max(1, ror_min_neighbors);
    const int target_k = std::clamp(sor_k, 4, 20);
    const float isolated_dist = static_cast<float>(cell_size * 3.0);

    size_t ror_dropped = 0;
    float best_sq[20];
    // Center cell (0,0,0) first, then 6 face neighbors, 12 edge neighbors, 8 corner neighbors
    static constexpr int8_t kOffsets[27][3] = {
        { 0, 0, 0},
        {-1, 0, 0}, { 1, 0, 0}, { 0,-1, 0}, { 0, 1, 0}, { 0, 0,-1}, { 0, 0, 1},
        {-1,-1, 0}, {-1, 1, 0}, { 1,-1, 0}, { 1, 1, 0},
        {-1, 0,-1}, {-1, 0, 1}, { 1, 0,-1}, { 1, 0, 1},
        { 0,-1,-1}, { 0,-1, 1}, { 0, 1,-1}, { 0, 1, 1},
        {-1,-1,-1}, {-1,-1, 1}, {-1, 1,-1}, {-1, 1, 1},
        { 1,-1,-1}, { 1,-1, 1}, { 1, 1,-1}, { 1, 1, 1}
    };

    struct ActiveNbCell {
        int32_t head;
        int8_t dx, dy, dz;
    };
    ActiveNbCell active_nb[27];

    // Cell-grouped traversal: 17x fewer hash-table lookups + exact AABB cell-distance pruning!
    for (const auto& kv : cell_head) {
        const slam::VoxelKey base_k = kv.first;
        const float ox = static_cast<float>(base_k.x) * cell_f;
        const float oy = static_cast<float>(base_k.y) * cell_f;
        const float oz = static_cast<float>(base_k.z) * cell_f;

        int nb_cnt = 0;
        active_nb[nb_cnt++] = ActiveNbCell{kv.second, 0, 0, 0};
        for (int o = 1; o < 27; ++o) {
            const slam::VoxelKey nk{
                base_k.x + kOffsets[o][0],
                base_k.y + kOffsets[o][1],
                base_k.z + kOffsets[o][2]
            };
            auto it = cell_head.find(nk);
            if (it != cell_head.end()) {
                active_nb[nb_cnt++] = ActiveNbCell{it->second, kOffsets[o][0], kOffsets[o][1], kOffsets[o][2]};
            }
        }

        for (int32_t i_idx = kv.second; i_idx != -1; i_idx = next_pt[static_cast<size_t>(i_idx)]) {
            const Pt3f pi = pts_f[static_cast<size_t>(i_idx)];
            const float fx = std::clamp(pi.x - ox, 0.0f, cell_f);
            const float fy = std::clamp(pi.y - oy, 0.0f, cell_f);
            const float fz = std::clamp(pi.z - oz, 0.0f, cell_f);
            const float rx = cell_f - fx;
            const float ry = cell_f - fy;
            const float rz = cell_f - fz;

            int ror_cnt = 0;
            int sor_cnt = 0;
            int max_idx = 0;
            float max_sq = -1.0f;

            for (int c = 0; c < nb_cnt; ++c) {
                const auto& nb = active_nb[c];
                if (c > 0) {
                    const float mdx = (nb.dx < 0) ? fx : ((nb.dx > 0) ? rx : 0.0f);
                    const float mdy = (nb.dy < 0) ? fy : ((nb.dy > 0) ? ry : 0.0f);
                    const float mdz = (nb.dz < 0) ? fz : ((nb.dz > 0) ? rz : 0.0f);
                    const float cell_min_sq = mdx * mdx + mdy * mdy + mdz * mdz;
                    const bool need_ror = enable_ror && (ror_cnt < min_nb) && (cell_min_sq <= ror_r_sq_f);
                    const bool need_sor = enable_sor && (cell_min_sq <= sor_r_sq_f) &&
                                          ((sor_cnt < target_k) || (cell_min_sq < max_sq));
                    if (!need_ror && !need_sor) continue;
                }

                int32_t j = nb.head;
                int visited = 0;
                while (j != -1 && visited < 48) {
                    if (j != i_idx) {
                        const Pt3f pj = pts_f[static_cast<size_t>(j)];
                        const float ddx = pi.x - pj.x;
                        const float ddy = pi.y - pj.y;
                        const float ddz = pi.z - pj.z;
                        const float sq = ddx * ddx + ddy * ddy + ddz * ddz;
                        if (sq <= ror_r_sq_f) {
                            ror_cnt++;
                        }
                        if (enable_sor && sq <= sor_r_sq_f) {
                            if (sor_cnt < target_k) {
                                best_sq[sor_cnt] = sq;
                                if (sq > max_sq) {
                                    max_sq = sq;
                                    max_idx = sor_cnt;
                                }
                                sor_cnt++;
                            } else if (sq < max_sq) {
                                best_sq[max_idx] = sq;
                                max_sq = best_sq[0];
                                max_idx = 0;
                                for (int m = 1; m < target_k; ++m) {
                                    if (best_sq[m] > max_sq) {
                                        max_sq = best_sq[m];
                                        max_idx = m;
                                    }
                                }
                            }
                        }
                    }
                    j = next_pt[static_cast<size_t>(j)];
                    visited++;
                }
            }

            if (enable_ror && ror_cnt < min_nb) {
                keep[static_cast<size_t>(i_idx)] = 0;
                ror_dropped++;
            } else if (enable_sor) {
                if (sor_cnt > 0) {
                    float sor_sum = 0.0f;
                    for (int m = 0; m < sor_cnt; ++m) {
                        sor_sum += std::sqrt(best_sq[m]);
                    }
                    sor_mean_dist[static_cast<size_t>(i_idx)] = sor_sum / static_cast<float>(sor_cnt);
                } else {
                    sor_mean_dist[static_cast<size_t>(i_idx)] = isolated_dist;
                }
            }
        }
    }

    size_t sor_dropped = 0;
    if (enable_sor) {
        double sum = 0.0;
        size_t valid_n = 0;
        for (size_t i = 0; i < N; ++i) {
            if (keep[i]) {
                sum += static_cast<double>(sor_mean_dist[i]);
                valid_n++;
            }
        }
        if (valid_n > 10) {
            const double mean = sum / static_cast<double>(valid_n);
            double var = 0.0;
            for (size_t i = 0; i < N; ++i) {
                if (keep[i]) {
                    const double d = static_cast<double>(sor_mean_dist[i]) - mean;
                    var += d * d;
                }
            }
            const double stddev = std::sqrt(var / static_cast<double>(valid_n));
            const float thresh = static_cast<float>(mean + std::max(0.5, sor_std_mul) * stddev);
            for (size_t i = 0; i < N; ++i) {
                if (keep[i] && sor_mean_dist[i] > thresh) {
                    keep[i] = 0;
                    sor_dropped++;
                }
            }
        }
    }

    std::vector<core::LidarPointXYZIRT> out;
    out.reserve(N - ror_dropped - sor_dropped);
    for (size_t i = 0; i < N; ++i) {
        if (keep[i]) out.push_back(points[i]);
    }
    if (out_ror_removed) *out_ror_removed = ror_dropped;
    if (out_sor_removed) *out_sor_removed = sor_dropped;
    return out;
}

size_t PointCloudCleanup::denoisePlanarSurfaces(
    std::vector<core::LidarPointXYZIRT>& io_points,
    double cell_size_m,
    double blend,
    double max_shift_m) {
    const size_t N = io_points.size();
    if (N < 20) return 0;

    const double cell = std::max(0.06, cell_size_m);
    const double inv_cell = 1.0 / cell;
    const double alpha = std::clamp(blend, 0.0, 1.0);
    const double max_shift = std::max(0.005, max_shift_m);

    std::unordered_map<slam::VoxelKey, int32_t, slam::VoxelKeyHash> cell_head;
    cell_head.reserve(N / 4 + 16);
    std::vector<int32_t> next_pt(N, -1);

    for (size_t i = 0; i < N; ++i) {
        const slam::VoxelKey k = makeKey(io_points[i].x, io_points[i].y, io_points[i].z, inv_cell);
        auto it = cell_head.find(k);
        if (it == cell_head.end()) {
            cell_head.emplace(k, static_cast<int32_t>(i));
        } else {
            next_pt[i] = it->second;
            it->second = static_cast<int32_t>(i);
        }
    }

    // Snapshot original XYZ so 3x3x3 neighbor fits are completely order-independent (no grid-boundary slicing!)
    std::vector<math::Vec3d> orig_xyz(N);
    for (size_t i = 0; i < N; ++i) {
        orig_xyz[i] = math::Vec3d(io_points[i].x, io_points[i].y, io_points[i].z);
    }

    size_t denoised_count = 0;
    const double fit_r_sq = cell * cell;
    std::vector<uint32_t> patch_indices;
    patch_indices.reserve(160);

    for (const auto& kv : cell_head) {
        const slam::VoxelKey base_k = kv.first;
        // 1. Compute cell centroid as the seamless spherical neighborhood center
        math::Vec3d center(0.0, 0.0, 0.0);
        size_t cell_cnt = 0;
        for (int32_t idx = kv.second; idx != -1; idx = next_pt[static_cast<size_t>(idx)]) {
            center += orig_xyz[static_cast<size_t>(idx)];
            cell_cnt++;
        }
        if (cell_cnt == 0) continue;
        center *= (1.0 / static_cast<double>(cell_cnt));

        // 2. Gather boundary-free spherical patch across the 3x3x3 neighbor cells (r <= cell)
        patch_indices.clear();
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dy = -1; dy <= 1; ++dy) {
                for (int64_t dz = -1; dz <= 1; ++dz) {
                    const slam::VoxelKey nk{base_k.x + dx, base_k.y + dy, base_k.z + dz};
                    auto it = cell_head.find(nk);
                    if (it == cell_head.end()) continue;
                    int32_t idx = it->second;
                    while (idx != -1) {
                        const math::Vec3d d = orig_xyz[static_cast<size_t>(idx)] - center;
                        if (d.squaredNorm() <= fit_r_sq) {
                            patch_indices.push_back(static_cast<uint32_t>(idx));
                        }
                        idx = next_pt[static_cast<size_t>(idx)];
                    }
                }
            }
        }
        if (patch_indices.size() < 10) continue;

        // Evenly stride if patch is very dense so PCA stays O(1) per cell and spatially unbiased
        const size_t stride = std::max<size_t>(1, patch_indices.size() / 96);
        math::Vec3d mean(0.0, 0.0, 0.0);
        size_t sample_cnt = 0;
        for (size_t s = 0; s < patch_indices.size(); s += stride) {
            mean += orig_xyz[patch_indices[s]];
            sample_cnt++;
        }
        const double inv_cnt = 1.0 / static_cast<double>(sample_cnt);
        mean *= inv_cnt;

        math::Mat3d cov = math::Mat3d::Zero();
        for (size_t s = 0; s < patch_indices.size(); s += stride) {
            const math::Vec3d d = orig_xyz[patch_indices[s]] - mean;
            cov = cov + math::Mat3d::OuterProduct(d, d);
        }
        cov = cov * inv_cnt;

        const math::SymEigen3d eig = math::SymEigen3d::compute(cov);
        const double l0 = std::max(0.0, eig.eigenvalues[0]);
        const double l1 = std::max(0.0, eig.eigenvalues[1]);

        // Strict planarity gate: corners, edges, trim, and curved furniture have l0 >= 0.04*l1 and stay untouched!
        if (l1 <= 1e-4 || l0 >= 0.04 * l1 || l0 >= 0.0016) {
            continue;
        }

        const math::Vec3d normal(eig.eigenvectors(0, 0),
                                 eig.eigenvectors(1, 0),
                                 eig.eigenvectors(2, 0));
        const double n_norm = normal.norm();
        if (n_norm < 1e-6) continue;
        const math::Vec3d unit_n = normal / n_norm;

        // Project points belonging to base_k onto the seamless local plane
        int32_t idx = kv.second;
        while (idx != -1) {
            const math::Vec3d& p0 = orig_xyz[static_cast<size_t>(idx)];
            auto& pt = io_points[static_cast<size_t>(idx)];
            const double signed_dist = (p0 - mean).dot(unit_n);
            const double shift = std::clamp(alpha * signed_dist, -max_shift, max_shift);
            pt.x = p0.x - shift * unit_n.x;
            pt.y = p0.y - shift * unit_n.y;
            pt.z = p0.z - shift * unit_n.z;
            denoised_count++;
            idx = next_pt[static_cast<size_t>(idx)];
        }
    }

    return denoised_count;
}

bool PointCloudCleanup::computeAndApplyFloorLeveling(
    std::vector<core::LidarPointXYZIRT>& io_points,
    double min_tilt_deg,
    double max_tilt_deg,
    math::SE3d* out_T_level_world,
    double* out_tilt_deg) {
    if (out_T_level_world) *out_T_level_world = math::SE3d::Identity();
    if (out_tilt_deg) *out_tilt_deg = 0.0;
    const size_t N = io_points.size();
    if (N < 500) return false;

    // Bin points into 0.25 m cells and fit boundary-free spherical horizontal patches across 3x3x3 neighbors
    const double cell = 0.25;
    const double inv_cell = 1.0 / cell;
    const double fit_r_sq = cell * cell;
    std::unordered_map<slam::VoxelKey, int32_t, slam::VoxelKeyHash> cell_head;
    cell_head.reserve(N / 8 + 16);
    std::vector<int32_t> next_pt(N, -1);
    for (size_t i = 0; i < N; ++i) {
        const slam::VoxelKey k = makeKey(io_points[i].x, io_points[i].y, io_points[i].z, inv_cell);
        auto it = cell_head.find(k);
        if (it == cell_head.end()) {
            cell_head.emplace(k, static_cast<int32_t>(i));
        } else {
            next_pt[i] = it->second;
            it->second = static_cast<int32_t>(i);
        }
    }

    struct HorizPatch {
        math::Vec3d normal;
        size_t weight;
    };
    std::vector<HorizPatch> horiz_patches;

    std::vector<uint32_t> idxs;
    idxs.reserve(256);
    for (const auto& kv : cell_head) {
        const slam::VoxelKey base_k = kv.first;
        math::Vec3d center(0.0, 0.0, 0.0);
        size_t cell_cnt = 0;
        for (int32_t idx = kv.second; idx != -1; idx = next_pt[static_cast<size_t>(idx)]) {
            center.x += io_points[static_cast<size_t>(idx)].x;
            center.y += io_points[static_cast<size_t>(idx)].y;
            center.z += io_points[static_cast<size_t>(idx)].z;
            cell_cnt++;
        }
        if (cell_cnt < 4) continue;
        center *= (1.0 / static_cast<double>(cell_cnt));

        idxs.clear();
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dy = -1; dy <= 1; ++dy) {
                for (int64_t dz = -1; dz <= 1; ++dz) {
                    const slam::VoxelKey nk{base_k.x + dx, base_k.y + dy, base_k.z + dz};
                    auto it = cell_head.find(nk);
                    if (it == cell_head.end()) continue;
                    int32_t idx = it->second;
                    while (idx != -1) {
                        const auto& p = io_points[static_cast<size_t>(idx)];
                        const double ddx = p.x - center.x;
                        const double ddy = p.y - center.y;
                        const double ddz = p.z - center.z;
                        if (ddx * ddx + ddy * ddy + ddz * ddz <= fit_r_sq) {
                            idxs.push_back(static_cast<uint32_t>(idx));
                        }
                        idx = next_pt[static_cast<size_t>(idx)];
                    }
                }
            }
        }
        if (idxs.size() < 15) continue;

        const size_t stride = std::max<size_t>(1, idxs.size() / 96);
        math::Vec3d mean(0.0, 0.0, 0.0);
        size_t sample_cnt = 0;
        for (size_t s = 0; s < idxs.size(); s += stride) {
            const auto& p = io_points[idxs[s]];
            mean.x += p.x; mean.y += p.y; mean.z += p.z;
            sample_cnt++;
        }
        const double inv_c = 1.0 / static_cast<double>(sample_cnt);
        mean *= inv_c;

        math::Mat3d cov = math::Mat3d::Zero();
        for (size_t s = 0; s < idxs.size(); s += stride) {
            const auto& p = io_points[idxs[s]];
            const math::Vec3d d(p.x - mean.x, p.y - mean.y, p.z - mean.z);
            cov = cov + math::Mat3d::OuterProduct(d, d);
        }
        cov = cov * inv_c;

        const math::SymEigen3d eig = math::SymEigen3d::compute(cov);
        const double l0 = std::max(0.0, eig.eigenvalues[0]);
        const double l1 = std::max(0.0, eig.eigenvalues[1]);
        if (l1 <= 5e-4 || l0 >= 0.025 * l1 || l0 >= 0.0012) continue;

        math::Vec3d n(eig.eigenvectors(0, 0), eig.eigenvectors(1, 0), eig.eigenvectors(2, 0));
        if (n.z < 0.0) n = -n;
        // Strict horizontal cone (~8.1 deg max) so vaulted ceilings or ramps never bias true floor normal
        if (n.z < 0.990) continue;

        horiz_patches.push_back({n.normalized(), cell_cnt});
    }

    if (horiz_patches.size() < 12) return false;

    size_t total_w = 0;
    for (const auto& hp : horiz_patches) total_w += hp.weight;
    if (total_w < 250) return false;

    // Best-seed consensus mode finding (robust even when sloped/vaulted ceilings coexist with a flat floor!)
    const size_t seed_stride = std::max<size_t>(1, horiz_patches.size() / 48);
    size_t best_w = 0;
    math::Vec3d best_seed(0.0, 0.0, 1.0);
    for (size_t s = 0; s < horiz_patches.size(); s += seed_stride) {
        const math::Vec3d& cand = horiz_patches[s].normal;
        size_t w_acc = 0;
        for (const auto& hp : horiz_patches) {
            if (hp.normal.dot(cand) >= 0.99905) {  // cos(2.5 deg)
                w_acc += hp.weight;
            }
        }
        if (w_acc > best_w) {
            best_w = w_acc;
            best_seed = cand;
        }
    }

    math::Vec3d n_refined(0.0, 0.0, 0.0);
    size_t inlier_w = 0;
    size_t inlier_patches = 0;
    for (const auto& hp : horiz_patches) {
        if (hp.normal.dot(best_seed) >= 0.99905) {  // cos(2.5 deg)
            n_refined += hp.normal * static_cast<double>(hp.weight);
            inlier_w += hp.weight;
            inlier_patches++;
        }
    }
    if (inlier_patches < 10 || inlier_w < (total_w * 35) / 100 || n_refined.norm() < 1e-6) return false;
    const math::Vec3d n_avg = n_refined.normalized();

    const double cos_tilt = std::clamp(n_avg.z, -1.0, 1.0);
    const double tilt_rad = std::acos(cos_tilt);
    const double tilt_deg = tilt_rad * math::kRadToDeg;
    if (out_tilt_deg) *out_tilt_deg = tilt_deg;

    // Only apply guarded fine gravity leveling within [min_tilt_deg, max_tilt_deg]
    if (tilt_deg < min_tilt_deg || tilt_deg > max_tilt_deg) {
        return false;
    }

    const math::Vec3d z_up(0.0, 0.0, 1.0);
    const math::Vec3d axis = n_avg.cross(z_up);
    const double axis_norm = axis.norm();
    if (axis_norm < 1e-7) return false;

    const math::SO3d R_level = math::SO3d::exp((axis / axis_norm) * tilt_rad);
    const math::SE3d T_level(R_level, math::Vec3d(0.0, 0.0, 0.0));
    if (out_T_level_world) *out_T_level_world = T_level;

    for (auto& pt : io_points) {
        const math::Vec3d p = R_level * math::Vec3d(pt.x, pt.y, pt.z);
        pt.x = p.x;
        pt.y = p.y;
        pt.z = p.z;
    }
    return true;
}

CleanupStats PointCloudCleanup::runCloudCleanupPipeline(
    const std::vector<core::LidarPointXYZIRT>& input_cloud,
    std::vector<core::LidarPointXYZIRT>& out_cleaned_cloud,
    const CleanupConfig& config) {
    const auto t0 = std::chrono::steady_clock::now();
    CleanupStats stats;
    stats.raw_input_points = input_cloud.size();
    if (input_cloud.empty()) {
        out_cleaned_cloud.clear();
        return stats;
    }

    std::vector<core::LidarPointXYZIRT> working = config.enable_voxel_centroid_fusion
        ? fuseVoxelCentroidsWeightedRgb(input_cloud, config.voxel_size_m)
        : input_cloud;
    stats.voxel_fused_points = working.size();

    if (config.enable_ror || config.enable_sor) {
        working = removeSpatialHashOutliers(
            working,
            config.enable_ror, config.ror_radius_m, config.ror_min_neighbors,
            config.enable_sor, config.sor_k, config.sor_std_mul,
            &stats.ror_removed, &stats.sor_removed);
    }

    if (config.enable_planar_denoise) {
        stats.planar_denoised_points = denoisePlanarSurfaces(
            working, config.planar_cell_m, config.planar_blend, config.planar_max_shift_m);
    }

    if (config.enable_floor_leveling) {
        stats.floor_leveled = computeAndApplyFloorLeveling(
            working, config.min_level_tilt_deg, config.max_level_tilt_deg,
            &stats.T_level_world, &stats.floor_tilt_corrected_deg);
    }

    stats.final_output_points = working.size();
    out_cleaned_cloud = std::move(working);
    const auto t1 = std::chrono::steady_clock::now();
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return stats;
}

CleanupStats PointCloudCleanup::runPostScanPipeline(
    const std::vector<core::KeyframeSubmap>& submaps,
    std::vector<core::LidarPointXYZIRT>& out_cleaned_cloud,
    const CleanupConfig& config) {
    const auto t0 = std::chrono::steady_clock::now();
    CleanupStats stats;
    for (const auto& sm : submaps) stats.raw_input_points += sm.local_points.size();
    if (stats.raw_input_points == 0) {
        out_cleaned_cloud.clear();
        return stats;
    }

    std::vector<core::LidarPointXYZIRT> stage1;
    if (config.enable_dynamic_filter) {
        stage1 = filterDynamicTransients(
            submaps, config.dynamic_voxel_m, config.dynamic_min_pass_through, &stats.dynamic_removed);
    } else {
        stage1.reserve(stats.raw_input_points);
        for (const auto& sm : submaps) {
            const auto& T = sm.T_world_keyframe;
            for (const auto& pt : sm.local_points) {
                if (!isFinitePoint(pt)) continue;
                const math::Vec3d pw = T.transform(math::Vec3d(pt.x, pt.y, pt.z));
                core::LidarPointXYZIRT w = pt;
                w.x = pw.x; w.y = pw.y; w.z = pw.z;
                stage1.push_back(w);
            }
        }
    }

    CleanupStats cloud_stats = runCloudCleanupPipeline(stage1, out_cleaned_cloud, config);
    stats.voxel_fused_points = cloud_stats.voxel_fused_points;
    stats.ror_removed = cloud_stats.ror_removed;
    stats.sor_removed = cloud_stats.sor_removed;
    stats.planar_denoised_points = cloud_stats.planar_denoised_points;
    stats.final_output_points = cloud_stats.final_output_points;
    stats.floor_leveled = cloud_stats.floor_leveled;
    stats.floor_tilt_corrected_deg = cloud_stats.floor_tilt_corrected_deg;
    stats.T_level_world = cloud_stats.T_level_world;

    const auto t1 = std::chrono::steady_clock::now();
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return stats;
}

}  // namespace processing
}  // namespace avscan
