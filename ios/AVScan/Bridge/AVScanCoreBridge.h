#pragma once

#import <Foundation/Foundation.h>
#import "../Sensors/CameraCaptureManager.h"
#include "avscan/backend/GicpVerifier.hpp"
#include "avscan/backend/PoseGraphOptimizer.hpp"
#include "avscan/backend/ScanContext.hpp"
#include "avscan/calibration/TargetlessCalibrator.hpp"
#include "avscan/core/Types.hpp"
#include "avscan/export/PointCloudExporter.hpp"
#include "avscan/quality/QualityMonitor.hpp"
#include "avscan/sensors/AiryDiscovery.hpp"
#include "avscan/sensors/AiryUdpReceiver.hpp"
#include "avscan/slam/FastLivo2Engine.hpp"
#include "avscan/storage/ProjectStore.hpp"
#include "avscan/sync/TimeSynchronizer.hpp"
#include "avscan/system/ResourceController.hpp"

@interface AVScanCoreBridge : NSObject

@property (nonatomic, assign, readonly) BOOL isScanning;
@property (nonatomic, copy, readonly) NSString *activeProjectId;
@property (nonatomic, copy, readonly) NSString *activeProjectDir;

- (instancetype)init;

// Project & Session Lifecycle
- (BOOL)createOrOpenProjectWithId:(NSString *)projectId name:(NSString *)projectName;
- (BOOL)startScanningSessionWithVisualFusion:(BOOL)enableVisual useArkitCamera:(BOOL)useArkit;
- (void)stopScanningSession;
- (BOOL)saveAndRefineSession;
- (BOOL)capturePointOfInterestPhotoWithNote:(NSString *)note;

// Exports
- (BOOL)exportCurrentMapToPly:(NSString *)filePath;
- (BOOL)exportCurrentMapToPcd:(NSString *)filePath;
- (BOOL)exportCurrentMapToE57:(NSString *)filePath;

// Diagnostics & State Access
- (avscan::sensors::AiryDiscoveryReport)runAiryDiagnostics;
- (void)updateNetworkConfig:(const avscan::sensors::AiryNetworkConfig &)config;
- (avscan::sensors::AiryNetworkConfig)currentNetworkConfig;
- (avscan::sensors::AiryPacketStats)currentPacketStats;
- (avscan::sync::SyncDiagnostics)currentSyncDiagnostics;
- (avscan::core::SlamState)currentSlamState;
- (avscan::quality::ScanQualityReport)latestQualityReport;
- (avscan::quality::FloorPlanGrid2D)latestFloorPlan;
- (std::vector<avscan::core::LidarPointXYZIRT>)latestGlobalPointCloud;
- (std::vector<avscan::core::SlamState>)currentTrajectory;
- (size_t)loopClosureCount;
- (avscan::core::ExtrinsicCalibrationProfile)currentCalibrationProfile;
- (void)applyCalibrationProfile:(const avscan::core::ExtrinsicCalibrationProfile &)profile;

@end
