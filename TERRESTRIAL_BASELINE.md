# TERRESTRIAL_BASELINE.md
# Artificial Vision — Unified Terrestrial Scanning Implementation Baseline

**Date**: 2026-11-17  
**Author**: Antigravity Orchestrator  
**Status**: ACTIVE — Milestone T0 Architecture & Baseline Completed  

---

## 1. Executive Summary & Capability Matrix

The objective of this initiative is to establish a unified native **Terrestrial Scanning Mode** across the three Artificial Vision scanning applications:
1. **AV scanAR** (`https://github.com/callen0817/av-scanar`)
2. **AV Scan iOS** (Local workspace: `c:\Users\calle\OneDrive\Desktop\AV_iOS`)
3. **AV Scan** (`https://github.com/callen0817/av-scan`)

All three applications will retain their full **Mobile SLAM Mode** (Mode A) intact, while introducing the discrete **Terrestrial Station Mode** (Mode B). In Terrestrial Mode, the scanner is positioned at a fixed physical tripod station, rotated through $360^\circ$ in nominal discrete angular increments (e.g. $45^\circ$, 8 stops), and accumulates stationary high-density LiDAR point clouds and optional photographic images into a single station coordinate origin.

### Application Capability Matrix

| Feature / Subsystem | AV scanAR | AV Scan iOS | AV Scan (Desktop) |
| :--- | :--- | :--- | :--- |
| **Compute Host** | NVIDIA Jetson Orin Nano / J4012 | Apple iPad (Air 4 / 10th Gen / M-Series) / iPhone | Desktop x86_64 / ARM64 workstation |
| **Primary 3D Sensor** | RoboSense Airy (96-beam hemispherical) | RoboSense Airy (96-beam hemispherical) | Configurable / RoboSense Airy |
| **LiDAR Mechanical Mount** | Rigid bracket, approx $45^\circ$ pitch | Rigid smallrig/clamp, $90^\circ$ pitch (forward-facing, cable down) | Configurable / Tripod |
| **RGB Camera** | Orbbec Gemini 336L (Stereo/RGB) | Apple iPad / iPhone native wide-angle camera | External USB / Industrial camera (optional) |
| **IMU Telemetry** | Airy internal 6-axis IMU (200 Hz) | Airy internal 6-axis IMU (200 Hz) + iOS CoreMotion | Airy IMU / Host IMU |
| **Motorized Tripod Support** | Single-axis turntable (Genie Mini / ZEAPON / Manual) | Single-axis turntable (Genie Mini / ZEAPON / Manual) | Single-axis turntable / Manual indexer |
| **Station Coordinate Frame** | Local Station Axis: $Z$ Up, $X$ Forward at $0^\circ$ | Local Station Axis: $Z$ Up, $X$ Forward at $0^\circ$ | Local Station Axis: $Z$ Up, $X$ Forward at $0^\circ$ |
| **Photographic Panorama** | Gemini 336L stitched cylindrical/equirectangular | iOS camera stitched cylindrical/equirectangular | Camera panorama or LiDAR reflectivity panorama |
| **Output Deliverables** | Structured E57, PLY/PCD, `lod0.bin`, `station_manifest.json`, `project.avproj` | Structured E57, PLY/PCD, `lod0.bin`, `station_manifest.json`, `project.avproj` | Structured E57, PLY/PCD, `lod0.bin`, `station_manifest.json`, `project.avproj` |

---

## 2. Universal Station Mathematical Geometry

Each physical scan station establishes its own local coordinate system at the intersection of the tripod rotation axis and the mounting flange:
- **$+Z_{\text{station}}$**: Aligned with the gravity-upright rotation axis.
- **$+X_{\text{station}}$**: Direction of the sensor reference orientation at $\theta = 0^\circ$.
- **$+Y_{\text{station}}$**: Completes right-handed orthogonal system ($+Y = +Z \times +X$, facing $90^\circ$ counter-clockwise / left).

For any angular stop $\theta_i \in [0^\circ, 360^\circ)$ on the tripod head, the transform from the LiDAR sensor optical frame to the stationary physical station frame is:

$$P_{\text{station}} = T_{\text{station\_axis}} \cdot R_z(\theta_i) \cdot T_{\text{axis\_body}} \cdot T_{\text{body\_lidar}} \cdot P_{\text{lidar}}$$

Where:
- $T_{\text{body\_lidar}}$: Mechanical sensor mounting transform (incorporates the $90^\circ$ pitch on iOS or $45^\circ$ pitch on Jetson scanAR).
- $T_{\text{axis\_body}}$: Physical offset between the device mounting screw/clamp center and the scanner body center.
- $R_z(\theta_i) \in SO(3)$: Rotation around the tripod rotation axis by angle $\theta_i$.
- $T_{\text{station\_axis}}$: Station leveling / reference orientation relative to the station origin.

