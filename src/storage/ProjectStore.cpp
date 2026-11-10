#include "avscan/storage/ProjectStore.hpp"
#include "avscan/calibration/TargetlessCalibrator.hpp"
#include "avscan/sensors/AiryUdpReceiver.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace avscan {
namespace storage {

bool ProjectStore::initializeProject(const std::string& root_dir,
                                     const std::string& project_id,
                                     const std::string& project_name) {
    std::error_code ec;
    fs::create_directories(fs::path(root_dir) / "chunks", ec);
    fs::create_directories(fs::path(root_dir) / "photos", ec);
    if (!fs::exists(root_dir)) {
        return false;
    }

    double now_sec = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    meta_.project_id = project_id;
    meta_.project_name = project_name;
    meta_.root_directory = root_dir;
    meta_.created_timestamp_sec = now_sec;
    meta_.updated_timestamp_sec = now_sec;
    meta_.keyframe_count = 0;
    meta_.loop_closure_count = 0;
    meta_.clean_shutdown = false;

    // Write active session lock / WAL header
    std::ofstream wal((fs::path(root_dir) / "session.wal").string(), std::ios::binary | std::ios::trunc);
    if (!wal.is_open()) return false;
    uint32_t magic = 0x4156574Cu;  // "AVWL"
    wal.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

    std::ofstream meta_out((fs::path(root_dir) / "project.meta").string(), std::ios::trunc);
    meta_out << "project_id=" << meta_.project_id << "\n";
    meta_out << "project_name=" << meta_.project_name << "\n";
    meta_out << "clean_shutdown=0\n";
    return meta_out.good();
}

bool ProjectStore::archiveSubmapToDisk(core::KeyframeSubmap& io_submap) const {
    if (meta_.root_directory.empty()) return false;
    if (io_submap.archived_to_disk && io_submap.local_points.empty()) return true;

    fs::path chunk_path = fs::path(meta_.root_directory) / "chunks" /
                          ("submap_" + std::to_string(io_submap.keyframe_id) + ".bin");
    std::ofstream out(chunk_path.string(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    uint32_t magic = 0x534D4150u;  // "SMAP"
    uint64_t kf_id = io_submap.keyframe_id;
    uint32_t count = static_cast<uint32_t>(io_submap.local_points.size());
    uint32_t crc = 0;
    if (count > 0) {
        crc = sensors::RawSensorRecorder::computeCrc32(
            reinterpret_cast<const uint8_t*>(io_submap.local_points.data()),
            count * sizeof(core::LidarPointXYZIRT));
    }

    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&kf_id), sizeof(kf_id));
    out.write(reinterpret_cast<const char*>(&io_submap.timestamp_sec), sizeof(io_submap.timestamp_sec));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    out.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
    if (count > 0) {
        out.write(reinterpret_cast<const char*>(io_submap.local_points.data()),
                  static_cast<std::streamsize>(count * sizeof(core::LidarPointXYZIRT)));
    }
    if (!out.good()) return false;

    io_submap.disk_chunk_path = chunk_path.string();
    io_submap.archived_to_disk = true;
    io_submap.local_points.clear();
    io_submap.local_points.shrink_to_fit();
    return true;
}

bool ProjectStore::loadSubmapFromDisk(core::KeyframeSubmap& io_submap) const {
    if (!io_submap.local_points.empty()) return true;
    std::string path = io_submap.disk_chunk_path;
    if (path.empty() && !meta_.root_directory.empty()) {
        path = (fs::path(meta_.root_directory) / "chunks" /
                ("submap_" + std::to_string(io_submap.keyframe_id) + ".bin")).string();
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    uint32_t magic = 0;
    uint64_t kf_id = 0;
    double ts = 0.0;
    uint32_t count = 0;
    uint32_t expected_crc = 0;

    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&kf_id), sizeof(kf_id));
    in.read(reinterpret_cast<char*>(&ts), sizeof(ts));
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    in.read(reinterpret_cast<char*>(&expected_crc), sizeof(expected_crc));
    if (!in.good() || magic != 0x534D4150u || kf_id != io_submap.keyframe_id) {
        return false;
    }

    io_submap.local_points.resize(count);
    if (count > 0) {
        in.read(reinterpret_cast<char*>(io_submap.local_points.data()),
                static_cast<std::streamsize>(count * sizeof(core::LidarPointXYZIRT)));
        if (!in.good()) {
            io_submap.local_points.clear();
            return false;
        }
        uint32_t actual_crc = sensors::RawSensorRecorder::computeCrc32(
            reinterpret_cast<const uint8_t*>(io_submap.local_points.data()),
            count * sizeof(core::LidarPointXYZIRT));
        if (actual_crc != expected_crc) {
            io_submap.local_points.clear();
            return false;
        }
    }
    return true;
}

