# AV Scan iOS — Milestones M0–M12 Engineering Reference

For the live implementation and verification status file in the project root, see [`MILESTONE_STATUS.md`](../../MILESTONE_STATUS.md).

---

## Milestone Verification Matrix

| Milestone | Subsystem | Implementation Deliverables | Verification Strategy & Gates |
| :--- | :--- | :--- | :--- |
| **`M0`** | Windows-to-iOS Toolchain & Signing | CMake cross-toolchain, `zsign.exe` build, IPA packaging scripts | Automated Toolchain Audit (`PASS`), IPA Sign on Host (`BLOCKED` awaiting `iPhoneOS.sdk`) |
| **`M1`** | Airy Ethernet Detection & Diagnostics | POSIX socket layer, IP auto-configuration, ARP resolution | **`HARDWARE VALIDATED (PASS — Gate 1)`**: `192.168.1.200` $\leftrightarrow$ `192.168.1.102` verified on Windows, iPad & iPhone |
| **`M2`** | UDP Reception, Decoding & `.avraw` Stream | MSOP/IMU/DIFOP decoders, CRC-32 validation, circular ring buffer | **`HARDWARE CAPTURE INGESTED & VERIFIED`**: Real-world packets decoded from `air_capture2.etl` / `.pcapng` |
| **`M3`** | Live Metal 3D Point-Cloud Renderer | Metal pipeline, point-cloud vertex buffer, camera trajectory frustum | Automated Structural Pass + **Gate 2** Hardware Rendering Validation |
| **`M4`** | FAST-LIVO2 C++17 Front-End & Voxel Map | 18-DOF ES-IEKF, motion deskewing, `ikd-Tree` voxel plane representation | Automated Mathematical Pass (<0.02m synthetic drift error) |
| **`M5`** | Camera Pipeline & Clock Synchronization | AVFoundation capture bridge, PTP / hardware timestamp regression | Automated Offset Recovery (<1 ms synthetic clock offset) |
| **`M6`** | Visual-Inertial-LiDAR Fusion & Colorization | Direct photometric error minimization, point cloud colorization | Automated Unit Pass + **Gate 3** Hardware Visual Tracking |
| **`M7`** | Targetless Extrinsic & Temporal Calibration | `DirectVLCalib` continuous estimator, `AX=XB` hand-eye solver | Synthetic Recovery (<0.01° rotation / <0.1 mm translation error) |
| **`M8`** | Loop Closure, GICP & Pose Graph Optimization | Scan Context keyframe descriptors, multi-threaded GICP, GTSAM backend | Loop Drift Reduction (>95% error reduction on synthetic trajectory) |
| **`M9`** | Quality Monitoring & 2D Floor Plan | Covariance conditioning, degenerate direction detection, slice extraction | Automated Pass for real-time operator alerts and occupancy grids |
| **`M10`** | AV Scan Workflow, WAL Recovery & Exporters | Project directory manifest, WAL persistence, PLY/PCD/E57 binary writers | Verified against `Open3D 0.19`, `plyfile 1.1.2`, and `pye57 0.4.18` |
| **`M11`** | Commercial Hardening, Thermal & RAM Budgets | Frame rate throttling, dynamic point decimation, licensing isolation | Audited and verified in [`docs/LICENSE_AUDIT.md`](../LICENSE_AUDIT.md) |
| **`M12`** | Final Integrated Hardware Validation | Full end-to-end integration test harness | **Gate 4** (Ready for Live Walking Scan on Target iPad) |
