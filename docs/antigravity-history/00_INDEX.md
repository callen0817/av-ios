# AV Scan iOS — Antigravity Engineering History & Knowledge Base

This directory documents the complete architectural evolution, milestone implementation records, hardware validation procedures, and session transcripts conducted with Antigravity during the development of **AV Scan iOS**.

---

## Document Index

| Document | Description | Key Topics |
| :--- | :--- | :--- |
| **[`01_SESSION_0_IOS_BUILD_M0_M12.md`](./01_SESSION_0_IOS_BUILD_M0_M12.md)** | **Full Session Transcript (M0–M12)** | Architectural plan, FAST-LIVO2 clean-room porting, Windows-to-iOS toolchain, native Metal 3D renderer, camera-LiDAR fusion, Scan Context loop closure, binary `.avraw` recording, and unit test audits. |
| **[`02_SESSION_1_AIRY_LIDAR_CONNECTION_TEST.md`](./02_SESSION_1_AIRY_LIDAR_CONNECTION_TEST.md)** | **Full Session Transcript (Hardware & Network)** | Real-world hardware bringup: passive packet capture (`pktmon`), IP discovery (`192.168.1.200` $\leftrightarrow$ `192.168.1.102`), static subnet configuration, Web UI verification, and custom shorty USB-C barrel power cable validation on iPad/iPhone. |
| **[`03_ARCHITECTURAL_DECISIONS.md`](./03_ARCHITECTURAL_DECISIONS.md)** | **Consolidated Architectural Decision Records (ADRs)** | SLAM algorithm trade-offs, GPL license isolation, hardware interconnect choices, cross-compilation pipeline, and live streaming memory models. |
| **[`04_MILESTONES_M0_M12_REFERENCE.md`](./04_MILESTONES_M0_M12_REFERENCE.md)** | **Milestone Verification & Status Reference** | Comprehensive audit matrix of Milestones `M0` through `M12` with hardware test gates and automated test coverage. |

---

## Hardware Context & Verification Baseline

- **Mobile Host**: iPad A2696 (10th Generation, Apple A14 Bionic, USB-C) / USB-C iPhone 15/16 series
- **LiDAR Sensor**: RoboSense Airy (96-beam hemispherical 3D LiDAR, integrated 6-axis IMU)
- **Power Delivery**: Custom shorty USB-C to DC barrel jack cable (powers LiDAR directly from host/power bank without bulky AC bricks)
- **Data Interconnect**: Belkin USB-C to Gigabit Ethernet + Charge Adapter
  - **Sensor IP**: `192.168.1.200/24` (Gateway: `192.168.1.1`)
  - **Host IP**: `192.168.1.102/24` (Gateway: `192.168.1.1`)
  - **MSOP (LiDAR Point Cloud)**: UDP Port `6699` (~1248 byte packets)
  - **IMU Stream**: UDP Port `6688` (200 Hz, ~51 byte packets)
  - **DIFOP (Device Information)**: UDP Port `7788`
  - **Web Diagnostic Interface**: `http://192.168.1.200/`
