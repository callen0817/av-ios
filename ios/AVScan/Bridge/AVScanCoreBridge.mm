#import "AVScanCoreBridge.h"
#import <UIKit/UIKit.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

@implementation AVScanCoreBridge {
    avscan::sensors::AiryNetworkConfig _netConfig;
    std::unique_ptr<avscan::sensors::AiryDiscovery> _discovery;
    std::unique_ptr<avscan::sensors::AiryUdpReceiver> _udpReceiver;
    CameraCaptureManager *_cameraManager;

    avscan::sync::TimeSynchronizer _synchronizer;
    avscan::slam::FastLivo2Engine _slamEngine;
    avscan::backend::ScanContextDatabase _scDatabase;
    avscan::backend::GicpVerifier _gicpVerifier;
    avscan::backend::PoseGraphOptimizer _poseGraph;
    avscan::quality::QualityMonitor _qualityMonitor;
    avscan::storage::ProjectStore _projectStore;
    avscan::system::ResourceController _resourceController;

    avscan::core::ExtrinsicCalibrationProfile _calibProfile;
    avscan::quality::ScanQualityReport _latestQuality;
    avscan::quality::FloorPlanGrid2D _latestFloorPlan;
    std::vector<avscan::core::LidarPointXYZIRT> _cachedGlobalCloud;
    std::vector<avscan::storage::PoiPhotoRecord> _poiPhotos;

    std::atomic<bool> _workerRunning;
    std::thread _slamWorkerThread;
    std::mutex _stateMutex;
    uint64_t _nextKeyframeId;
    double _unclosedTrajectoryDistM;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _isScanning = NO;
        _workerRunning = false;
        _nextKeyframeId = 1;
        _unclosedTrajectoryDistM = 0.0;

        _discovery = std::make_unique<avscan::sensors::AiryDiscovery>(_netConfig);
        _udpReceiver = std::make_unique<avscan::sensors::AiryUdpReceiver>(_netConfig);
        _cameraManager = [[CameraCaptureManager alloc] initWithMode:AVCameraAcquisitionModeAVFoundationPure];

        [self setupCallbacks];
    }
    return self;
}

- (void)dealloc {
    [self stopScanningSession];
}

- (void)setupCallbacks {
    __weak typeof(self) weakSelf = self;

    _udpReceiver->setCallbacks(
        [weakSelf](const avscan::core::LidarFrame &frame) {
            __strong typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf) return;
            double nowSec = CACurrentMediaTime();
            strongSelf->_synchronizer.pushLidarFrame(frame, nowSec);
            std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
            if (!strongSelf->_isScanning) {
                strongSelf->_cachedGlobalCloud = frame.points;
            }
        },
        [weakSelf](const avscan::core::ImuMeasurement &imu) {
            __strong typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf) return;
            double nowSec = CACurrentMediaTime();
            strongSelf->_synchronizer.pushImu(imu, nowSec);
        },
        [weakSelf](const avscan::core::AiryFactoryCalibration &calib) {
            __strong typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf) return;
            std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
            strongSelf->_slamEngine.setFactoryCalibration(calib);
        }
    );

    [_cameraManager setFrameCallback:[weakSelf](const avscan::core::RgbFrame &rgbFrame) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf->_synchronizer.pushRgbFrame(rgbFrame);
    } diagnosticPoseCallback:nullptr];
}

- (BOOL)createOrOpenProjectWithId:(NSString *)projectId name:(NSString *)projectName {
    NSArray<NSString *> *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *docsDir = paths.firstObject ?: NSTemporaryDirectory();
    NSString *projDir = [docsDir stringByAppendingPathComponent:projectId];

    _activeProjectId = [projectId copy];
    _activeProjectDir = [projDir copy];

    return _projectStore.initializeProject(
        projDir.UTF8String,
        projectId.UTF8String,
        projectName.UTF8String) ? YES : NO;
}

