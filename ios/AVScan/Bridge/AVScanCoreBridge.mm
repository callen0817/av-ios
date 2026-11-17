#import "AVScanCoreBridge.h"
#import <UIKit/UIKit.h>
#include <sys/utsname.h>
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
    avscan::core::AiryFactoryCalibration _factoryCalib;
    avscan::calibration::OnlineAutoCalibrator _autoCalibrator;
    std::atomic<bool> _autoCalibActive;
    avscan::quality::ScanQualityReport _latestQuality;
    avscan::quality::FloorPlanGrid2D _latestFloorPlan;
    std::vector<avscan::core::LidarPointXYZIRT> _cachedGlobalCloud;
    std::vector<avscan::storage::PoiPhotoRecord> _poiPhotos;

    std::unique_ptr<avscan::terrestrial::TerrestrialStationManager> _stationManager;
    BOOL _isTerrestrialStationActive;
    BOOL _isSlamPausedForStation;
    avscan::math::SE3d _flashStationAnchorPose;
    AVMotorVerificationStatus _motorVerificationStatus;

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
        _autoCalibActive = false;
        _nextKeyframeId = 1;
        _unclosedTrajectoryDistM = 0.0;

        _discovery = std::make_unique<avscan::sensors::AiryDiscovery>(_netConfig);
        _udpReceiver = std::make_unique<avscan::sensors::AiryUdpReceiver>(_netConfig);
        _cameraManager = [[CameraCaptureManager alloc] initWithMode:AVCameraAcquisitionModeAVFoundationPure];

        [self setupCallbacks];
        [self detectAndApplyHostDeviceProfile];
    }
    return self;
}

- (void)dealloc {
    [self stopScanningSession];
}

- (void)setupCallbacks {
    AVScanCoreBridge * __weak weakSelf = self;

    _udpReceiver->setCallbacks(
        [weakSelf](const avscan::core::LidarFrame &frame) {
            AVScanCoreBridge *strongSelf = weakSelf;
            if (!strongSelf) return;
            double nowSec = CACurrentMediaTime();
            strongSelf->_synchronizer.pushLidarFrame(frame, nowSec);
            if (strongSelf->_isTerrestrialStationActive && strongSelf->_stationManager) {
                strongSelf->_stationManager->pushLidarFrame(frame);
            }
            std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
            if (!strongSelf->_isScanning && !strongSelf->_isTerrestrialStationActive) {
                strongSelf->_cachedGlobalCloud = frame.points;
            }
        },
        [weakSelf](const avscan::core::ImuMeasurement &imu) {
            AVScanCoreBridge *strongSelf = weakSelf;
            if (!strongSelf) return;
            double nowSec = CACurrentMediaTime();
            strongSelf->_synchronizer.pushImu(imu, nowSec);
            if (strongSelf->_isTerrestrialStationActive && strongSelf->_stationManager) {
                strongSelf->_stationManager->pushImuMeasurement(imu);
            }
        },
        [weakSelf](const avscan::core::AiryFactoryCalibration &calib) {
            AVScanCoreBridge *strongSelf = weakSelf;
            if (!strongSelf) return;
            std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
            strongSelf->_factoryCalib = calib;
            strongSelf->_slamEngine.setFactoryCalibration(calib);
        }
    );

    [_cameraManager setFrameCallback:[weakSelf](const avscan::core::RgbFrame &rgbFrame) {
        AVScanCoreBridge *strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf->_synchronizer.pushRgbFrame(rgbFrame);
    } diagnosticPoseCallback:[weakSelf](double timestampSec, const avscan::math::SE3d &pose) {
        AVScanCoreBridge *strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_autoCalibActive.load()) return;
        strongSelf->_autoCalibrator.pushCameraPose(timestampSec, pose);
    }];
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

