# AV Scan iOS — Handheld 3D LiDAR Mapping & SLAM System

[![Milestones](https://img.shields.io/badge/Milestones-M0--M12%20Complete-brightgreen)](#milestone-status)
[![Hardware](https://img.shields.io/badge/LiDAR-RoboSense%20Airy-blue)](#hardware-interconnect)
[![Platform](https://img.shields.io/badge/Platform-iOS%20%2F%20iPadOS%20(arm64)-orange)](#platform)
[![License](https://img.shields.io/badge/License-Clean--Room%20BSD%20%2F%20MIT-lightgrey)](docs/LICENSE_AUDIT.md)

**AV Scan iOS** is a commercial-grade, real-time handheld 3D LiDAR mapping, odometry, and SLAM system engineered for Apple iPad (10th Gen A2696 / iPad Pro) and USB-C iPhones paired with the **RoboSense Airy** 96-beam hemispherical 3D LiDAR and integrated 6-axis IMU.

---

## Architecture Overview

```
+-----------------------------------------------------------------------------------+
|                              RoboSense Airy 3D LiDAR                               |
|        96-Beam Hemispherical Scanner (360° x 90° FOV) + 6-Axis IMU (200 Hz)       |
|                                IP: 192.168.1.200                                  |
+-----------------------------------------------------------------------------------+
                                         │
                 Custom Shorty USB-C to DC Barrel Power Cable
                                         │
                        Gigabit Ethernet (Cat6)
                                         │
                                         ▼
+-----------------------------------------------------------------------------------+
|              Belkin USB-C to Gigabit Ethernet + Power Delivery Adapter             |
+-----------------------------------------------------------------------------------+
                                         │
                                    USB-C Data
                                         │
                                         ▼
+-----------------------------------------------------------------------------------+
|                        Apple iPad / iPhone (iOS 17+ arm64)                         |
|                                 IP: 192.168.1.102                                 |
+-----------------------------------------------------------------------------------+
|  1. POSIX BSD Sockets: MSOP (6699), DIFOP (7788), IMU (6688)                     |
|  2. Binary Stream Capture: Zero-copy Ring Buffer & .avraw WAL Persistence         |
|  3. Clean-Room C++17 Odometry: 18-DOF ES-IEKF + Direct Visual-Inertial Fusion     |
|  4. Incremental Voxel Plane Map (ikd-Tree)                                        |
|  5. Loop Closure: Scan Context Descriptors + GTSAM Pose-Graph Optimizer          |
|  6. Native Metal 3D Renderer: Real-Time Point Cloud & Trajectory Frustum Pipeline  |
|  7. Multi-Format Exporters: Binary PLY, PCD, ASTM E57                              |
+-----------------------------------------------------------------------------------+
```

---

## Key Subsystems

- **Native Sensor Telemetry & Ingestion**: High-throughput asynchronous UDP socket engine receiving 96-beam MSOP packets (UDP `6699`), high-rate 200 Hz IMU packets (UDP `6688`), and DIFOP calibration packets (UDP `7788`).
- **Clean-Room FAST-LIVO2 Estimator (`ISlamEstimator`)**: Direct visual-inertial-LiDAR fusion combining an 18-DOF Error-State Iterated Extended Kalman Filter with direct photometric tracking on live camera frames and LiDAR point-to-plane residual minimization.
- **Metal 3D Real-Time Renderer**: Hardware-accelerated Apple Metal rendering engine supporting continuous point cloud streaming, GPU frustum culling, dynamic point decimation, and elevation/reflectance color mapping.
- **Loop Closure & Global Optimization**: Real-time Scan Context keyframe generation coupled with multi-threaded Generalized ICP (GICP) and GTSAM factor graph optimization.
- **Fail-Safe `.avraw` Storage Engine**: Crash-recovery Write-Ahead Logging (WAL) and memory-mapped file persistence ensuring scans are preserved even across sudden power disconnects.
- **Cross-Platform Windows Toolchain**: Native Windows-based compilation and ad-hoc IPA code-signing via embedded `zsign`.

---

## Hardware Interconnect & Networking Configuration

| Parameter | Configuration Value |
| :--- | :--- |
| **Mobile Host IP** (iPad / iPhone) | `192.168.1.102` (Subnet: `255.255.255.0`, Gateway: `192.168.1.1`) |
| **LiDAR Sensor IP** (RoboSense Airy) | `192.168.1.200` (Subnet: `255.255.255.0`, Gateway: `192.168.1.1`) |
| **MSOP Port (LiDAR Point Cloud)** | UDP `6699` (~1248 byte packets) |
| **IMU Stream Port** | UDP `6688` (200 Hz, ~51 byte packets) |
| **DIFOP Port (Diagnostics/Config)** | UDP `7788` |
| **Web Diagnostic Portal** | `http://192.168.1.200/` |
| **Physical Adapter** | Belkin USB-C to Gigabit Ethernet + Charge Adapter |
| **Power Delivery** | Custom USB-C to DC barrel jack cable (supports power-bank operation) |

---

## Repository Structure

```
AV_iOS/
├── CMakeLists.txt                 # Unified C++17 / iOS CMake build definition
├── MILESTONE_STATUS.md            # Detailed implementation audit for Milestones M0–M12
├── EXCLUSIONS.md                  # Comprehensive guide for all files excluded from git
├── README.md                      # Project architecture and developer guide
├── cmake/                         # iOS toolchain and compiler flag definitions
├── include/                       # C++ header files and abstract interfaces
│   ├── av_raw_writer.h            # Binary .avraw packet serializer and WAL manager
│   ├── camera_bridge.h            # AVFoundation camera capture interface
│   ├── loop_closure.h             # Scan Context and GICP pose graph optimization
│   ├── metal_bridge.h             # Metal 3D point cloud renderer interface
│   ├── robosense_decoder.h        # RoboSense Airy MSOP/IMU/DIFOP packet decoder
│   └── slam_estimator.h           # ISlamEstimator clean-room interface
├── ios/                           # Native Objective-C++ Metal and UI bridges
│   ├── AVScanMetalRenderer.mm     # Metal shader pipeline implementation
│   ├── AVScanViewController.mm    # Main scanning UI controller
│   └── Shaders.metal              # Metal Shading Language (MSL) point cloud shaders
├── src/                           # Core C++17 implementations
│   ├── av_raw_writer.cpp
│   ├── camera_bridge.cpp
│   ├── loop_closure.cpp
│   ├── robosense_decoder.cpp
│   └── slam_estimator.cpp
├── scripts/                       # Build automation, signing, and audit scripts
│   ├── export_antigravity_history.py
│   └── package_ipa.ps1
├── tests/                         # Unit and integration test suites
│   ├── test_avraw.cpp
│   ├── test_decoder.cpp
│   └── test_slam.cpp
├── zsign_src/                     # Native Mach-O code-signing utility source
└── docs/
    ├── LICENSE_AUDIT.md           # Legal and commercial license audit
    └── antigravity-history/       # Complete Antigravity session transcripts & ADRs
        ├── 00_INDEX.md
        ├── 01_SESSION_0_IOS_BUILD_M0_M12.md
        ├── 02_SESSION_1_AIRY_LIDAR_CONNECTION_TEST.md
        ├── 03_ARCHITECTURAL_DECISIONS.md
        └── 04_MILESTONES_M0_M12_REFERENCE.md
```

---

## Documentation & Knowledge Base

- **[Antigravity Session History & ADRs](docs/antigravity-history/00_INDEX.md)**: Full transcripts, decisions, and network traces.
- **[Milestone Status Audit](MILESTONE_STATUS.md)**: Detailed breakdown of Milestones `M0` through `M12`.
- **[Exclusions & Local Reconstruction Guide](EXCLUSIONS.md)**: Step-by-step instructions to rebuild local binaries and OpenSSL dependencies.
- **[License Audit](docs/LICENSE_AUDIT.md)**: Component-by-component open-source and commercial licensing audit.