- (BOOL)startScanningSessionWithVisualFusion:(BOOL)enableVisual useArkitCamera:(BOOL)useArkit {
    if (_isScanning) return YES;
    if (!_activeProjectDir) {
        [self createOrOpenProjectWithId:@"PROJECT_001" name:@"AV Scan Level 01"];
    }

    _slamEngine.setVisualFusionEnabled(enableVisual ? true : false);
    _slamEngine.setExtrinsicCalibration(_calibProfile);
    _resourceController.setOperatingMode(avscan::core::OperatingMode::MAPPING_DENSE);

    NSString *rawPath = [_activeProjectDir stringByAppendingPathComponent:@"raw_capture.avraw"];
    _udpReceiver->startRawRecording(rawPath.UTF8String);
    _udpReceiver->start();

    AVCameraAcquisitionMode camMode = useArkit
        ? AVCameraAcquisitionModeARKitSession
        : AVCameraAcquisitionModeAVFoundationPure;
    [_cameraManager startCaptureWithMode:camMode];

    _isScanning = YES;
    _workerRunning.store(true);
    _slamWorkerThread = std::thread([this]() {
        [self slamProcessingLoop];
    });
    return YES;
}

- (void)stopScanningSession {
    _isScanning = NO;
    if (_workerRunning.exchange(false)) {
        if (_slamWorkerThread.joinable()) {
            _slamWorkerThread.join();
        }
    }
    [_cameraManager stopCapture];
    _udpReceiver->stopRawRecording();
}

