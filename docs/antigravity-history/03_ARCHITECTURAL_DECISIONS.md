# Architectural Decisions Record (ADR) — AV Scan iOS

This document consolidates the key engineering, algorithm, and hardware architectural decisions made across the Antigravity sessions.

---

## ADR 1: SLAM Engine — FAST-LIVO2 Clean-Room Port vs. Alternative Odometry

### Context
Commercial handheld 3D scanning requires real-time odometry with robust operation in geometrically degraded environments (e.g. long corridors, outdoor open fields) and rapid movement. The system utilizes a RoboSense Airy 96-beam hemispherical LiDAR with an integrated 6-axis IMU and host mobile cameras.

### Decision
1. Selected **FAST-LIVO2** as the mathematical and algorithmic baseline. FAST-LIVO2 provides tightly coupled visual-inertial-LiDAR state estimation via an 18-DOF Iterated Extended Kalman Filter (ES-IEKF), direct photometric error minimization (avoiding unstable feature tracking in dark or low-texture areas), and an incremental voxel map (`ikd-Tree`).
2. Implemented the estimator in **clean-room C++17** behind the abstract interface `ISlamEstimator`.
3. Added Scan Context loop closure and GTSAM pose-graph optimization on the back-end to eliminate accumulated drift on multi-minute loops.

### License Isolation
Original research implementations of FAST-LIVO2 are released under GPLv2. To preserve commercial closed-source distribution on the Apple iOS App Store, all algorithms are implemented in clean-room C++17 with mathematical equivalence to the published academic papers, completely decoupled from GPL-licensed codebases. See [`docs/LICENSE_AUDIT.md`](../LICENSE_AUDIT.md) for full legal audit.

---

## ADR 2: Hardware Interconnect & Networking Model

### Context
Mobile devices lack native RJ45 ports. The RoboSense Airy broadcasts massive volumes of UDP traffic (LiDAR MSOP at port 6699, IMU at port 6688, and DIFOP at port 7788).

### Decision
1. **Belkin USB-C to Gigabit Ethernet + Charge Adapter**: Provides low-latency, deterministic gigabit Ethernet throughput while simultaneously supplying Power Delivery (PD) to the mobile device.
2. **Deterministic Static IPv4 Addressing**:
   - Host (iPhone/iPad): `192.168.1.102`, Subnet `255.255.255.0`, Gateway `192.168.1.1`
   - LiDAR (Airy): `192.168.1.200`, Subnet `255.255.255.0`, Gateway `192.168.1.1`
3. **Gateway Specification on iOS**: Adding `192.168.1.1` as the gateway on iOS creates the default route required for iOS to resolve local ARP and allow ICMP ping / HTTP diagnostic queries to `http://192.168.1.200/`.
4. **Power System**: Replaced the bulky AC power supply with a custom shorty USB-C to DC barrel jack cable that powers the RoboSense Airy directly from high-output USB-C PD power banks.

---

## ADR 3: Recording & File Serialization Format (`.avraw`)

### Context
Raw packet capture during high-speed scanning can easily overwhelm device flash memory or stall the odometry thread if not buffered efficiently.

### Decision
1. Implemented a zero-copy circular ring buffer between the POSIX/BSD UDP socket receiver and disk writers.
2. Formatted data using **`.avraw` (AV Raw Binary Stream)**:
   - File header: 64-byte magic identifier, sensor hardware ID, firmware version, and epoch timestamp.
   - Packet framing: 16-byte packet header containing hardware nanosecond timestamp, stream identifier (MSOP=0x01, IMU=0x02, DIFOP=0x03, CAM=0x04), payload length, and CRC-32 checksum.
   - Crash Recovery WAL (Write-Ahead Logging) ensures that partial scans are preserved even if the host application is interrupted.

---

## ADR 4: Rendering Architecture — Native Apple Metal 3D Pipeline

### Context
Visualizing tens of millions of raw 3D point cloud vertices in real time on an iPad A14 Bionic / iPhone requires minimizing CPU-to-GPU data transfer overhead.

### Decision
1. Native Metal shader pipeline written in Objective-C++ / Metal Shading Language (`.metal`).
2. Point cloud vertex buffers allocated with `MTLResourceStorageModeShared` to enable zero-copy GPU access from the C++ voxel mapping thread.
3. Implemented level-of-detail (LOD) frustum culling and reflectance / height ramp color mapping in GPU vertex shaders.

---

## ADR 5: Cross-Platform Build & Ad-Hoc Signing on Windows

### Context
The development environment is Windows 11, while the deployment target is native Apple iOS (`arm64`).

### Decision
1. CMake cross-compilation toolchain targeting `arm64-apple-darwin` or `arm64-apple-ios`.
2. Native compilation of `zsign` (C++ Mach-O binary and IPA re-signer) directly on Windows using MSBuild / Visual Studio 2022.
3. Automated provisioning profile and ad-hoc certificate embedding to allow direct installation on connected test iPads without requiring a macOS host for routine builds.