- (BOOL)startScanningSessionWithAutoCalib:(BOOL)autoCalib {
    if (_isScanning) return YES;
    if (autoCalib) {
        _autoCalibrator.reset();
        _autoCalibActive.store(true);
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            // Keep LIO-only until OnlineAutoCalibrator locks T_cam_lidar for this mount
            _calibProfile.verified = false;
            _calibProfile.confidence_score = 0.0;
        }
        return [self startScanningSessionWithVisualFusion:NO useArkitCamera:YES];
    }
    _autoCalibActive.store(false);
    return [self startScanningSessionWithVisualFusion:YES useArkitCamera:NO];
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
    _slamWorkerThread = std::thread([self]() {
        [self slamProcessingLoop];
    });
    return YES;
}

- (void)stopScanningSession {
    _isScanning = NO;
    _autoCalibActive.store(false);
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
        if (_isSlamPausedForStation) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

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

        if (_autoCalibActive.load()) {
            const avscan::math::SE3d T_world_lidar = state.T_world_body * _factoryCalib.T_imu_lidar;
            if (_autoCalibrator.pushLidarBundleAndPose(bundle, T_world_lidar)) {
                if (!_autoCalibrator.isLocked()) {
                    std::vector<avscan::calibration::HandEyeMotionPair> motions;
                    std::vector<avscan::calibration::DirectVLCalibPair> vlPairs;
                    std::vector<std::pair<double, double>> imuNorms;
                    std::vector<std::pair<double, double>> camNorms;
                    if (_autoCalibrator.beginSolve(motions, vlPairs, imuNorms, camNorms)) {
                        AVScanCoreBridge * __weak weakSelf = self;
                        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                            AVScanCoreBridge *strongSelf = weakSelf;
                            if (!strongSelf) return;

                            struct utsname sysInfo {};
                            uname(&sysInfo);
                            std::string deviceModel = sysInfo.machine[0] ? sysInfo.machine : "iOS_Device";

                            avscan::core::ExtrinsicCalibrationProfile solvedProfile;
                            bool ok = strongSelf->_autoCalibrator.completeSolve(
                                motions, vlPairs, imuNorms, camNorms,
                                deviceModel, "universal_clamp_auto", solvedProfile);
                            if (ok && solvedProfile.verified) {
                                std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
                                if (strongSelf->_isScanning) {
                                    strongSelf->_calibProfile = solvedProfile;
                                    strongSelf->_slamEngine.setExtrinsicCalibration(solvedProfile);
                                    strongSelf->_slamEngine.setVisualFusionEnabled(true);
                                    strongSelf->_synchronizer.setCalibratedCameraTimeOffset(solvedProfile.camera_time_offset_sec);
                                }
                            }
                        });
                    }
                } else {
                    std::vector<avscan::calibration::DirectVLCalibPair> vlPairs;
                    avscan::core::ExtrinsicCalibrationProfile initProfile;
                    if (_autoCalibrator.beginContinuousRefine(vlPairs, initProfile)) {
                        AVScanCoreBridge * __weak weakSelf = self;
                        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                            AVScanCoreBridge *strongSelf = weakSelf;
                            if (!strongSelf) return;

                            avscan::core::ExtrinsicCalibrationProfile profileToApply;
                            strongSelf->_autoCalibrator.completeContinuousRefine(vlPairs, profileToApply);
                            if (profileToApply.verified) {
                                std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
                                if (strongSelf->_isScanning) {
                                    strongSelf->_calibProfile = profileToApply;
                                    strongSelf->_slamEngine.setExtrinsicCalibration(profileToApply);
                                    strongSelf->_slamEngine.setVisualFusionEnabled(true);
                                }
                            }
                        });
                    }
                }
            }
        }

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