bool ProjectStore::appendKeyframeToWal(const core::KeyframeSubmap& submap) {
    if (meta_.root_directory.empty()) return false;

    // Persist point chunk to disk so crash recovery can always restore full geometry
    core::KeyframeSubmap copy = submap;
    if (!copy.local_points.empty()) {
        fs::path chunk_path = fs::path(meta_.root_directory) / "chunks" /
                              ("submap_" + std::to_string(copy.keyframe_id) + ".bin");
        std::ofstream chunk_out(chunk_path.string(), std::ios::binary | std::ios::trunc);
        if (chunk_out.is_open()) {
            uint32_t magic = 0x534D4150u;
            uint32_t count = static_cast<uint32_t>(copy.local_points.size());
            uint32_t crc = sensors::RawSensorRecorder::computeCrc32(
                reinterpret_cast<const uint8_t*>(copy.local_points.data()),
                count * sizeof(core::LidarPointXYZIRT));
            chunk_out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
            chunk_out.write(reinterpret_cast<const char*>(&copy.keyframe_id), sizeof(copy.keyframe_id));
            chunk_out.write(reinterpret_cast<const char*>(&copy.timestamp_sec), sizeof(copy.timestamp_sec));
            chunk_out.write(reinterpret_cast<const char*>(&count), sizeof(count));
            chunk_out.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
            chunk_out.write(reinterpret_cast<const char*>(copy.local_points.data()),
                            static_cast<std::streamsize>(count * sizeof(core::LidarPointXYZIRT)));
        }
    }

    std::ofstream wal((fs::path(meta_.root_directory) / "session.wal").string(), std::ios::binary | std::ios::app);
    if (!wal.is_open()) return false;

    math::Quatd q = submap.T_world_keyframe.so3.unit_quaternion();
    const math::Vec3d& t = submap.T_world_keyframe.t;
    double pose_vec[7] = {q.x, q.y, q.z, q.w, t.x, t.y, t.z};

    wal.write(reinterpret_cast<const char*>(&submap.keyframe_id), sizeof(submap.keyframe_id));
    wal.write(reinterpret_cast<const char*>(&submap.timestamp_sec), sizeof(submap.timestamp_sec));
    wal.write(reinterpret_cast<const char*>(pose_vec), sizeof(pose_vec));
    wal.flush();
    meta_.keyframe_count++;
    return wal.good();
}

bool ProjectStore::saveProjectSession(const core::ExtrinsicCalibrationProfile& calib,
                                      const std::vector<core::KeyframeSubmap>& submaps,
                                      const std::vector<backend::PoseGraphFactor>& factors,
                                      const std::vector<PoiPhotoRecord>& photos) {
    if (meta_.root_directory.empty()) return false;
    fs::path root(meta_.root_directory);

    if (calib.verified) {
        calibration::TargetlessCalibrator::saveProfileToFile(calib, (root / "calibration.cfg").string());
    }

    // Rewrite WAL with latest globally optimized poses and ensure every submap chunk is on disk
    std::ofstream wal((root / "session.wal").string(), std::ios::binary | std::ios::trunc);
    if (!wal.is_open()) return false;
    uint32_t magic = 0x4156574Cu;
    wal.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

    for (const auto& sm : submaps) {
        if (!sm.local_points.empty()) {
            core::KeyframeSubmap tmp = sm;
            archiveSubmapToDisk(tmp);
        }
        math::Quatd q = sm.T_world_keyframe.so3.unit_quaternion();
        const math::Vec3d& t = sm.T_world_keyframe.t;
        double pose_vec[7] = {q.x, q.y, q.z, q.w, t.x, t.y, t.z};
        wal.write(reinterpret_cast<const char*>(&sm.keyframe_id), sizeof(sm.keyframe_id));
        wal.write(reinterpret_cast<const char*>(&sm.timestamp_sec), sizeof(sm.timestamp_sec));
        wal.write(reinterpret_cast<const char*>(pose_vec), sizeof(pose_vec));
    }
    wal.close();

    // Save factors
    std::ofstream fac_out((root / "pose_graph.factors").string(), std::ios::binary | std::ios::trunc);
    uint32_t num_fac = static_cast<uint32_t>(factors.size());
    fac_out.write(reinterpret_cast<const char*>(&num_fac), sizeof(num_fac));
    size_t loops = 0;
    for (const auto& f : factors) {
        if (f.type == backend::FactorType::LOOP_CLOSURE) loops++;
        fac_out.write(reinterpret_cast<const char*>(&f), sizeof(backend::PoseGraphFactor));
    }
    fac_out.close();

    // Save POI photos
    std::ofstream poi_out((root / "poi_photos.txt").string(), std::ios::trunc);
    for (const auto& p : photos) {
        math::Quatd q = p.T_world_camera.so3.unit_quaternion();
        const math::Vec3d& t = p.T_world_camera.t;
        poi_out << p.photo_id << "|" << p.timestamp_sec << "|"
                << q.x << "|" << q.y << "|" << q.z << "|" << q.w << "|"
                << t.x << "|" << t.y << "|" << t.z << "|"
                << p.image_file_path << "|" << p.note << "\n";
    }
    poi_out.close();

    meta_.keyframe_count = submaps.size();
    meta_.loop_closure_count = loops;
    meta_.clean_shutdown = true;

    std::ofstream meta_out((root / "project.meta").string(), std::ios::trunc);
    meta_out << "project_id=" << meta_.project_id << "\n";
    meta_out << "project_name=" << meta_.project_name << "\n";
    meta_out << "keyframe_count=" << meta_.keyframe_count << "\n";
    meta_out << "loop_closure_count=" << meta_.loop_closure_count << "\n";
    meta_out << "clean_shutdown=1\n";
    return meta_out.good();
}

