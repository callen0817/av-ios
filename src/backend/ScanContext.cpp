#include "avscan/backend/ScanContext.hpp"
#include <algorithm>
#include <cmath>

namespace avscan {
namespace backend {

ScanContextDatabase::ScanContextDatabase(int min_keyframe_gap, double sc_distance_threshold)
    : min_keyframe_gap_(std::max(1, min_keyframe_gap)),
      sc_distance_threshold_(std::clamp(sc_distance_threshold, 0.05, 0.65)) {}

ScanContextDescriptor ScanContextDatabase::computeDescriptor(
    uint64_t keyframe_id,
    double timestamp_sec,
    const std::vector<core::LidarPointXYZIRT>& local_points,
    const std::optional<core::RgbFrame>& optional_rgb) {
    ScanContextDescriptor desc;
    desc.keyframe_id = keyframe_id;
    desc.timestamp_sec = timestamp_sec;
    desc.matrix.fill(0.0f);
    desc.ring_key.fill(0.0f);
    desc.visual_bow_signature.fill(0.0f);

    const double ring_step = ScanContextDescriptor::kMaxRadiusMeters / ScanContextDescriptor::kNumRings;
    const double sector_step = (2.0 * math::kPi) / ScanContextDescriptor::kNumSectors;

    for (const auto& pt : local_points) {
        double r = std::sqrt(pt.x * pt.x + pt.y * pt.y);
        if (r < 0.15 || r >= ScanContextDescriptor::kMaxRadiusMeters) continue;

        double angle = std::atan2(pt.y, pt.x);
        if (angle < 0.0) angle += 2.0 * math::kPi;

        int r_idx = std::clamp(static_cast<int>(r / ring_step), 0, ScanContextDescriptor::kNumRings - 1);
        int s_idx = std::clamp(static_cast<int>(angle / sector_step), 0, ScanContextDescriptor::kNumSectors - 1);

        float shifted_z = static_cast<float>(std::max(0.0, pt.z + ScanContextDescriptor::kHeightOffsetMeters));
        size_t cell = static_cast<size_t>(r_idx * ScanContextDescriptor::kNumSectors + s_idx);
        if (shifted_z > desc.matrix[cell]) {
            desc.matrix[cell] = shifted_z;
        }
    }

    for (int r = 0; r < ScanContextDescriptor::kNumRings; ++r) {
        float sum = 0.0f;
        for (int s = 0; s < ScanContextDescriptor::kNumSectors; ++s) {
            sum += desc.matrix[static_cast<size_t>(r * ScanContextDescriptor::kNumSectors + s)];
        }
        desc.ring_key[static_cast<size_t>(r)] = sum / static_cast<float>(ScanContextDescriptor::kNumSectors);
    }

    // Compute optional 64-bin visual orientation-intensity signature if RGB frame is present and well-lit
    if (optional_rgb.has_value() && !optional_rgb->gray_bytes.empty()) {
        const auto& img = optional_rgb.value();
        double mean_lum = 0.0;
        for (uint8_t px : img.gray_bytes) mean_lum += px;
        mean_lum /= static_cast<double>(img.gray_bytes.size());

        // Only enable visual verifier if scene is not dark (mean luminance > 15)
        if (mean_lum > 15.0 && img.intrinsics.width >= 16 && img.intrinsics.height >= 16) {
            int w = img.intrinsics.width;
            int h = img.intrinsics.height;
            double total_mag = 0.0;
            for (int y = 2; y < h - 2; y += 2) {
                for (int x = 2; x < w - 2; x += 2) {
                    double gx = static_cast<double>(img.gray_bytes[static_cast<size_t>(y * w + x + 1)]) -
                                static_cast<double>(img.gray_bytes[static_cast<size_t>(y * w + x - 1)]);
                    double gy = static_cast<double>(img.gray_bytes[static_cast<size_t>((y + 1) * w + x)]) -
                                static_cast<double>(img.gray_bytes[static_cast<size_t>((y - 1) * w + x)]);
                    double mag = std::sqrt(gx * gx + gy * gy);
                    if (mag < 5.0) continue;
                    double ori = std::atan2(gy, gx) + math::kPi;
                    int ori_bin = std::clamp(static_cast<int>((ori / (2.0 * math::kPi)) * 8.0), 0, 7);
                    int spatial_bin = ((y * 2) / h) * 4 + ((x * 4) / w);
                    spatial_bin = std::clamp(spatial_bin, 0, 7);
                    desc.visual_bow_signature[static_cast<size_t>(spatial_bin * 8 + ori_bin)] += static_cast<float>(mag);
                    total_mag += mag;
                }
            }
            if (total_mag > 1e-3) {
                float norm_sq = 0.0f;
                for (float v : desc.visual_bow_signature) norm_sq += v * v;
                float inv_norm = (norm_sq > 1e-6f) ? (1.0f / std::sqrt(norm_sq)) : 1.0f;
                for (float& v : desc.visual_bow_signature) v *= inv_norm;
                desc.has_visual_signature = true;
            }
        }
    }

    return desc;
}

double ScanContextDatabase::computeDistanceAndYawShift(const ScanContextDescriptor& query,
                                                       const ScanContextDescriptor& candidate,
                                                       int& out_best_sector_shift) {
    out_best_sector_shift = 0;
    double min_dist = 1.0;

    for (int shift = 0; shift < ScanContextDescriptor::kNumSectors; ++shift) {
        double col_cos_sum = 0.0;
        int valid_cols = 0;

        for (int s = 0; s < ScanContextDescriptor::kNumSectors; ++s) {
            int shifted_s = (s + shift) % ScanContextDescriptor::kNumSectors;
            double dot = 0.0, n1 = 0.0, n2 = 0.0;
            for (int r = 0; r < ScanContextDescriptor::kNumRings; ++r) {
                double v1 = query.matrix[static_cast<size_t>(r * ScanContextDescriptor::kNumSectors + s)];
                double v2 = candidate.matrix[static_cast<size_t>(r * ScanContextDescriptor::kNumSectors + shifted_s)];
                dot += v1 * v2;
                n1 += v1 * v1;
                n2 += v2 * v2;
            }
            if (n1 > 1e-6 && n2 > 1e-6) {
                col_cos_sum += dot / std::sqrt(n1 * n2);
                valid_cols++;
            }
        }

        double dist = (valid_cols > 0) ? (1.0 - (col_cos_sum / static_cast<double>(valid_cols))) : 1.0;
        if (dist < min_dist) {
            min_dist = dist;
            out_best_sector_shift = shift;
        }
    }
    return min_dist;
}

double ScanContextDatabase::computeVisualSimilarity(const ScanContextDescriptor& a, const ScanContextDescriptor& b) {
    if (!a.has_visual_signature || !b.has_visual_signature) {
        return 0.0;
    }
    double dot = 0.0;
    for (size_t i = 0; i < a.visual_bow_signature.size(); ++i) {
        dot += static_cast<double>(a.visual_bow_signature[i] * b.visual_bow_signature[i]);
    }
    return std::clamp(dot, 0.0, 1.0);
}

void ScanContextDatabase::addKeyframeDescriptor(const ScanContextDescriptor& desc) {
    database_.push_back(desc);
}

LoopCandidate ScanContextDatabase::detectLoopClosure(const ScanContextDescriptor& query_desc) const {
    LoopCandidate result;
    result.query_keyframe_id = query_desc.keyframe_id;
    if (database_.size() <= static_cast<size_t>(min_keyframe_gap_)) {
        return result;
    }

    size_t search_limit = database_.size() - static_cast<size_t>(min_keyframe_gap_);
    struct RingCandidate {
        size_t index{0};
        double ring_l1_dist{0.0};
    };
    std::vector<RingCandidate> shortlist;
    shortlist.reserve(search_limit);

    for (size_t i = 0; i < search_limit; ++i) {
        double l1 = 0.0;
        for (int r = 0; r < ScanContextDescriptor::kNumRings; ++r) {
            l1 += std::abs(query_desc.ring_key[static_cast<size_t>(r)] -
                           database_[i].ring_key[static_cast<size_t>(r)]);
        }
        shortlist.push_back({i, l1});
    }

    std::sort(shortlist.begin(), shortlist.end(),
              [](const RingCandidate& a, const RingCandidate& b) { return a.ring_l1_dist < b.ring_l1_dist; });

    size_t top_k = std::min<size_t>(10, shortlist.size());
    double best_dist = 1.0;
    int best_shift = 0;
    size_t best_idx = 0;

    for (size_t k = 0; k < top_k; ++k) {
        size_t idx = shortlist[k].index;
        int shift = 0;
        double dist = computeDistanceAndYawShift(query_desc, database_[idx], shift);
        // Secondary visual evidence boosts confidence when geometry is slightly ambiguous
        double vis_sim = computeVisualSimilarity(query_desc, database_[idx]);
        double effective_dist = (vis_sim > 0.75) ? (dist * 0.85) : dist;

        if (effective_dist < best_dist) {
            best_dist = effective_dist;
            best_shift = shift;
            best_idx = idx;
        }
    }

    if (best_dist <= sc_distance_threshold_) {
        result.match_keyframe_id = database_[best_idx].keyframe_id;
        result.sc_cosine_distance = best_dist;
        result.visual_similarity = computeVisualSimilarity(query_desc, database_[best_idx]);
        result.initial_yaw_shift_rad =
            static_cast<double>(best_shift) * ((2.0 * math::kPi) / ScanContextDescriptor::kNumSectors);
        result.valid = true;
    }
    return result;
}

LoopCandidate ScanContextDatabase::findRelocalizationCandidate(const ScanContextDescriptor& lost_query_desc) const {
    LoopCandidate result;
    result.query_keyframe_id = lost_query_desc.keyframe_id;
    if (database_.empty()) return result;

    double best_dist = 1.0;
    int best_shift = 0;
    size_t best_idx = 0;

    for (size_t i = 0; i < database_.size(); ++i) {
        int shift = 0;
        double dist = computeDistanceAndYawShift(lost_query_desc, database_[i], shift);
        if (dist < best_dist) {
            best_dist = dist;
            best_shift = shift;
            best_idx = i;
        }
    }

    if (best_dist <= sc_distance_threshold_ * 1.25) {
        result.match_keyframe_id = database_[best_idx].keyframe_id;
        result.sc_cosine_distance = best_dist;
        result.visual_similarity = computeVisualSimilarity(lost_query_desc, database_[best_idx]);
        result.initial_yaw_shift_rad =
            static_cast<double>(best_shift) * ((2.0 * math::kPi) / ScanContextDescriptor::kNumSectors);
        result.valid = true;
    }
    return result;
}

}  // namespace backend
}  // namespace avscan
