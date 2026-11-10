#pragma once

#include "avscan/backend/PoseGraphOptimizer.hpp"
#include "avscan/core/Types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace avscan {
namespace storage {

struct PoiPhotoRecord {
    uint64_t photo_id{0};
    double timestamp_sec{0.0};
    math::SE3d T_world_camera{math::SE3d::Identity()};
    std::string image_file_path{};
    std::string note{};
};

struct ProjectMetadata {
    std::string project_id{"PROJ_001"};
    std::string project_name{"AV Scan Session"};
    std::string root_directory{};
    double created_timestamp_sec{0.0};
    double updated_timestamp_sec{0.0};
    size_t keyframe_count{0};
    size_t loop_closure_count{0};
    bool clean_shutdown{false};
};

class ProjectStore {
public:
    ProjectStore() = default;

    // Creates or opens a local offline scan project directory and initializes crash-recovery WAL
    bool initializeProject(const std::string& root_dir,
                           const std::string& project_id,
                           const std::string& project_name);

    // Appends a keyframe submap to the write-ahead crash recovery log immediately upon creation
    bool appendKeyframeToWal(const core::KeyframeSubmap& submap);

    // Archives a keyframe submap's point cloud to a binary chunk file on disk and frees its in-RAM vector
    bool archiveSubmapToDisk(core::KeyframeSubmap& io_submap) const;

    // Reloads an archived keyframe submap's point cloud from disk into RAM
    bool loadSubmapFromDisk(core::KeyframeSubmap& io_submap) const;

    // Saves full project state (metadata, calibration, pose graph factors, submaps, POI photos) cleanly
    bool saveProjectSession(const core::ExtrinsicCalibrationProfile& calib,
                            const std::vector<core::KeyframeSubmap>& submaps,
                            const std::vector<backend::PoseGraphFactor>& factors,
                            const std::vector<PoiPhotoRecord>& photos);

    // Recovers an interrupted or saved scan session from disk
    bool recoverOrLoadSession(const std::string& root_dir,
                              ProjectMetadata& out_meta,
                              core::ExtrinsicCalibrationProfile& out_calib,
                              std::vector<core::KeyframeSubmap>& out_submaps,
                              std::vector<backend::PoseGraphFactor>& out_factors,
                              std::vector<PoiPhotoRecord>& out_photos,
                              bool& out_recovered_from_crash);

    const ProjectMetadata& getMetadata() const { return meta_; }

private:
    ProjectMetadata meta_{};
};

}  // namespace storage
}  // namespace avscan