bool ProjectStore::recoverOrLoadSession(const std::string& root_dir,
                                        ProjectMetadata& out_meta,
                                        core::ExtrinsicCalibrationProfile& out_calib,
                                        std::vector<core::KeyframeSubmap>& out_submaps,
                                        std::vector<backend::PoseGraphFactor>& out_factors,
                                        std::vector<PoiPhotoRecord>& out_photos,
                                        bool& out_recovered_from_crash) {
    out_submaps.clear();
    out_factors.clear();
    out_photos.clear();
    out_recovered_from_crash = false;

    fs::path root(root_dir);
    if (!fs::exists(root / "project.meta")) return false;

    meta_.root_directory = root_dir;
    meta_.clean_shutdown = false;
    std::ifstream meta_in((root / "project.meta").string());
    std::string line;
    while (std::getline(meta_in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "project_id") meta_.project_id = v;
        else if (k == "project_name") meta_.project_name = v;
        else if (k == "clean_shutdown") meta_.clean_shutdown = (std::stoi(v) != 0);
    }
    out_recovered_from_crash = !meta_.clean_shutdown;

    if (fs::exists(root / "calibration.cfg")) {
        calibration::TargetlessCalibrator::loadProfileFromFile((root / "calibration.cfg").string(), out_calib);
    }

    // Read WAL keyframes and reload their binary submap chunks
    std::ifstream wal((root / "session.wal").string(), std::ios::binary);
    if (!wal.is_open()) return false;
    uint32_t magic = 0;
    wal.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!wal.good() || magic != 0x4156574Cu) return false;

    while (wal.peek() != EOF) {
        core::KeyframeSubmap sm;
        double pose_vec[7]{};
        wal.read(reinterpret_cast<char*>(&sm.keyframe_id), sizeof(sm.keyframe_id));
        if (wal.eof()) break;
        wal.read(reinterpret_cast<char*>(&sm.timestamp_sec), sizeof(sm.timestamp_sec));
        wal.read(reinterpret_cast<char*>(pose_vec), sizeof(pose_vec));
        if (!wal.good()) break;

        math::Quatd q(pose_vec[0], pose_vec[1], pose_vec[2], pose_vec[3]);
        q.normalize();
        sm.T_world_keyframe = math::SE3d(q, math::Vec3d(pose_vec[4], pose_vec[5], pose_vec[6]));
        sm.disk_chunk_path = (root / "chunks" / ("submap_" + std::to_string(sm.keyframe_id) + ".bin")).string();
        if (loadSubmapFromDisk(sm)) {
            out_submaps.push_back(std::move(sm));
        }
    }

    if (fs::exists(root / "pose_graph.factors")) {
        std::ifstream fac_in((root / "pose_graph.factors").string(), std::ios::binary);
        uint32_t num_fac = 0;
        fac_in.read(reinterpret_cast<char*>(&num_fac), sizeof(num_fac));
        if (fac_in.good() && num_fac < 1000000u) {
            out_factors.resize(num_fac);
            for (uint32_t i = 0; i < num_fac; ++i) {
                fac_in.read(reinterpret_cast<char*>(&out_factors[i]), sizeof(backend::PoseGraphFactor));
            }
        }
    }

    meta_.keyframe_count = out_submaps.size();
    out_meta = meta_;
    return !out_submaps.empty();
}

}  // namespace storage
}  // namespace avscan