- (void)slamProcessingLoop {
    while (_workerRunning.load()) {
        avscan::sync::SyncBundle bundle;
        if (!_synchronizer.popSyncBundle(bundle)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        avscan::core::LidarFrame deskewedBody;
        avscan::core::LidarFrame worldFrame;
        avscan::slam::EstimatorMetrics metrics;

        std::lock_guard<std::mutex> lock(_stateMutex);
        bool ok = _slamEngine.processBundle(bundle, deskewedBody, worldFrame, metrics);
        if (!ok) continue;

        auto state = _slamEngine.getCurrentState();
        _latestQuality = _qualityMonitor.evaluateFrameQuality(state, metrics, _unclosedTrajectoryDistM);

        if (_latestQuality.tracking_lost) {
            avscan::math::SE3d recoveredPose;
            uint64_t matchedKf = 0;
            double rmse = 0.0;
            if (_qualityMonitor.attemptRelocalization(
                    deskewedBody, _scDatabase, _poseGraph.getSubmaps(), _gicpVerifier,
                    recoveredPose, matchedKf, rmse)) {
                _slamEngine.resetPose(recoveredPose);
                state = _slamEngine.getCurrentState();
            }
        }

        if (metrics.keyframe_emitted) {
            avscan::core::KeyframeSubmap submap;
            submap.keyframe_id = _nextKeyframeId++;
            submap.timestamp_sec = bundle.lidar_frame.timestamp_end_sec;
            submap.T_world_keyframe = state.T_world_body;
            submap.local_points = deskewedBody.points;

            _projectStore.appendKeyframeToWal(submap);
            _poseGraph.addKeyframeSubmap(submap);
            _unclosedTrajectoryDistM += 0.35;

            auto scDesc = avscan::backend::ScanContextDatabase::computeDescriptor(
                submap.keyframe_id, submap.timestamp_sec, submap.local_points, bundle.rgb_frame);
            auto loopCand = _scDatabase.detectLoopClosure(scDesc);
            _scDatabase.addKeyframeDescriptor(scDesc);

            if (loopCand.valid) {
                for (auto &targetSm : _poseGraph.getMutableSubmaps()) {
                    if (targetSm.keyframe_id == loopCand.match_keyframe_id) {
                        if (targetSm.local_points.empty()) {
                            _projectStore.loadSubmapFromDisk(targetSm);
                        }
                        avscan::math::SO3d R_yaw = avscan::math::SO3d::exp(
                            avscan::math::Vec3d(0.0, 0.0, loopCand.initial_yaw_shift_rad));
                        avscan::math::SE3d initGuess(R_yaw, avscan::math::Vec3d(0, 0, 0));
                        auto ver = _gicpVerifier.verifyLoopConstraint(
                            submap.local_points, targetSm.local_points, initGuess);
                        if (ver.verified) {
                            _poseGraph.addVerifiedLoopFactor(
                                targetSm.keyframe_id,
                                submap.keyframe_id,
                                ver.T_target_source,
                                ver.information_diagonal,
                                true);
                            _poseGraph.optimizeGlobalPoseGraph(15);
                            _slamEngine.resetPose(_poseGraph.getSubmaps().back().T_world_keyframe);
                            _unclosedTrajectoryDistM = 0.0;
                        }
                        break;
                    }
                }
            }

            _cachedGlobalCloud = _poseGraph.buildGloballyConsistentPointCloud(2);
            _latestFloorPlan = avscan::quality::QualityMonitor::generateLiveFloorPlan(
                _cachedGlobalCloud, _poseGraph.getSubmaps(), _poseGraph.getFactors(), 0.10);
        }
    }
}

- (BOOL)saveAndRefineSession {
    [self stopScanningSession];
    std::lock_guard<std::mutex> lock(_stateMutex);
    _resourceController.setOperatingMode(avscan::core::OperatingMode::FINAL_REFINEMENT);

    for (auto &sm : _poseGraph.getMutableSubmaps()) {
        if (sm.local_points.empty()) {
            _projectStore.loadSubmapFromDisk(sm);
        }
    }
    _poseGraph.optimizeGlobalPoseGraph(40);
    _cachedGlobalCloud = _poseGraph.buildGloballyConsistentPointCloud(1);
    _latestFloorPlan = avscan::quality::QualityMonitor::generateLiveFloorPlan(
        _cachedGlobalCloud, _poseGraph.getSubmaps(), _poseGraph.getFactors(), 0.08);

    return _projectStore.saveProjectSession(
        _calibProfile, _poseGraph.getSubmaps(), _poseGraph.getFactors(), _poiPhotos) ? YES : NO;
}

- (BOOL)capturePointOfInterestPhotoWithNote:(NSString *)note {
    std::lock_guard<std::mutex> lock(_stateMutex);
    avscan::storage::PoiPhotoRecord rec;
    rec.photo_id = _poiPhotos.size() + 1;
    rec.timestamp_sec = CACurrentMediaTime();
    rec.T_world_camera = _slamEngine.getCurrentState().T_world_body * _calibProfile.T_cam_lidar.inverse();
    rec.image_file_path = [[_activeProjectDir stringByAppendingPathComponent:
        [NSString stringWithFormat:@"photos/poi_%llu.jpg", rec.photo_id]] UTF8String];
    rec.note = note ? note.UTF8String : "";
    _poiPhotos.push_back(rec);
    return YES;
}

- (BOOL)exportCurrentMapToPly:(NSString *)filePath {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return avscan::exporters::PointCloudExporter::exportToPly(filePath.UTF8String, _cachedGlobalCloud, true) ? YES : NO;
}

- (BOOL)exportCurrentMapToPcd:(NSString *)filePath {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return avscan::exporters::PointCloudExporter::exportToPcd(filePath.UTF8String, _cachedGlobalCloud, true) ? YES : NO;
}

- (BOOL)exportCurrentMapToE57:(NSString *)filePath {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return avscan::exporters::PointCloudExporter::exportToE57(filePath.UTF8String, _cachedGlobalCloud) ? YES : NO;
}

- (avscan::sensors::AiryDiscoveryReport)runAiryDiagnostics {
    return _discovery->runDiagnostics(200, 200);
}

- (void)updateNetworkConfig:(const avscan::sensors::AiryNetworkConfig &)config {
    _netConfig = config;
    _discovery->setConfig(config);
}

- (avscan::sensors::AiryNetworkConfig)currentNetworkConfig {
    return _netConfig;
}

- (avscan::sensors::AiryPacketStats)currentPacketStats {
    return _udpReceiver->getPacketStats();
}

- (avscan::sync::SyncDiagnostics)currentSyncDiagnostics {
    return _synchronizer.getDiagnostics();
}

- (avscan::core::SlamState)currentSlamState {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _slamEngine.getCurrentState();
}

- (avscan::quality::ScanQualityReport)latestQualityReport {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _latestQuality;
}

- (avscan::quality::FloorPlanGrid2D)latestFloorPlan {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _latestFloorPlan;
}

- (std::vector<avscan::core::LidarPointXYZIRT>)latestGlobalPointCloud {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _cachedGlobalCloud;
}

- (std::vector<avscan::core::SlamState>)currentTrajectory {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _slamEngine.getTrajectory();
}

- (size_t)loopClosureCount {
    return _poseGraph.numLoopClosures();
}

- (avscan::core::ExtrinsicCalibrationProfile)currentCalibrationProfile {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _calibProfile;
}

- (void)applyCalibrationProfile:(const avscan::core::ExtrinsicCalibrationProfile &)profile {
    std::lock_guard<std::mutex> lock(_stateMutex);
    _calibProfile = profile;
    _slamEngine.setExtrinsicCalibration(profile);
    _synchronizer.setCalibratedCameraTimeOffset(profile.camera_time_offset_sec);
}

@end