### Motion Guard & Stillness Verification (Airy IMU at 200 Hz)
To prevent angular motion blur during manual or motorized indexing without requiring complex hardware synchronization, the `TerrestrialCaptureController` uses the Airy's internal IMU telemetry:
- Angular velocity magnitude $\omega = \sqrt{\omega_x^2 + \omega_y^2 + \omega_z^2}$.
- Stationary threshold: $\text{std}(\omega) < 0.05\text{ rad/s}$ and $|\omega| < 0.08\text{ rad/s}$ sustained for $\ge 400\text{ ms}$.
- If movement is detected during accumulation, the segment is paused/rejected until the tripod settles.

---

## 3. Station Directory Hierarchy & Output Contract

Every terrestrial scan station produces an identical folder structure:

```
<project_root>/
├── project.avproj                     # Unified container manifest
├── stations/
│   ├── st_001/
│   │   ├── station_manifest.json      # Station v1.0 canonical metadata
│   │   ├── pointcloud.ply             # Merged station point cloud
│   │   ├── station.e57                # ASTM E2807 structured/station E57
│   │   ├── cache/
│   │   │   └── lod0.bin               # 15-byte packed binary point cloud
│   │   ├── pano/
│   │   │   ├── pano_rgb.jpg           # Cylindrical / equirectangular RGB panorama
│   │   │   └── coverage_mask.png      # Usable field-of-view coverage mask
│   │   └── raw/
│   │       ├── stop_000_000deg.avraw  # Raw LiDAR/IMU packets at 0 deg
│   │       ├── stop_001_045deg.avraw  # Raw LiDAR/IMU packets at 45 deg
│   │       ├── ...
│   │       └── images/                # Original unstitched camera frames
│   └── st_002/
└── scans/                             # Mobile SLAM sessions (Mode A - preserved)
```

---

## 4. Universal `station_manifest.json` Specification (v1.0)

```json
{
  "$schema": "https://artificialvision.ai/schemas/station_manifest_v1.0.json",
  "schema_version": "1.0",
  "project_id": "PROJECT_001",
  "station_id": "st_001",
  "name": "Station 01 - Main Entrance",
  "mode": "terrestrial",
  "created_utc": "2026-11-17T18:58:00Z",
  "hardware_profile": {
    "app": "AV Scan iOS",
    "app_version": "1.0.0",
    "host_device": "iPad 10th Gen (A2696)",
    "sensor_model": "RoboSense Airy",
    "sensor_serial": "08:48:57:09:82:89",
    "camera_model": "Apple Wide 12MP"
  },
  "coordinate_system": {
    "handedness": "right",
    "up": "+Z",
    "forward_at_zero": "+X",
    "units": "meters"
  },
  "rotation_spec": {
    "mode": "manual",
    "nominal_increment_deg": 60.0,
    "total_stops": 6,
    "accumulation_sec_per_stop": 5.0,
    "axis_tilt_deg": 0.0
  },
  "stops": [
    {
      "index": 0,
      "nominal_angle_deg": 0.0,
      "measured_angle_deg": 0.0,
      "angle_source": "nominal_manual",
      "timestamp_start_sec": 1794500000.00,
      "timestamp_end_sec": 1794500005.00,
      "lidar_points_captured": 48210,
      "raw_lidar_file": "raw/stop_000_000deg.avraw",
      "rgb_image_file": "raw/images/stop_000.jpg",
      "registration_correction": {
        "delta_yaw_deg": 0.0,
        "delta_pitch_deg": 0.0,
        "delta_roll_deg": 0.0,
        "rmse_m": 0.0021
      }
    }
  ],
  "products": {
    "point_cloud_ply": "pointcloud.ply",
    "point_cloud_lod0": "cache/lod0.bin",
    "station_e57": "station.e57",
    "panorama_image": "pano/pano_rgb.jpg",
    "coverage_mask": "pano/coverage_mask.png",
    "total_merged_points": 385680
  },
  "registration_quality": {
    "status": "VERIFIED_HIGH_CONFIDENCE",
    "overall_rmse_m": 0.0034,
    "min_overlap_ratio": 0.42,
    "angular_closure_residual_deg": 0.08
  }
}
```

---

## 5. Architectural Implementation Next Steps

1. **Header Contracts (`include/avscan/terrestrial/`)**:
   - `StationDataModel.hpp`: Immutable station data structures, capture parameters, stop records.
   - `IRotationController.hpp`: Abstract rotation provider (`Manual`, `Preset`, `Automatic`).
   - `RotationGeometry.hpp`: Kinematic chain solver for $T_{\text{station\_lidar}}(\theta)$.
   - `TerrestrialStationManager.hpp`: Orchestrates stop lifecycle, raw capture WAL, and merging.
2. **Core Source (`src/terrestrial/`)**:
   - `RotationGeometry.cpp`
   - `TerrestrialStationManager.cpp`
   - `StationRegistrationEngine.cpp`: Overlap-constrained 1-DOF rotation alignment across stops.
3. **Bridge & UI Layer**:
   - Extend `AVScanCoreBridge.h/.mm` with mode switcher (`AVScanAcquisitionModeMobileSlam` vs `AVScanAcquisitionModeTerrestrial`).
   - Update `MainScannerViewController.mm` with the clean Apple Glass Terrestrial capture workflow.
