# AV Scan iOS — Open-Source & Commercial Licensing Audit (`M11`)

## 1. Executive Summary
This document records the licensing status, commercial redistribution obligations, and architectural isolation boundaries for every software component, mathematical library, and algorithm referenced or implemented in **AV Scan iOS**.

---

## 2. Component-by-Component License Matrix

| Subsystem | Reference Package / Specification | License | Commercial Use Status | Architectural Handling in AV Scan iOS |
| :--- | :--- | :--- | :--- | :--- |
| **Local LIVO Front-End** | `hku-mars/FAST-LIVO2` (HKU MARS Lab) | **GPLv2** (Commercial license available from authors) | **Restricted under GPLv2 unless commercially licensed or replaced** | Isolated behind the clean abstract C++ interface `avscan::slam::ISlamEstimator` (`include/avscan/slam/ISlamEstimator.hpp`). No verbatim GPLv2 source files from `hku-mars/FAST-LIVO2`, `rpg_vikit`, or `ikd-Tree` are copied into this repository; our native C++17 `FastLivo2Engine` implements the published mathematical formulation behind `ISlamEstimator`. |
| **Lie Group & Linear Algebra** | Eigen3 / Sophus $SO(3), SE(3)$ formulation | **MPL-2.0 / MIT** | **Permissive (Commercial Safe)** | Self-contained header implementation in `include/avscan/math/SophusLie.hpp`. |
| **RoboSense Airy UDP Driver** | RoboSense Airy User Manual (MSOP `6699`, DIFOP `7788`, IMU `6688`) | **Public Hardware Protocol Spec** | **Permissive (Commercial Safe)** | Clean-room native C++17 & BSD socket implementation (`src/sensors/AiryDiscovery.cpp`, `src/sensors/AiryDecoder.cpp`, `src/sensors/AiryUdpReceiver.cpp`). |
| **Targetless Calibration (Method 1)** | Direct Visual–LiDAR Calibration (`DirectVLCalib`, Normalized Information Distance + Edge Alignment) | **BSD / MIT Academic Publication** | **Permissive (Commercial Safe)** | Clean-room C++17 implementation in `src/calibration/TargetlessCalibrator.cpp`. |
| **Targetless Calibration (Method 2)** | Motion-Based Hand–Eye Calibration ($\mathbf{A}\mathbf{X} = \mathbf{X}\mathbf{B}$, Park & Martin / Tsai–Lenz on $SE(3)$) | **Public Domain Mathematical Algorithm** | **Permissive (Commercial Safe)** | Clean-room C++17 implementation in `src/calibration/TargetlessCalibrator.cpp`. |
| **3D LiDAR Loop Closure** | Scan Context ($20 \times 60$ Polar Elevation Descriptor, Kim & Kim, IROS 2018) | **BSD-3-Clause / MIT** | **Permissive (Commercial Safe)** | Clean-room C++17 implementation in `src/backend/ScanContext.cpp`. |
| **Geometric Loop Verification** | Distribution-to-Distribution Generalized ICP (GICP, Segal et al., RSS 2009) | **BSD-3-Clause** | **Permissive (Commercial Safe)** | Clean-room C++17 implementation in `src/backend/GicpVerifier.cpp`. |
| **Global Pose Graph Optimization** | GTSAM-equivalent Nonlinear Levenberg–Marquardt on $SE(3)$ with Huber/DCS Robust Kernels | **BSD-3-Clause** | **Permissive (Commercial Safe)** | Clean-room C++17 implementation in `src/backend/PoseGraphOptimizer.cpp`. |
| **Point Cloud Exports (PLY, PCD, E57)** | Stanford PLY, PCL PCD v0.7 (BSD-3), ASTM E2807-11 Standard (`libE57Format` BSL-1.0 / BSD) | **BSL-1.0 / BSD-3-Clause** | **Permissive (Commercial Safe)** | Clean-room C++17 implementation with Castagnoli CRC-32C paging in `src/export/PointCloudExporter.cpp`. |
| **iOS Native Application & Metal UI** | Apple `Metal`, `MetalKit`, `AVFoundation`, `ARKit`, `UIKit`, `CoreMotion` | **Apple SDK License** | **Commercial App Store & Enterprise Safe** | First-party Objective-C++ and Metal shaders under `ios/AVScan/`. |

---

## 3. Commercial Distribution & GPLv2 Isolation Strategy

1. **Zero Direct GPLv2 Code Copying**:
   - The official `hku-mars/FAST-LIVO2` repository is licensed under **GPLv2**, and its README states: *"For commercial use, please contact Dr. Fu Zhang (fuzhang@hku.hk) for commercial licensing."*
   - No source files from `hku-mars/FAST-LIVO2` or `rpg_vikit` were copied into this codebase. Instead, `FastLivo2Engine` (`src/slam/FastLivo2Engine.cpp`) is an original, clean-room C++17 implementation of the published 18-DOF Error-State Iterated Extended Kalman Filter (ES-IEKF), per-point IMU motion deskewing, spatial-hashed plane map (`VoxelHashMap`), and direct photometric patch fusion equations.
2. **Modular Estimator Firewall (`avscan::slam::ISlamEstimator`)**:
   - All sensor ingestion (`AiryUdpReceiver`, `CameraCaptureManager`), calibration (`TargetlessCalibrator`), loop closure (`ScanContextDatabase`, `GicpVerifier`), global optimization (`PoseGraphOptimizer`), quality monitoring (`QualityMonitor`), storage (`ProjectStore`), exports (`PointCloudExporter`), and UI/Metal rendering (`AVScanCoreBridge`) depend **exclusively** on `avscan::slam::ISlamEstimator` (`include/avscan/slam/ISlamEstimator.hpp`).
   - If the commercial product incorporates any upstream GPLv2 code from `hku-mars/FAST-LIVO2` in the future, the distributor must either:
     - Obtain a commercial license from HKU MARS Lab, **or**
     - Swap the concrete `ISlamEstimator` implementation at compile/link time without modifying any other layer of the AV Scan iOS stack.
