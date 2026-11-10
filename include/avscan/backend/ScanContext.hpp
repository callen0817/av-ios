#pragma once

#include "avscan/core/Types.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace avscan {
namespace backend {

struct ScanContextDescriptor {
    static constexpr int kNumRings = 20;
    static constexpr int kNumSectors = 60;
    static constexpr double kMaxRadiusMeters = 40.0;
    static constexpr double kHeightOffsetMeters = 2.0;

    uint64_t keyframe_id{0};
    double timestamp_sec{0.0};
    std::array<float, kNumRings * kNumSectors> matrix{};  // Row-major [ring * kNumSectors + sector]
    std::array<float, kNumRings> ring_key{};              // Rotation-invariant ring summary
    std::array<float, 64> visual_bow_signature{};         // Optional 64-bin visual gradient/ORB signature
    bool has_visual_signature{false};
};

struct LoopCandidate {
    uint64_t query_keyframe_id{0};
    uint64_t match_keyframe_id{0};
    double sc_cosine_distance{1.0};
    double visual_similarity{0.0};
    double initial_yaw_shift_rad{0.0};
    bool valid{false};
};

class ScanContextDatabase {
public:
    explicit ScanContextDatabase(int min_keyframe_gap = 15, double sc_distance_threshold = 0.22);

    static ScanContextDescriptor computeDescriptor(
        uint64_t keyframe_id,
        double timestamp_sec,
        const std::vector<core::LidarPointXYZIRT>& local_points,
        const std::optional<core::RgbFrame>& optional_rgb = std::nullopt);

    // Computes minimum column-shifted cosine distance between two Scan Context descriptors
    static double computeDistanceAndYawShift(const ScanContextDescriptor& query,
                                             const ScanContextDescriptor& candidate,
                                             int& out_best_sector_shift);

    void addKeyframeDescriptor(const ScanContextDescriptor& desc);

    // Searches historical keyframes (excluding recent min_keyframe_gap) for loop closure candidates
    LoopCandidate detectLoopClosure(const ScanContextDescriptor& query_desc) const;

    // Searches all stored keyframes (including recent ones) for relocalization after tracking loss
    LoopCandidate findRelocalizationCandidate(const ScanContextDescriptor& lost_query_desc) const;

    size_t size() const { return database_.size(); }
    void clear() { database_.clear(); }
    const std::vector<ScanContextDescriptor>& getDescriptors() const { return database_; }

private:
    static double computeVisualSimilarity(const ScanContextDescriptor& a, const ScanContextDescriptor& b);

    int min_keyframe_gap_{15};
    double sc_distance_threshold_{0.22};
    std::vector<ScanContextDescriptor> database_;
};

}  // namespace backend
}  // namespace avscan
