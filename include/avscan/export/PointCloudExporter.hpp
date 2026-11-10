#pragma once

#include "avscan/core/Types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace avscan {
namespace exporters {

class PointCloudExporter {
public:
    // Exports colorized point cloud to Stanford PLY format (binary_little_endian or ascii)
    static bool exportToPly(const std::string& file_path,
                            const std::vector<core::LidarPointXYZIRT>& points,
                            bool binary_format = true);

    // Reads back a PLY file to verify interoperability
    static bool readFromPly(const std::string& file_path,
                            std::vector<core::LidarPointXYZIRT>& out_points);

    // Exports colorized point cloud to PCL PCD v0.7 format (binary or ascii)
    static bool exportToPcd(const std::string& file_path,
                            const std::vector<core::LidarPointXYZIRT>& points,
                            bool binary_format = true);

    // Reads back a PCD v0.7 file to verify interoperability
    static bool readFromPcd(const std::string& file_path,
                            std::vector<core::LidarPointXYZIRT>& out_points);

    // Exports colorized point cloud to ASTM E2807-11 E57 format (paged 1020+4 byte CRC32C stream + CompressedVector + XML)
    static bool exportToE57(const std::string& file_path,
                            const std::vector<core::LidarPointXYZIRT>& points,
                            const std::string& scan_guid = "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
                            const std::string& scan_name = "AVScan_Fused_Cloud");

    // Reads and validates an ASTM E2807-11 E57 file (verifying CRC32C page checksums, CompressedVector records, and XML)
    static bool readFromE57(const std::string& file_path,
                            std::vector<core::LidarPointXYZIRT>& out_points);

    // Computes Castagnoli CRC-32C (polynomial 0x1EDC6F41, reflected 0x82F63B78) used by ASTM E2807-11 E57 pages
    static uint32_t computeE57Crc32c(const uint8_t* data, size_t len);
};

}  // namespace exporters
}  // namespace avscan