- (BOOL)exportCurrentMapToSpatialBundle:(NSString *)outputDir error:(NSError **)error {
    std::lock_guard<std::mutex> lock(_stateMutex);

    avscan::export_bundle::AvShowcaseProjectConfig config;
    config.project_id = _activeProjectId ? _activeProjectId.UTF8String : "PROJ_001";
    config.project_name = "AV Scan Spatial Session";
    config.device_profile_id = avscan::core::DeviceProfileRegistry::instance().activeProfile().profile_id;
    config.target_av_showcase_dir = "";

    // Build stations from optimized keyframe submaps
    std::vector<avscan::export_bundle::PanoStationRecord> stations;
    const auto &submaps = _poseGraph.getSubmaps();
    for (size_t i = 0; i < submaps.size(); ++i) {
        avscan::export_bundle::PanoStationRecord st;
        st.station_id = "st_" + std::to_string(i);
        st.station_index = i;
        st.keyframe_id = submaps[i].keyframe_id;
        st.timestamp_sec = submaps[i].timestamp_sec;
        st.T_world_station = submaps[i].T_world_keyframe;
        st.point_count = submaps[i].local_points.size();
        st.pano_image_relpath = "panos/pano_" + std::to_string(i) + ".jpg";
        st.depth_image_relpath = "panos/pano_" + std::to_string(i) + "_depth.png";
        stations.push_back(st);
    }

    // Trajectory frames
    auto states = _slamEngine.getTrajectory();

    bool ok = avscan::export_bundle::AvSpatialBundleExporter::exportWalkthroughBundle(
        outputDir.UTF8String, config, _cachedGlobalCloud, states, _latestFloorPlan, stations);

    if (!ok && error) {
        *error = [NSError errorWithDomain:@"com.artificialvision.avscan.export"
                                     code:-1
                                 userInfo:@{NSLocalizedDescriptionKey: @"Failed to export AV Spatial Walkthrough Bundle"}];
    }
    return ok ? YES : NO;
}

- (BOOL)exportCurrentMapToAvShowcaseProject:(NSString *)showcaseRootDir error:(NSError **)error {
    std::lock_guard<std::mutex> lock(_stateMutex);

    avscan::export_bundle::AvShowcaseProjectConfig config;
    config.project_id = _activeProjectId ? _activeProjectId.UTF8String : "PROJ_001";
    config.project_name = "AV Scan Spatial Session";
    config.device_profile_id = avscan::core::DeviceProfileRegistry::instance().activeProfile().profile_id;
    config.target_av_showcase_dir = showcaseRootDir.UTF8String;

    std::vector<avscan::export_bundle::PanoStationRecord> stations;
    const auto &submaps = _poseGraph.getSubmaps();
    for (size_t i = 0; i < submaps.size(); ++i) {
        avscan::export_bundle::PanoStationRecord st;
        st.station_id = "st_" + std::to_string(i);
        st.station_index = i;
        st.keyframe_id = submaps[i].keyframe_id;
        st.timestamp_sec = submaps[i].timestamp_sec;
        st.T_world_station = submaps[i].T_world_keyframe;
        st.point_count = submaps[i].local_points.size();
        stations.push_back(st);
    }

    auto states = _slamEngine.getTrajectory();

    bool ok = avscan::export_bundle::AvSpatialBundleExporter::exportWalkthroughBundle(
        showcaseRootDir.UTF8String, config, _cachedGlobalCloud, states, _latestFloorPlan, stations);

    if (!ok && error) {
        *error = [NSError errorWithDomain:@"com.artificialvision.avscan.export"
                                     code:-2
                                 userInfo:@{NSLocalizedDescriptionKey: @"Failed to export AV Showcase Project"}];
    }
    return ok ? YES : NO;
}

- (NSArray<NSDictionary<NSString *, id> *> *)availableDeviceProfiles {
    const auto &all = avscan::core::DeviceProfileRegistry::instance().allProfiles();
    NSMutableArray *arr = [NSMutableArray arrayWithCapacity:all.size()];
    for (const auto &p : all) {
        [arr addObject:@{
            @"id": [NSString stringWithUTF8String:p.profile_id.c_str()],
            @"name": [NSString stringWithUTF8String:p.display_name.c_str()],
            @"model": [NSString stringWithUTF8String:p.device_model.c_str()],
            @"screen": @(p.screen_diagonal_inches),
            @"isPhone": @(p.is_phone),
            @"hasNativeLidar": @(p.has_native_lidar),
            @"cameraOffset": [NSString stringWithFormat:@"%.1f, %.1f, %.1f mm",
                p.camera_offset_from_usbc_m.x * 1000.0,
                p.camera_offset_from_usbc_m.y * 1000.0,
                p.camera_offset_from_usbc_m.z * 1000.0]
        }];
    }
    return arr;
}

