#pragma once

#import <Foundation/Foundation.h>
#import "../Sensors/CameraCaptureManager.h"
#import "../Sensors/MultipeerCollabTransport.h"
#include "avscan/backend/GicpVerifier.hpp"
#include "avscan/backend/PoseGraphOptimizer.hpp"
#include "avscan/backend/ScanContext.hpp"
#include "avscan/calibration/OnlineAutoCalibrator.hpp"
#include "avscan/calibration/TargetlessCalibrator.hpp"
#include "avscan/core/DeviceMountProfile.hpp"
#include "avscan/core/Types.hpp"
#include "avscan/cslam/CollaborativeSlamEngine.hpp"
#include "avscan/cslam/CollaborativeTransport.hpp"
#include "avscan/export/AvSpatialBundleExporter.hpp"
#include "avscan/export/PointCloudExporter.hpp"
#include "avscan/processing/PointCloudCleanup.hpp"
#include "avscan/quality/QualityMonitor.hpp"
#include "avscan/sensors/AiryDiscovery.hpp"
#include "avscan/sensors/AiryUdpReceiver.hpp"
#include "avscan/sensors/IDepthProvider.hpp"
#include "avscan/slam/FastLivo2Engine.hpp"
#include "avscan/storage/ProjectStore.hpp"
#include "avscan/sync/TimeSynchronizer.hpp"
#include "avscan/system/ResourceController.hpp"
#include "avscan/terrestrial/TerrestrialStationManager.hpp"

typedef NS_ENUM(NSInteger, AVScanAcquisitionMode) {
    AVScanAcquisitionModeMobileSlam = 0,
    AVScanAcquisitionModeTerrestrial = 1,
    AVScanAcquisitionModeHybridFlashStation = 2
};

typedef NS_ENUM(NSInteger, AVMotorVerificationStatus) {
    AVMotorVerificationStatusUnverified = 0,
    AVMotorVerificationStatusManualConfirmed = 1,
    AVMotorVerificationStatusAutomaticConnected = 2
};

@interface AVScanCoreBridge : NSObject

@property (nonatomic, assign) AVScanAcquisitionMode acquisitionMode;
@property (nonatomic, assign) AVMotorVerificationStatus motorVerificationStatus;
@property (nonatomic, assign, readonly) BOOL isScanning;
@property (nonatomic, assign, readonly) BOOL isTerrestrialStationActive;
@property (nonatomic, assign, readonly) BOOL isSlamPausedForStation;
@property (nonatomic, assign, readonly) BOOL isCslamEnabled;
@property (nonatomic, copy, readonly) NSString *activeProjectId;
@property (nonatomic, copy, readonly) NSString *activeProjectName;
@property (nonatomic, copy, readonly) NSString *activeProjectDir;
@property (nonatomic, copy, readonly) NSString *activeScanId;
@property (nonatomic, copy, readonly) NSString *activeScanName;
@property (nonatomic, copy, readonly) NSString *activeScanFloorLevel;

- (instancetype)init;

// Project & Scan Creation Hierarchy (AV Scan / AV Scanar Structure)
- (NSArray<NSDictionary<NSString *, id> *> *)listProjects;
- (BOOL)createProjectWithName:(NSString *)name site:(NSString *)site notes:(NSString *)notes outProjectId:(NSString **)outId;
- (BOOL)openProjectWithId:(NSString *)projectId;
- (NSArray<NSDictionary<NSString *, id> *> *)listScansInActiveProject;
- (BOOL)createAndSelectScanWithName:(NSString *)scanName floorLevel:(NSString *)floorLevel mode:(AVScanAcquisitionMode)mode outScanId:(NSString **)outScanId;
- (BOOL)selectScanWithId:(NSString *)scanId;
- (void)resetAutoCalibration;

// Project & Session Lifecycle
- (BOOL)createOrOpenProjectWithId:(NSString *)projectId name:(NSString *)projectName;
- (BOOL)startScanningSessionWithVisualFusion:(BOOL)enableVisual useArkitCamera:(BOOL)useArkit;
- (BOOL)startScanningSessionWithAutoCalib:(BOOL)autoCalib;
- (void)stopScanningSession;
- (BOOL)saveAndRefineSession;
- (BOOL)capturePointOfInterestPhotoWithNote:(NSString *)note;

// Exports
- (BOOL)exportCurrentMapToPly:(NSString *)filePath;
- (BOOL)exportCurrentMapToPcd:(NSString *)filePath;
- (BOOL)exportCurrentMapToE57:(NSString *)filePath;
- (BOOL)exportCurrentMapToSpatialBundle:(NSString *)outputDir error:(NSError **)error;
- (BOOL)exportCurrentMapToAvShowcaseProject:(NSString *)showcaseRootDir error:(NSError **)error;

// Device Mount Profiles & Hardware Rig
- (NSArray<NSDictionary<NSString *, id> *> *)availableDeviceProfiles;
- (NSString *)activeDeviceProfileId;
- (NSString *)activeDeviceProfileDisplayName;
- (BOOL)setActiveDeviceProfileWithId:(NSString *)profileId;
- (void)detectAndApplyHostDeviceProfile;

// Terrestrial Station Mode (Mode B) & Hybrid Flash Station during SLAM
- (NSString *)nextAvailableStationIdWithPrefix:(NSString *)prefix;
- (BOOL)confirmMotorAttachedWithAutomatic:(BOOL)isAuto model:(NSString *)motorModel;
- (BOOL)startTerrestrialStationWithId:(NSString *)stationId name:(NSString *)stationName stepDeg:(double)stepDeg isFlashDuringSlam:(BOOL)isFlash;
- (BOOL)beginTerrestrialStop;
- (BOOL)finishTerrestrialStop;
- (BOOL)advanceToNextTerrestrialStop;
- (BOOL)repeatCurrentTerrestrialStop;
- (BOOL)finalizeAndSaveTerrestrialStation;
- (BOOL)resumeSlamAfterFlashStation;
- (uint32_t)currentTerrestrialStopIndex;
- (size_t)completedTerrestrialStopCount;
- (double)terrestrialAccumulationProgress;
- (BOOL)isTerrestrialStationary;
- (avscan::terrestrial::TerrestrialCaptureState)currentTerrestrialCaptureState;
- (avscan::terrestrial::TerrestrialStationMetadata)currentTerrestrialStationMetadata;

// Collaborative SLAM (C-SLAM) Multi-Device Peer Networking (iOS / Jetson / Windows)
- (BOOL)setCollaborativeSlamEnabled:(BOOL)enabled;
- (void)connectCollaborativePeerByIp:(NSString *)ipAddress;
- (void)syncSharedOriginWithPeers;
- (NSArray<NSDictionary<NSString *, id> *> *)connectedCollaborativePeers;
- (avscan::cslam::CslamStats)collaborativeSlamStats;
- (avscan::sensors::DepthComputeTier)recommendedDepthComputeTier;

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
- (avscan::calibration::AutoCalibStatus)currentAutoCalibStatus;
- (void)applyCalibrationProfile:(const avscan::core::ExtrinsicCalibrationProfile &)profile;
- (double)cameraMeasuredFps;
- (uint64_t)cameraFramesDelivered;
- (NSString *)cameraStatusSummary;
- (BOOL)isRgbColorizationActive;
- (BOOL)activeProfileUsesExternalUsbCamera;
- (NSArray<NSString *> *)connectedExternalUsbCameras;
- (avscan::processing::CleanupStats)lastCleanupStats;

@end