- (NSString *)activeDeviceProfileId {
    return [NSString stringWithUTF8String:avscan::core::DeviceProfileRegistry::instance().activeProfile().profile_id.c_str()];
}

- (NSString *)activeDeviceProfileDisplayName {
    return [NSString stringWithUTF8String:avscan::core::DeviceProfileRegistry::instance().activeProfile().display_name.c_str()];
}

- (BOOL)setActiveDeviceProfileWithId:(NSString *)profileId {
    if (!profileId) return NO;
    bool ok = avscan::core::DeviceProfileRegistry::instance().setActiveProfile(profileId.UTF8String);
    if (ok) {
        auto ext = avscan::core::DeviceProfileRegistry::instance().activeExtrinsics();
        [self applyCalibrationProfile:ext];
        return YES;
    }
    return NO;
}

- (void)detectAndApplyHostDeviceProfile {
    struct utsname sysInfo;
    if (uname(&sysInfo) == 0) {
        NSString *machine = [NSString stringWithCString:sysInfo.machine encoding:NSUTF8StringEncoding];
        if (machine.length > 0) {
            auto match = avscan::core::DeviceProfileRegistry::instance().getProfileForModel(machine.UTF8String);
            if (match.has_value()) {
                [self setActiveDeviceProfileWithId:[NSString stringWithUTF8String:match->profile_id.c_str()]];
                return;
            }
        }
    }
    // Default testing hardware profile: iPad 10th Gen (A2696)
    [self setActiveDeviceProfileWithId:@"ipad_10th_gen_airy_rigid"];
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

- (avscan::calibration::AutoCalibStatus)currentAutoCalibStatus {
    return _autoCalibrator.getStatus();
}

- (void)applyCalibrationProfile:(const avscan::core::ExtrinsicCalibrationProfile &)profile {
    std::lock_guard<std::mutex> lock(_stateMutex);
    _calibProfile = profile;
    _slamEngine.setExtrinsicCalibration(profile);
    _synchronizer.setCalibratedCameraTimeOffset(profile.camera_time_offset_sec);
}

#pragma mark - Terrestrial Mode Implementation (Mode B) & Hybrid Flash Station

- (BOOL)confirmMotorAttachedWithAutomatic:(BOOL)isAuto model:(NSString *)motorModel {
    _motorVerificationStatus = isAuto ? AVMotorVerificationStatusAutomaticConnected
                                      : AVMotorVerificationStatusManualConfirmed;
    return YES;
}

- (BOOL)startTerrestrialStationWithId:(NSString *)stationId name:(NSString *)stationName stepDeg:(double)stepDeg isFlashDuringSlam:(BOOL)isFlash {
    if (isFlash && _isScanning) {
        // Hybrid Flash Station during active SLAM: pause SLAM trajectory, anchor pose
        _isSlamPausedForStation = YES;
        std::lock_guard<std::mutex> lock(_stateMutex);
        _flashStationAnchorPose = _slamEngine.getCurrentState().T_world_body;
        _acquisitionMode = AVScanAcquisitionModeHybridFlashStation;
    } else {
        if (_isScanning) {
            [self stopScanningSession];
        }
        _isSlamPausedForStation = NO;
        _acquisitionMode = AVScanAcquisitionModeTerrestrial;
    }

    if (!_activeProjectDir) {
        [self createOrOpenProjectWithId:@"PROJECT_001" name:@"AV Terrestrial Session"];
    }

    avscan::terrestrial::TerrestrialStationConfig config;
    config.project_id = _activeProjectId ? _activeProjectId.UTF8String : "PROJ_001";
    config.station_id = stationId ? stationId.UTF8String : "st_001";
    config.station_name = stationName ? stationName.UTF8String : "Station 01";
    config.nominal_step_deg = (stepDeg > 5.0) ? stepDeg : 60.0;
    config.total_expected_stops = static_cast<size_t>(std::round(360.0 / config.nominal_step_deg));
    config.accumulation_sec_per_stop = 5.0;

    auto mountProfile = avscan::core::DeviceProfileRegistry::instance().activeProfile();
    _stationManager = std::make_unique<avscan::terrestrial::TerrestrialStationManager>(config);
    _stationManager->startNewStation(_activeProjectDir.UTF8String, config, mountProfile);

    _udpReceiver->start();
    _isTerrestrialStationActive = YES;
    return YES;
}

- (BOOL)beginTerrestrialStop {
    if (!_stationManager) return NO;
    return _stationManager->beginCurrentStopAccumulation() ? YES : NO;
}

- (BOOL)finishTerrestrialStop {
    if (!_stationManager) return NO;
    BOOL ok = _stationManager->finishCurrentStop() ? YES : NO;
    if (ok) {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (_isSlamPausedForStation) {
            // Transform station points by anchor pose into global map
            auto statPoints = _stationManager->buildStationPointCloud();
            for (const auto &p : statPoints) {
                avscan::math::Vec3d v(p.x, p.y, p.z);
                avscan::math::Vec3d vw = _flashStationAnchorPose * v;
                avscan::core::LidarPointXYZIRT pw = p;
                pw.x = vw.x; pw.y = vw.y; pw.z = vw.z;
                _cachedGlobalCloud.push_back(pw);
            }
        } else {
            _cachedGlobalCloud = _stationManager->buildStationPointCloud();
        }
    }
    return ok;
}

- (BOOL)advanceToNextTerrestrialStop {
    if (!_stationManager) return NO;
    return _stationManager->advanceToNextStop() ? YES : NO;
}

- (BOOL)repeatCurrentTerrestrialStop {
    if (!_stationManager) return NO;
    return _stationManager->repeatCurrentStop() ? YES : NO;
}

- (BOOL)finalizeAndSaveTerrestrialStation {
    if (!_stationManager) return NO;
    std::string err;
    BOOL ok = _stationManager->finalizeAndSaveStation(err) ? YES : NO;
    if (ok) {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (_isSlamPausedForStation) {
            // Create high-weight anchor keyframe in SLAM pose graph
            avscan::core::KeyframeSubmap anchorSubmap;
            anchorSubmap.keyframe_id = _nextKeyframeId++;
            anchorSubmap.timestamp_sec = CACurrentMediaTime();
            anchorSubmap.T_world_keyframe = _flashStationAnchorPose;
            anchorSubmap.local_points = _stationManager->buildStationPointCloud();

            _projectStore.appendKeyframeToWal(anchorSubmap);
            _poseGraph.addKeyframeSubmap(anchorSubmap);

            [self resumeSlamAfterFlashStation];
        } else {
            _cachedGlobalCloud = _stationManager->buildStationPointCloud();
            _isTerrestrialStationActive = NO;
        }
    }
    return ok;
}

- (BOOL)resumeSlamAfterFlashStation {
    _isSlamPausedForStation = NO;
    _isTerrestrialStationActive = NO;
    _acquisitionMode = AVScanAcquisitionModeMobileSlam;
    _slamEngine.resetPose(_flashStationAnchorPose);
    return YES;
}

- (uint32_t)currentTerrestrialStopIndex {
    return _stationManager ? _stationManager->getCurrentStopIndex() : 0;
}

- (double)terrestrialAccumulationProgress {
    return _stationManager ? _stationManager->getCurrentAccumulationProgress() : 0.0;
}

- (BOOL)isTerrestrialStationary {
    return _stationManager ? (_stationManager->isStationary() ? YES : NO) : NO;
}

@end
