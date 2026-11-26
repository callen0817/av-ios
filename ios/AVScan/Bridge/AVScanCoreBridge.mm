#import "AVScanCoreBridge.h"
#import <UIKit/UIKit.h>
#include <sys/utsname.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {

constexpr double kLiveDisplayVoxelM = 0.03;

inline avscan::slam::VoxelKey liveVoxelKey(double x, double y, double z) {
    return {
        static_cast<int64_t>(std::floor(x / kLiveDisplayVoxelM)),
        static_cast<int64_t>(std::floor(y / kLiveDisplayVoxelM)),
        static_cast<int64_t>(std::floor(z / kLiveDisplayVoxelM))
    };
}

void mergePointsIntoLiveCloud(
    const std::vector<avscan::core::LidarPointXYZIRT>& pts,
    size_t stride,
    std::vector<avscan::core::LidarPointXYZIRT>& cloud,
    std::unordered_map<avscan::slam::VoxelKey, size_t, avscan::slam::VoxelKeyHash>& voxelIndex) {
    const size_t step = std::max<size_t>(1, stride);
    for (size_t i = 0; i < pts.size(); i += step) {
        const auto& p = pts[i];
        const auto key = liveVoxelKey(p.x, p.y, p.z);
        auto it = voxelIndex.find(key);
        if (it == voxelIndex.end()) {
            voxelIndex.emplace(key, cloud.size());
            cloud.push_back(p);
        } else if (p.colorized) {
            auto& dst = cloud[it->second];
            const uint8_t newQ = static_cast<uint8_t>((p.status_flags >> 2) & 0x3Fu);
            const uint8_t oldQ = static_cast<uint8_t>((dst.status_flags >> 2) & 0x3Fu);
            if (!dst.colorized || newQ > static_cast<uint8_t>(oldQ + 8u)) {
                // Upgrade gray voxel OR replace distant/grazing color with a significantly sharper view
                dst.r = p.r;
                dst.g = p.g;
                dst.b = p.b;
                dst.colorized = true;
                dst.status_flags = p.status_flags;
            }
        }
    }
}

void rebuildLiveVoxelIndex(
    const std::vector<avscan::core::LidarPointXYZIRT>& cloud,
    std::unordered_map<avscan::slam::VoxelKey, size_t, avscan::slam::VoxelKeyHash>& voxelIndex) {
    voxelIndex.clear();
    voxelIndex.reserve(cloud.size());
    for (size_t i = 0; i < cloud.size(); ++i) {
        const auto& p = cloud[i];
        voxelIndex[liveVoxelKey(p.x, p.y, p.z)] = i;
    }
}

}  // namespace

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
    std::vector<avscan::core::LidarPointXYZIRT> _livePreviewCloud;
    std::unordered_map<avscan::slam::VoxelKey, size_t, avscan::slam::VoxelKeyHash> _liveDisplayVoxelIndex;
    std::vector<avscan::storage::PoiPhotoRecord> _poiPhotos;

    std::unique_ptr<avscan::terrestrial::TerrestrialStationManager> _stationManager;
    BOOL _isTerrestrialStationActive;
    BOOL _isSlamPausedForStation;
    avscan::math::SE3d _flashStationAnchorPose;
    AVMotorVerificationStatus _motorVerificationStatus;
    NSString *_activeTerrestrialStationId;
    int32_t _lastCapturedRgbStopIndex;

    std::atomic<bool> _workerRunning;
    std::thread _slamWorkerThread;
    std::mutex _stateMutex;
    uint64_t _nextKeyframeId;
    double _unclosedTrajectoryDistM;
    double _totalTraveledDistM;
    std::unordered_map<uint64_t, double> _keyframeTraveledDist;
    uint64_t _lastLoopKeyframeId;
    avscan::math::Vec3d _prevOdomPos;
    bool _hasPrevOdomPos;
    BOOL _forceScratchAutoCalib;
    double _scanStartHostTime;
    uint64_t _scanStartCamFrames;
    BOOL _cameraWatchdogKicked;
    avscan::processing::CleanupStats _lastCleanupStats;

    std::unique_ptr<avscan::cslam::CollaborativeSlamEngine> _cslamEngine;
    std::unique_ptr<avscan::cslam::CollaborativeLanTransport> _cslamLanTransport;
    MultipeerCollabTransport *_multipeerTransport;
    std::atomic<bool> _cslamBeaconRunning;
    std::thread _cslamBeaconThread;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _isScanning = NO;
        _workerRunning = false;
        _autoCalibActive = false;
        _nextKeyframeId = 1;
        _unclosedTrajectoryDistM = 0.0;
        _totalTraveledDistM = 0.0;
        _lastLoopKeyframeId = 0;
        _prevOdomPos = avscan::math::Vec3d(0.0, 0.0, 0.0);
        _hasPrevOdomPos = false;
        _forceScratchAutoCalib = NO;
        _scanStartHostTime = 0.0;
        _scanStartCamFrames = 0;
        _cameraWatchdogKicked = NO;
        _isCslamEnabled = NO;
        _cslamBeaconRunning = false;

        NSString *devName = [[UIDevice currentDevice] name] ?: @"AV-iPad";
        NSString *agentId = [[[[UIDevice currentDevice] identifierForVendor] UUIDString] substringToIndex:8] ?: @"ios_node";
        _cslamEngine = std::make_unique<avscan::cslam::CollaborativeSlamEngine>(
            std::string("ios_") + agentId.UTF8String,
            devName.UTF8String,
            avscan::cslam::PeerPlatform::IOS,
            avscan::cslam::PeerApp::AV_IOS);
        _cslamLanTransport = std::make_unique<avscan::cslam::CollaborativeLanTransport>();
        _multipeerTransport = [[MultipeerCollabTransport alloc] initWithDisplayName:devName];

        // Strict planar GICP gate for live SLAM loop verification (reject centroid-fallback false positives)
        avscan::backend::GicpVerificationConfig gicpCfg;
        gicpCfg.voxel_resolution_m = 0.40;
        gicpCfg.max_correspondence_dist_m = 0.30;
        gicpCfg.max_iterations = 20;
        gicpCfg.min_inlier_overlap_ratio = 0.50;
        gicpCfg.max_fitness_rmse_m = 0.07;
        gicpCfg.max_source_points = 4000;
        gicpCfg.allow_centroid_fallback = false;
        _gicpVerifier = avscan::backend::GicpVerifier(gicpCfg);

        _discovery = std::make_unique<avscan::sensors::AiryDiscovery>(_netConfig);
        _udpReceiver = std::make_unique<avscan::sensors::AiryUdpReceiver>(_netConfig);
        _cameraManager = [[CameraCaptureManager alloc] initWithMode:AVCameraAcquisitionModeAVFoundationPure];

        [self setupCallbacks];
        [self detectAndApplyHostDeviceProfile];
        [_cameraManager requestCameraPermissionIfNeeded];
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
            if (!strongSelf->_isTerrestrialStationActive) {
                strongSelf->_synchronizer.pushLidarFrame(frame, nowSec);
            } else if (strongSelf->_stationManager) {
                strongSelf->_stationManager->pushLidarFrame(frame);
            }
            std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
            if (!strongSelf->_isScanning && !strongSelf->_isTerrestrialStationActive) {
                // Keep idle live preview separate so UDP packets never overwrite a finished SLAM map before Export
                strongSelf->_livePreviewCloud = frame.points;
            }
        },
        [weakSelf](const avscan::core::ImuMeasurement &imu) {
            AVScanCoreBridge *strongSelf = weakSelf;
            if (!strongSelf) return;
            double nowSec = CACurrentMediaTime();
            if (!strongSelf->_isTerrestrialStationActive) {
                strongSelf->_synchronizer.pushImu(imu, nowSec);
            } else if (strongSelf->_stationManager) {
                strongSelf->_stationManager->pushImuMeasurement(imu);
            }
        },
        [weakSelf](const avscan::core::AiryFactoryCalibration &calib) {
            AVScanCoreBridge *strongSelf = weakSelf;
            if (!strongSelf) return;
            std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
            const double dRotDeg =
                (strongSelf->_factoryCalib.T_imu_lidar.so3.inverse() * calib.T_imu_lidar.so3).log().norm() *
                (180.0 / 3.14159265358979323846);
            strongSelf->_factoryCalib = calib;
            strongSelf->_slamEngine.setFactoryCalibration(calib);
            // If late DIFOP shifts T_imu_lidar > 5 deg during startup, flush any pre-DIFOP baked Keyframe 0
            if (dRotDeg > 5.0 && strongSelf->_isScanning && strongSelf->_poseGraph.getSubmaps().size() <= 5) {
                strongSelf->_poseGraph.clear();
                strongSelf->_scDatabase.clear();
                strongSelf->_cachedGlobalCloud.clear();
                strongSelf->_liveDisplayVoxelIndex.clear();
                strongSelf->_keyframeTraveledDist.clear();
                strongSelf->_nextKeyframeId = 1;
                strongSelf->_totalTraveledDistM = 0.0;
                strongSelf->_unclosedTrajectoryDistM = 0.0;
                strongSelf->_hasPrevOdomPos = false;
            }
        }
    );

    [_cameraManager setFrameCallback:[weakSelf](const avscan::core::RgbFrame &rgbFrame) {
        AVScanCoreBridge *strongSelf = weakSelf;
        if (!strongSelf) return;
        if (!strongSelf->_isTerrestrialStationActive) {
            strongSelf->_synchronizer.pushRgbFrame(rgbFrame);
            return;
        }
        // Per-stop RGB capture during stationary Terrestrial / Hybrid Flash accumulation
        if (strongSelf->_stationManager &&
            strongSelf->_stationManager->getCaptureState() == avscan::terrestrial::TerrestrialCaptureState::ACCUMULATING_STOP &&
            !rgbFrame.rgb_bytes.empty() && rgbFrame.intrinsics.width > 0 && rgbFrame.intrinsics.height > 0) {
            int32_t stopIdx = static_cast<int32_t>(strongSelf->_stationManager->getCurrentStopIndex());
            if (strongSelf->_lastCapturedRgbStopIndex != stopIdx) {
                strongSelf->_lastCapturedRgbStopIndex = stopIdx;
                NSString *stId = strongSelf->_activeTerrestrialStationId ?: @"st_001";
                NSString *relPath = [NSString stringWithFormat:@"raw/images/stop_%03d.jpg", stopIdx];
                NSString *absPath = [[strongSelf->_activeProjectDir
                    stringByAppendingPathComponent:[NSString stringWithFormat:@"stations/%@", stId]]
                    stringByAppendingPathComponent:relPath];

                // Convert RGB24 buffer to JPEG via CoreGraphics/UIKit
                size_t w = static_cast<size_t>(rgbFrame.intrinsics.width);
                size_t h = static_cast<size_t>(rgbFrame.intrinsics.height);
                std::vector<uint8_t> rgba(w * h * 4, 255);
                for (size_t i = 0; i < w * h; ++i) {
                    rgba[i * 4 + 0] = rgbFrame.rgb_bytes[i * 3 + 0];
                    rgba[i * 4 + 1] = rgbFrame.rgb_bytes[i * 3 + 1];
                    rgba[i * 4 + 2] = rgbFrame.rgb_bytes[i * 3 + 2];
                }
                CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                CGContextRef ctx = CGBitmapContextCreate(
                    rgba.data(), w, h, 8, w * 4, cs,
                    kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
                if (ctx) {
                    CGImageRef cgImg = CGBitmapContextCreateImage(ctx);
                    if (cgImg) {
                        UIImage *uiImg = [UIImage imageWithCGImage:cgImg];
                        NSData *jpegData = UIImageJPEGRepresentation(uiImg, 0.90);
                        if (jpegData) {
                            [[NSFileManager defaultManager] createDirectoryAtPath:[absPath stringByDeletingLastPathComponent]
                                                      withIntermediateDirectories:YES attributes:nil error:nil];
                            if ([jpegData writeToFile:absPath atomically:YES]) {
                                strongSelf->_stationManager->pushCameraImage(relPath.UTF8String);
                            }
                        }
                        CGImageRelease(cgImg);
                    }
                    CGContextRelease(ctx);
                }
                CGColorSpaceRelease(cs);
            }
        }
    } diagnosticPoseCallback:[weakSelf](double timestampSec, const avscan::math::SE3d &pose) {
        AVScanCoreBridge *strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_autoCalibActive.load()) return;
        strongSelf->_autoCalibrator.pushCameraPose(timestampSec, pose);
    }];
}

- (NSString *)projectsRootDirectory {
    NSArray<NSString *> *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *docsDir = paths.firstObject ?: NSTemporaryDirectory();
    NSString *projectsRoot = [docsDir stringByAppendingPathComponent:@"Projects"];
    [[NSFileManager defaultManager] createDirectoryAtPath:projectsRoot withIntermediateDirectories:YES attributes:nil error:nil];
    return projectsRoot;
}

- (NSString *)sanitizeIdentifier:(NSString *)raw fallback:(NSString *)fallback {
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"];
    NSMutableString *clean = [NSMutableString string];
    for (NSUInteger i = 0; i < raw.length; ++i) {
        unichar c = [raw characterAtIndex:i];
        if ([allowed characterIsMember:c]) {
            [clean appendFormat:@"%C", c];
        } else if (c == ' ' || c == '.') {
            [clean appendString:@"_"];
        }
    }
    return (clean.length > 0) ? clean : fallback;
}

- (NSArray<NSDictionary<NSString *, id> *> *)listProjects {
    NSString *root = [self projectsRootDirectory];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray<NSString *> *entries = [fm contentsOfDirectoryAtPath:root error:nil] ?: @[];
    NSMutableArray<NSDictionary<NSString *, id> *> *result = [NSMutableArray array];

    for (NSString *entry in entries) {
        NSString *projDir = [root stringByAppendingPathComponent:entry];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:projDir isDirectory:&isDir] || !isDir) continue;

        NSString *metaPath = [projDir stringByAppendingPathComponent:@"project_manifest.json"];
        NSDictionary *meta = nil;
        NSData *data = [NSData dataWithContentsOfFile:metaPath];
        if (data) {
            meta = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        }

        NSString *scansDir = [projDir stringByAppendingPathComponent:@"scans"];
        NSArray<NSString *> *scanEntries = [fm contentsOfDirectoryAtPath:scansDir error:nil] ?: @[];
        NSUInteger scanCount = 0;
        for (NSString *se in scanEntries) {
            BOOL sDir = NO;
            if ([fm fileExistsAtPath:[scansDir stringByAppendingPathComponent:se] isDirectory:&sDir] && sDir) {
                scanCount++;
            }
        }

        [result addObject:@{
            @"id": meta[@"id"] ?: entry,
            @"name": meta[@"name"] ?: entry,
            @"site": meta[@"site"] ?: @"",
            @"notes": meta[@"notes"] ?: @"",
            @"createdAt": meta[@"createdAt"] ?: @"",
            @"scanCount": @(scanCount),
            @"path": projDir
        }];
    }

    [result sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        return [b[@"createdAt"] compare:a[@"createdAt"]];
    }];
    return result;
}

- (BOOL)createProjectWithName:(NSString *)name site:(NSString *)site notes:(NSString *)notes outProjectId:(NSString **)outId {
    NSString *displayName = (name.length > 0) ? name : @"Untitled Project";
    NSString *slug = [self sanitizeIdentifier:displayName fallback:@"PROJECT"];
    NSDateFormatter *df = [[NSDateFormatter alloc] init];
    df.dateFormat = @"yyyyMMdd_HHmmss";
    NSString *stamp = [df stringFromDate:[NSDate date]];
    NSString *projId = [NSString stringWithFormat:@"%@_%@", slug, stamp];

    NSString *projDir = [[self projectsRootDirectory] stringByAppendingPathComponent:projId];
    NSString *scansDir = [projDir stringByAppendingPathComponent:@"scans"];
    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm createDirectoryAtPath:scansDir withIntermediateDirectories:YES attributes:nil error:nil]) {
        return NO;
    }

    NSISO8601DateFormatter *iso = [[NSISO8601DateFormatter alloc] init];
    NSDictionary *manifest = @{
        @"id": projId,
        @"name": displayName,
        @"site": site ?: @"",
        @"notes": notes ?: @"",
        @"createdAt": [iso stringFromDate:[NSDate date]]
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingPrettyPrinted error:nil];
    if (json) {
        [json writeToFile:[projDir stringByAppendingPathComponent:@"project_manifest.json"] atomically:YES];
    }

    if (outId) *outId = projId;
    return [self openProjectWithId:projId];
}

- (BOOL)openProjectWithId:(NSString *)projectId {
    if (projectId.length == 0) return NO;
    NSString *projDir = [[self projectsRootDirectory] stringByAppendingPathComponent:projectId];
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:projDir isDirectory:&isDir] || !isDir) {
        return NO;
    }

    NSString *metaPath = [projDir stringByAppendingPathComponent:@"project_manifest.json"];
    NSDictionary *meta = nil;
    NSData *data = [NSData dataWithContentsOfFile:metaPath];
    if (data) {
        meta = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    }

    _activeProjectId = [projectId copy];
    _activeProjectName = [(meta[@"name"] ?: projectId) copy];

    NSArray<NSDictionary<NSString *, id> *> *scans = [self listScansInActiveProject];
    if (scans.count > 0) {
        return [self selectScanWithId:scans.firstObject[@"id"]];
    }

    // Create default initial scan for newly opened project
    return [self createAndSelectScanWithName:@"Scan 01" floorLevel:@"Level 01" mode:AVScanAcquisitionModeMobileSlam outScanId:nil];
}

- (NSArray<NSDictionary<NSString *, id> *> *)listScansInActiveProject {
    if (_activeProjectId.length == 0) return @[];
    NSString *projDir = [[self projectsRootDirectory] stringByAppendingPathComponent:_activeProjectId];
    NSString *scansDir = [projDir stringByAppendingPathComponent:@"scans"];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray<NSString *> *entries = [fm contentsOfDirectoryAtPath:scansDir error:nil] ?: @[];
    NSMutableArray<NSDictionary<NSString *, id> *> *result = [NSMutableArray array];

    for (NSString *entry in entries) {
        NSString *scanDir = [scansDir stringByAppendingPathComponent:entry];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:scanDir isDirectory:&isDir] || !isDir) continue;

        NSString *metaPath = [scanDir stringByAppendingPathComponent:@"scan_manifest.json"];
        NSDictionary *meta = nil;
        NSData *data = [NSData dataWithContentsOfFile:metaPath];
        if (data) {
            meta = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        }
        [result addObject:@{
            @"id": meta[@"id"] ?: entry,
            @"name": meta[@"name"] ?: entry,
            @"floorLevel": meta[@"floorLevel"] ?: @"Level 01",
            @"mode": meta[@"mode"] ?: @"Mobile SLAM",
            @"createdAt": meta[@"createdAt"] ?: @"",
            @"path": scanDir
        }];
    }

    [result sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        return [b[@"createdAt"] compare:a[@"createdAt"]];
    }];
    return result;
}

- (BOOL)createAndSelectScanWithName:(NSString *)scanName floorLevel:(NSString *)floorLevel mode:(AVScanAcquisitionMode)mode outScanId:(NSString **)outScanId {
    if (_activeProjectId.length == 0) {
        [self createProjectWithName:@"Default Project" site:@"" notes:@"" outProjectId:nil];
    }
    NSString *displayName = (scanName.length > 0) ? scanName : @"Scan";
    NSString *level = (floorLevel.length > 0) ? floorLevel : @"Level 01";
    NSString *slug = [self sanitizeIdentifier:displayName fallback:@"SCAN"];
    NSDateFormatter *df = [[NSDateFormatter alloc] init];
    df.dateFormat = @"yyyyMMdd_HHmmss";
    NSString *scanId = [NSString stringWithFormat:@"%@_%@", slug, [df stringFromDate:[NSDate date]]];

    NSString *projDir = [[self projectsRootDirectory] stringByAppendingPathComponent:_activeProjectId];
    NSString *scanDir = [[projDir stringByAppendingPathComponent:@"scans"] stringByAppendingPathComponent:scanId];
    [[NSFileManager defaultManager] createDirectoryAtPath:scanDir withIntermediateDirectories:YES attributes:nil error:nil];

    NSString *modeStr = (mode == AVScanAcquisitionModeTerrestrial) ? @"Terrestrial" : @"Mobile SLAM";
    NSISO8601DateFormatter *iso = [[NSISO8601DateFormatter alloc] init];
    NSDictionary *manifest = @{
        @"id": scanId,
        @"name": displayName,
        @"floorLevel": level,
        @"mode": modeStr,
        @"createdAt": [iso stringFromDate:[NSDate date]]
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingPrettyPrinted error:nil];
    if (json) {
        [json writeToFile:[scanDir stringByAppendingPathComponent:@"scan_manifest.json"] atomically:YES];
    }

    if (outScanId) *outScanId = scanId;
    return [self selectScanWithId:scanId];
}

- (BOOL)selectScanWithId:(NSString *)scanId {
    if (_activeProjectId.length == 0 || scanId.length == 0) return NO;
    if (_isScanning) {
        [self stopScanningSession];
    }
    NSString *projDir = [[self projectsRootDirectory] stringByAppendingPathComponent:_activeProjectId];
    NSString *scanDir = [[projDir stringByAppendingPathComponent:@"scans"] stringByAppendingPathComponent:scanId];
    BOOL isDir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:scanDir isDirectory:&isDir] || !isDir) {
        return NO;
    }

    NSString *metaPath = [scanDir stringByAppendingPathComponent:@"scan_manifest.json"];
    NSDictionary *meta = nil;
    NSData *data = [NSData dataWithContentsOfFile:metaPath];
    if (data) {
        meta = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    }

    _activeScanId = [scanId copy];
    _activeScanName = [(meta[@"name"] ?: scanId) copy];
    _activeScanFloorLevel = [(meta[@"floorLevel"] ?: @"Level 01") copy];
    _activeProjectDir = [scanDir copy];

    std::lock_guard<std::mutex> lock(_stateMutex);
    _slamEngine.reset();
    _synchronizer.reset();
    _poseGraph.clear();
    _scDatabase.clear();
    _cachedGlobalCloud.clear();
    _liveDisplayVoxelIndex.clear();
    _keyframeTraveledDist.clear();
    _poiPhotos.clear();
    _nextKeyframeId = 1;
    _unclosedTrajectoryDistM = 0.0;
    _totalTraveledDistM = 0.0;
    _lastLoopKeyframeId = 0;
    _prevOdomPos = avscan::math::Vec3d(0.0, 0.0, 0.0);
    _hasPrevOdomPos = false;

    return _projectStore.initializeProject(
        scanDir.UTF8String,
        _activeProjectId.UTF8String,
        _activeScanName.UTF8String) ? YES : NO;
}

- (AVCameraAcquisitionMode)preferredCameraModeForActiveProfileUseArkit:(BOOL)useArkit {
    const auto active = avscan::core::DeviceProfileRegistry::instance().activeProfile();
    if (active.uses_external_usb_camera) {
        NSString *hint = [NSString stringWithUTF8String:active.preferred_usb_camera_name.c_str()];
        [_cameraManager setPreferredExternalCameraName:hint fallbackIntrinsics:active.default_intrinsics];
        // ScanAR external rig: NEVER open the built-in iPad camera or ARKit
        return AVCameraAcquisitionModeExternalUsbUvc;
    }
    return useArkit ? AVCameraAcquisitionModeARKitSession : AVCameraAcquisitionModeAVFoundationPure;
}

- (void)resetAutoCalibration {
    const auto active = avscan::core::DeviceProfileRegistry::instance().activeProfile();
    if (active.uses_external_usb_camera) {
        // ScanAR external rig is physically decoupled from iPad's built-in camera/ARKit;
        // re-seed from empirically validated ScanAR V4 336L+Airy extrinsics for DirectVLCalib refinement.
        _forceScratchAutoCalib = NO;
        auto ext = avscan::core::DeviceProfileRegistry::instance().activeExtrinsics();
        [self applyCalibrationProfile:ext];
        _autoCalibrator.seedFromMountProfile(ext);
        return;
    }
    _forceScratchAutoCalib = YES;
    _autoCalibrator.reset();
    std::lock_guard<std::mutex> lock(_stateMutex);
    _calibProfile.verified = false;
    _calibProfile.confidence_score = 0.0;
    _slamEngine.setVisualFusionEnabled(false);
    _slamEngine.setExtrinsicCalibration(_calibProfile);
}

- (BOOL)createOrOpenProjectWithId:(NSString *)projectId name:(NSString *)projectName {
    NSArray<NSDictionary<NSString *, id> *> *existing = [self listProjects];
    if (existing.count > 0) {
        return [self openProjectWithId:existing.firstObject[@"id"]];
    }
    return [self createProjectWithName:(projectName.length > 0 ? projectName : @"Project 001")
                                  site:@"Default Site"
                                 notes:@""
                          outProjectId:nil];
}

- (BOOL)startScanningSessionWithAutoCalib:(BOOL)autoCalib {
    if (_isScanning) return YES;

    const auto active = avscan::core::DeviceProfileRegistry::instance().activeProfile();
    // Only run blind from-scratch ARKit Hand-Eye discovery when explicitly requested via
    // "Reset & Re-Run Auto-Calib" on a built-in iPad/iPhone camera rig (never on ScanAR external USB rig).
    if (autoCalib && _forceScratchAutoCalib && !active.uses_external_usb_camera) {
        _autoCalibrator.reset();
        _autoCalibActive.store(true);
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _calibProfile.verified = false;
            _calibProfile.confidence_score = 0.0;
        }
        return [self startScanningSessionWithVisualFusion:NO useArkitCamera:YES];
    }

    // Standard Profile-First Mode (iPad rigs & ScanAR V4 Airy + USB Gemini 336L):
    // Respect the active Device Mount Profile immediately from Frame 1 so RGB colorization engages
    // on 100% of scans, while bounded online DirectVLCalib trims any small offset.
    avscan::core::ExtrinsicCalibrationProfile activeSeed;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (!_calibProfile.verified) {
            _calibProfile = avscan::core::DeviceProfileRegistry::instance().activeExtrinsics();
        }
        _calibProfile.verified = true;
        activeSeed = _calibProfile;
    }
    _autoCalibrator.seedFromMountProfile(activeSeed);
    _autoCalibActive.store(true);

    // Pure LIO geometry + active rig camera (AVFoundation built-in for iPad mounts, External USB UVC for ScanAR 336L)
    return [self startScanningSessionWithVisualFusion:NO useArkitCamera:NO];
}

- (BOOL)startScanningSessionWithVisualFusion:(BOOL)enableVisual useArkitCamera:(BOOL)useArkit {
    if (_isScanning) return YES;
    if (!_activeProjectDir) {
        [self createOrOpenProjectWithId:@"PROJECT_001" name:@"AV Scan Level 01"];
    }

    // Clear stale disk submap chunks from any prior run in this same scan slot so WAL/chunks stay consistent
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *chunksDir = [_activeProjectDir stringByAppendingPathComponent:@"chunks"];
    NSArray<NSString *> *oldChunks = [fm contentsOfDirectoryAtPath:chunksDir error:nil];
    for (NSString *entry in oldChunks) {
        if ([entry hasPrefix:@"submap_"] && [entry hasSuffix:@".bin"]) {
            [fm removeItemAtPath:[chunksDir stringByAppendingPathComponent:entry] error:nil];
        }
    }

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        // Clean session state so previous scan velocity, poses, and stale voxels never bleed into a new scan
        _slamEngine.reset();
        _synchronizer.reset();
        _poseGraph.clear();
        _scDatabase.clear();
        _cachedGlobalCloud.clear();
        _liveDisplayVoxelIndex.clear();
        _keyframeTraveledDist.clear();
        _nextKeyframeId = 1;
        _unclosedTrajectoryDistM = 0.0;
        _totalTraveledDistM = 0.0;
        _lastLoopKeyframeId = 0;
        _prevOdomPos = avscan::math::Vec3d(0.0, 0.0, 0.0);
        _hasPrevOdomPos = false;

        _projectStore.initializeProject(
            _activeProjectDir.UTF8String,
            (_activeProjectId ?: @"PROJECT_001").UTF8String,
            (_activeScanName ?: @"Scan 01").UTF8String);

        _slamEngine.setFactoryCalibration(_factoryCalib);
        _slamEngine.setVisualFusionEnabled(enableVisual ? true : false);
        _slamEngine.setExtrinsicCalibration(_calibProfile);
        _synchronizer.setCalibratedCameraTimeOffset(_calibProfile.camera_time_offset_sec);
    }
    _resourceController.setOperatingMode(avscan::core::OperatingMode::MAPPING_DENSE);

    NSString *rawPath = [_activeProjectDir stringByAppendingPathComponent:@"raw_capture.avraw"];
    _udpReceiver->startRawRecording(rawPath.UTF8String);
    _udpReceiver->start();

    _scanStartHostTime = CACurrentMediaTime();
    _scanStartCamFrames = _cameraManager.framesDelivered;
    _cameraWatchdogKicked = NO;

    AVCameraAcquisitionMode camMode = [self preferredCameraModeForActiveProfileUseArkit:useArkit];
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

        // Camera engagement watchdog: if 2.5 s have elapsed since Start Scan with zero RGB frames delivered,
        // automatically re-engage the active profile's camera mode (External USB 336L or built-in AVFoundation).
        if (!_cameraWatchdogKicked && _scanStartHostTime > 0.0 &&
            (CACurrentMediaTime() - _scanStartHostTime) > 2.5) {
            if (_cameraManager.framesDelivered == _scanStartCamFrames && _cameraManager.cameraAuthorized) {
                _cameraWatchdogKicked = YES;
                AVCameraAcquisitionMode fallbackMode = [self preferredCameraModeForActiveProfileUseArkit:NO];
                [_cameraManager startCaptureWithMode:fallbackMode];
            } else if (_cameraManager.framesDelivered > _scanStartCamFrames) {
                _cameraWatchdogKicked = YES;
            }
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
        if (_hasPrevOdomPos) {
            const double stepDist = (state.T_world_body.t - _prevOdomPos).norm();
            if (stepDist < 2.0) {
                _totalTraveledDistM += stepDist;
                _unclosedTrajectoryDistM += stepDist;
            }
        }
        _prevOdomPos = state.T_world_body.t;
        _hasPrevOdomPos = true;

        _latestQuality = _qualityMonitor.evaluateFrameQuality(state, metrics, _unclosedTrajectoryDistM);

        // Incrementally accumulate a single unified 3 cm voxel cloud on every 10 Hz sweep
        // (upgrading previously gray voxels in place once verified RGB observes them)
        mergePointsIntoLiveCloud(worldFrame.points, 3, _cachedGlobalCloud, _liveDisplayVoxelIndex);

        if (_autoCalibActive.load()) {
            const avscan::math::SE3d T_world_lidar = state.T_world_body * _factoryCalib.T_imu_lidar;
            const avscan::math::SE3d T_lidar_body = _factoryCalib.T_imu_lidar.inverse();

            // Convert motion-compensated body-frame points back to LiDAR frame for DirectVLCalib
            avscan::sync::SyncBundle calibBundle;
            calibBundle.lidar_frame.frame_id = deskewedBody.frame_id;
            calibBundle.lidar_frame.timestamp_start_sec = deskewedBody.timestamp_start_sec;
            calibBundle.lidar_frame.timestamp_end_sec = deskewedBody.timestamp_end_sec;
            calibBundle.imu_measurements = bundle.imu_measurements;
            calibBundle.rgb_frame = bundle.rgb_frame;
            calibBundle.estimated_cam_to_lidar_offset_sec = bundle.estimated_cam_to_lidar_offset_sec;
            calibBundle.lidar_frame.points.reserve(deskewedBody.points.size() / 4 + 1);
            for (size_t i = 0; i < deskewedBody.points.size(); i += 4) {
                auto pt = deskewedBody.points[i];
                const avscan::math::Vec3d p_l = T_lidar_body * avscan::math::Vec3d(pt.x, pt.y, pt.z);
                pt.x = p_l.x;
                pt.y = p_l.y;
                pt.z = p_l.z;
                calibBundle.lidar_frame.points.push_back(pt);
            }

            if (_autoCalibrator.pushLidarBundleAndPose(calibBundle, T_world_lidar)) {
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
                            bool solved = strongSelf->_autoCalibrator.completeSolve(
                                motions, vlPairs, imuNorms, camNorms,
                                deviceModel, "universal_clamp_auto", solvedProfile);
                            if (solved && solvedProfile.verified) {
                                std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
                                if (strongSelf->_isScanning) {
                                    // Enable accurate SLERP RGB colorization while keeping geometry pure LIO
                                    strongSelf->_forceScratchAutoCalib = NO;
                                    strongSelf->_calibProfile = solvedProfile;
                                    strongSelf->_slamEngine.setExtrinsicCalibration(solvedProfile);
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
            submap.local_points = deskewedBody.points;  // Consistent sweep-end IMU body frame

            _projectStore.appendKeyframeToWal(submap);
            _poseGraph.addKeyframeSubmap(submap);
            _keyframeTraveledDist[submap.keyframe_id] = _totalTraveledDistM;

            // Gravity-leveled ScanContext descriptor so tilted Airy scans remain invariant
            auto scDesc = avscan::backend::ScanContextDatabase::computeDescriptor(
                submap.keyframe_id, submap.timestamp_sec, submap.local_points,
                bundle.rgb_frame, state.T_world_body.so3);
            auto loopCand = _scDatabase.detectLoopClosure(scDesc);
            _scDatabase.addKeyframeDescriptor(scDesc);

            const bool cooldownOk = (_lastLoopKeyframeId == 0) ||
                                    ((submap.keyframe_id > _lastLoopKeyframeId) &&
                                     ((submap.keyframe_id - _lastLoopKeyframeId) >= 10));

            if (loopCand.valid && cooldownOk) {
                for (auto &targetSm : _poseGraph.getMutableSubmaps()) {
                    if (targetSm.keyframe_id == loopCand.match_keyframe_id) {
                        const uint64_t kfGap = (submap.keyframe_id > targetSm.keyframe_id)
                                                   ? (submap.keyframe_id - targetSm.keyframe_id)
                                                   : 0;
                        auto distIt = _keyframeTraveledDist.find(targetSm.keyframe_id);
                        const double traveledGapM = (distIt != _keyframeTraveledDist.end())
                                                        ? (_totalTraveledDistM - distIt->second)
                                                        : 0.0;
                        const double spatialDistM = (submap.T_world_keyframe.t - targetSm.T_world_keyframe.t).norm();

                        // Strict temporal, traveled-distance, and spatial gating before GICP
                        if (kfGap >= 30 && traveledGapM >= 8.0 && spatialDistM < 4.5) {
                            if (targetSm.local_points.empty()) {
                                _projectStore.loadSubmapFromDisk(targetSm);
                            }
                            const avscan::math::SE3d initGuess =
                                targetSm.T_world_keyframe.inverse() * submap.T_world_keyframe;
                            auto ver = _gicpVerifier.verifyLoopConstraint(
                                submap.local_points, targetSm.local_points, initGuess);
                            if (ver.verified) {
                                const avscan::math::SE3d delta = initGuess.inverse() * ver.T_target_source;
                                const double deltaRotDeg = delta.so3.log().norm() * (180.0 / 3.14159265358979323846);
                                const double deltaTransM = delta.t.norm();
                                if (deltaRotDeg < 20.0 && deltaTransM < 2.0) {
                                    _poseGraph.addVerifiedLoopFactor(
                                        targetSm.keyframe_id,
                                        submap.keyframe_id,
                                        ver.T_target_source,
                                        ver.information_diagonal,
                                        true);
                                    _poseGraph.optimizeGlobalPoseGraph(15);
                                    _lastLoopKeyframeId = submap.keyframe_id;
                                    _unclosedTrajectoryDistM = 0.0;
                                    // Decouple frontend LIO from backend PGO (never teleport frontend away from local_map_)
                                    // Rebuild the unified live display cloud on the optimized pose graph
                                    _cachedGlobalCloud = _poseGraph.buildGloballyConsistentPointCloud(2, kLiveDisplayVoxelM);
                                    rebuildLiveVoxelIndex(_cachedGlobalCloud, _liveDisplayVoxelIndex);
                                }
                            }
                        }
                        break;
                    }
                }
            }

            _latestFloorPlan = avscan::quality::QualityMonitor::generateLiveFloorPlan(
                _cachedGlobalCloud, _poseGraph.getSubmaps(), _poseGraph.getFactors(), 0.10);

            // Collaborative SLAM: register local keyframe and broadcast to connected iOS / Jetson / Windows peers
            if (_isCslamEnabled && _cslamEngine) {
                _cslamEngine->addLocalKeyframe(submap, scDesc);
                auto kfPkt = _cslamEngine->serializeKeyframeSync(submap, scDesc, 1200);
                if (_cslamLanTransport && _cslamLanTransport->isRunning()) {
                    _cslamLanTransport->broadcastTcpPacket(kfPkt);
                }
                if (_multipeerTransport && _multipeerTransport.isRunning) {
                    [_multipeerTransport broadcastData:kfPkt reliable:YES];
                }
            }
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
    if (!_poseGraph.getSubmaps().empty()) {
        _poseGraph.optimizeGlobalPoseGraph(40);
        // Full Tier-2 Architectural Cleanup Pipeline:
        // 1. Dynamic transient ray-carving -> 2. 1.5 cm voxel centroid + weighted RGB fusion ->
        // 3. O(N) spatial-hash ROR + SOR -> 4. Edge-preserving planar denoising -> 5. Guarded floor tilt leveling
        avscan::processing::CleanupConfig cleanupCfg;
        std::vector<avscan::core::LidarPointXYZIRT> cleaned;
        _lastCleanupStats = avscan::processing::PointCloudCleanup::runPostScanPipeline(
            _poseGraph.getSubmaps(), cleaned, cleanupCfg);
        if (_lastCleanupStats.floor_leveled) {
            // Keep keyframe poses & POI photos locked in the exact same leveled datum as the exported cloud
            const avscan::math::SE3d& T_lvl = _lastCleanupStats.T_level_world;
            for (auto &sm : _poseGraph.getMutableSubmaps()) {
                sm.T_world_keyframe = T_lvl * sm.T_world_keyframe;
            }
            for (auto &poi : _poiPhotos) {
                poi.T_world_camera = T_lvl * poi.T_world_camera;
            }
        }
        if (!cleaned.empty()) {
            _cachedGlobalCloud = std::move(cleaned);
            rebuildLiveVoxelIndex(_cachedGlobalCloud, _liveDisplayVoxelIndex);
        }
    } else if (!_cachedGlobalCloud.empty()) {
        avscan::processing::CleanupConfig cleanupCfg;
        cleanupCfg.enable_dynamic_filter = false;
        std::vector<avscan::core::LidarPointXYZIRT> cleaned;
        _lastCleanupStats = avscan::processing::PointCloudCleanup::runCloudCleanupPipeline(
            _cachedGlobalCloud, cleaned, cleanupCfg);
        if (!cleaned.empty()) {
            _cachedGlobalCloud = std::move(cleaned);
            rebuildLiveVoxelIndex(_cachedGlobalCloud, _liveDisplayVoxelIndex);
        }
    }
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
    rec.T_world_camera = _slamEngine.getCurrentState().T_world_body *
                         _factoryCalib.T_imu_lidar *
                         _calibProfile.T_cam_lidar.inverse();
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
        [[NSUserDefaults standardUserDefaults] setObject:profileId forKey:@"AVScanSelectedDeviceProfileId"];
        [[NSUserDefaults standardUserDefaults] synchronize];
        _forceScratchAutoCalib = NO;
        const auto active = avscan::core::DeviceProfileRegistry::instance().activeProfile();
        if (active.uses_external_usb_camera) {
            NSString *hint = [NSString stringWithUTF8String:active.preferred_usb_camera_name.c_str()];
            [_cameraManager setPreferredExternalCameraName:hint fallbackIntrinsics:active.default_intrinsics];
        }
        auto ext = avscan::core::DeviceProfileRegistry::instance().activeExtrinsics();
        [self applyCalibrationProfile:ext];
        if (_autoCalibActive.load()) {
            _autoCalibrator.seedFromMountProfile(ext);
        }
        if (_isScanning) {
            AVCameraAcquisitionMode targetMode = [self preferredCameraModeForActiveProfileUseArkit:NO];
            if (_cameraManager.activeMode != targetMode) {
                [_cameraManager startCaptureWithMode:targetMode];
            }
        }
        return YES;
    }
    return NO;
}

- (void)detectAndApplyHostDeviceProfile {
    // 1. Respect operator's persisted Device Mount Profile choice across app launches
    NSString *savedId = [[NSUserDefaults standardUserDefaults] stringForKey:@"AVScanSelectedDeviceProfileId"];
    if (savedId.length > 0 && [self setActiveDeviceProfileWithId:savedId]) {
        return;
    }

    // 2. Otherwise exact-match host hardware model (iPad13,16/17 -> Air 5, iPad13,18/19 -> iPad 10, etc.)
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
    if (_isTerrestrialStationActive && _stationManager) {
        auto statPts = _stationManager->buildStationPointCloud();
        if (_isSlamPausedForStation) {
            std::vector<avscan::core::LidarPointXYZIRT> combined = _cachedGlobalCloud;
            for (const auto &p : statPts) {
                avscan::math::Vec3d v(p.x, p.y, p.z);
                avscan::math::Vec3d vw = _flashStationAnchorPose * v;
                avscan::core::LidarPointXYZIRT pw = p;
                pw.x = vw.x; pw.y = vw.y; pw.z = vw.z;
                combined.push_back(pw);
            }
            return combined;
        }
        return statPts;
    }
    std::vector<avscan::core::LidarPointXYZIRT> baseCloud = !_cachedGlobalCloud.empty() ? _cachedGlobalCloud : _livePreviewCloud;
    if (_isCslamEnabled && _cslamEngine) {
        auto peerPts = _cslamEngine->buildAlignedPeerPointCloud(100000);
        if (!peerPts.empty()) {
            baseCloud.insert(baseCloud.end(), peerPts.begin(), peerPts.end());
        }
    }
    return baseCloud;
}

- (std::vector<avscan::core::SlamState>)currentTrajectory {
    std::lock_guard<std::mutex> lock(_stateMutex);
    auto traj = _slamEngine.getTrajectory();
    if (_isCslamEnabled && _cslamEngine) {
        auto peerTraj = _cslamEngine->getAlignedPeerTrajectories();
        if (!peerTraj.empty()) {
            traj.insert(traj.end(), peerTraj.begin(), peerTraj.end());
        }
    }
    return traj;
}

#pragma mark - Collaborative SLAM (C-SLAM) & Depth Compute Tier

- (BOOL)setCollaborativeSlamEnabled:(BOOL)enabled {
    if (enabled == _isCslamEnabled) return YES;
    if (enabled) {
        AVScanCoreBridge * __weak weakSelf = self;
        _cslamLanTransport->setPacketCallback([weakSelf](const uint8_t *data, size_t len, const std::string &senderIp) {
            AVScanCoreBridge *strongSelf = weakSelf;
            if (!strongSelf || !strongSelf->_cslamEngine) return;
            strongSelf->_cslamEngine->ingestRemotePacket(data, len, senderIp, CACurrentMediaTime());
        });
        [_multipeerTransport setPacketCallback:[weakSelf](const uint8_t *data, size_t len, const std::string &senderLabel) {
            AVScanCoreBridge *strongSelf = weakSelf;
            if (!strongSelf || !strongSelf->_cslamEngine) return;
            strongSelf->_cslamEngine->ingestRemotePacket(data, len, senderLabel, CACurrentMediaTime());
        } peerCallback:[weakSelf](const std::string &agentId, const std::string &displayName) {
            AVScanCoreBridge *strongSelf = weakSelf;
            if (!strongSelf || !strongSelf->_cslamEngine) return;
            strongSelf->_cslamEngine->registerExternalPeer(
                agentId, displayName, avscan::cslam::PeerPlatform::IOS, avscan::cslam::PeerApp::AV_IOS, "Bluetooth/P2P Wi-Fi", CACurrentMediaTime());
        }];

        _cslamLanTransport->start();
        [_multipeerTransport start];
        _isCslamEnabled = YES;

        _cslamBeaconRunning.store(true);
        _cslamBeaconThread = std::thread([weakSelf]() {
            while (true) {
                AVScanCoreBridge *strongSelf = weakSelf;
                if (!strongSelf || !strongSelf->_cslamBeaconRunning.load()) break;

                avscan::math::SE3d pose;
                uint32_t kfCount = 0;
                {
                    std::lock_guard<std::mutex> lock(strongSelf->_stateMutex);
                    pose = strongSelf->_slamEngine.getCurrentState().T_world_body;
                    kfCount = static_cast<uint32_t>(strongSelf->_poseGraph.getSubmaps().size());
                }
                auto beacon = strongSelf->_cslamEngine->serializeBeacon(pose, kfCount);
                if (strongSelf->_cslamLanTransport && strongSelf->_cslamLanTransport->isRunning()) {
                    strongSelf->_cslamLanTransport->broadcastUdpPacket(beacon);
                }
                if (strongSelf->_multipeerTransport && strongSelf->_multipeerTransport.isRunning) {
                    [strongSelf->_multipeerTransport broadcastData:beacon reliable:NO];
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        });
    } else {
        _isCslamEnabled = NO;
        if (_cslamBeaconRunning.exchange(false)) {
            if (_cslamBeaconThread.joinable()) _cslamBeaconThread.join();
        }
        if (_cslamLanTransport) _cslamLanTransport->stop();
        [_multipeerTransport stop];
    }
    return YES;
}

- (void)connectCollaborativePeerByIp:(NSString *)ipAddress {
    if (ipAddress.length == 0) return;
    if (!_isCslamEnabled) {
        [self setCollaborativeSlamEnabled:YES];
    }
    if (_cslamLanTransport) {
        _cslamLanTransport->addDirectPeerIp(ipAddress.UTF8String);
        avscan::math::SE3d pose;
        uint32_t kfCount = 0;
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            pose = _slamEngine.getCurrentState().T_world_body;
            kfCount = static_cast<uint32_t>(_poseGraph.getSubmaps().size());
        }
        auto beacon = _cslamEngine->serializeBeacon(pose, kfCount);
        _cslamLanTransport->broadcastUdpPacket(beacon);
    }
}

- (void)syncSharedOriginWithPeers {
    if (!_cslamEngine) return;
    _cslamEngine->syncSharedOriginWithAllPeers();
    auto pkt = _cslamEngine->serializeSharedOriginSync();
    if (_cslamLanTransport && _cslamLanTransport->isRunning()) {
        _cslamLanTransport->broadcastUdpPacket(pkt);
        _cslamLanTransport->broadcastTcpPacket(pkt);
    }
    if (_multipeerTransport && _multipeerTransport.isRunning) {
        [_multipeerTransport broadcastData:pkt reliable:YES];
    }
}

- (NSArray<NSDictionary<NSString *, id> *> *)connectedCollaborativePeers {
    if (!_cslamEngine) return @[];
    auto peers = _cslamEngine->getPeers(CACurrentMediaTime(), 20.0);
    NSMutableArray *arr = [NSMutableArray arrayWithCapacity:peers.size()];
    for (const auto &p : peers) {
        [arr addObject:@{
            @"id": [NSString stringWithUTF8String:p.agent_id.c_str()],
            @"name": [NSString stringWithUTF8String:p.display_name.c_str()],
            @"ip": [NSString stringWithUTF8String:p.ip_address.c_str()],
            @"platform": [NSString stringWithUTF8String:avscan::cslam::CollaborativeSlamEngine::platformToString(p.platform).c_str()],
            @"app": [NSString stringWithUTF8String:avscan::cslam::CollaborativeSlamEngine::appToString(p.app).c_str()],
            @"connected": @(p.connected),
            @"aligned": @(p.aligned),
            @"keyframes": @(p.keyframe_count)
        }];
    }
    return arr;
}

- (avscan::cslam::CslamStats)collaborativeSlamStats {
    return _cslamEngine ? _cslamEngine->getStats() : avscan::cslam::CslamStats{};
}

- (avscan::sensors::DepthComputeTier)recommendedDepthComputeTier {
    struct utsname sysInfo {};
    std::string machine = "Generic";
    if (uname(&sysInfo) == 0 && sysInfo.machine[0]) {
        machine = sysInfo.machine;
    }
    return avscan::sensors::LidarAnchoredDepthAligner::computeTierForHardware(machine);
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

- (double)cameraMeasuredFps {
    return _cameraManager ? _cameraManager.measuredFps : 0.0;
}

- (uint64_t)cameraFramesDelivered {
    return _cameraManager ? _cameraManager.framesDelivered : 0;
}

- (NSString *)cameraStatusSummary {
    return _cameraManager ? _cameraManager.statusSummary : @"IDLE";
}

- (BOOL)isRgbColorizationActive {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return (_calibProfile.verified && _cameraManager && _cameraManager.measuredFps >= 1.0) ? YES : NO;
}

- (BOOL)activeProfileUsesExternalUsbCamera {
    return avscan::core::DeviceProfileRegistry::instance().activeProfile().uses_external_usb_camera ? YES : NO;
}

- (NSArray<NSString *> *)connectedExternalUsbCameras {
    return _cameraManager ? [_cameraManager connectedExternalCameraNames] : @[];
}

- (avscan::processing::CleanupStats)lastCleanupStats {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _lastCleanupStats;
}

#pragma mark - Terrestrial Mode Implementation (Mode B) & Hybrid Flash Station

- (NSString *)nextAvailableStationIdWithPrefix:(NSString *)prefix {
    NSString *pfx = (prefix.length > 0) ? prefix : @"st";
    if (!_activeProjectDir) {
        [self createOrOpenProjectWithId:@"PROJECT_001" name:@"AV Terrestrial Session"];
    }
    NSString *stationsRoot = [_activeProjectDir stringByAppendingPathComponent:@"stations"];
    NSFileManager *fm = [NSFileManager defaultManager];
    for (int idx = 1; idx <= 999; ++idx) {
        NSString *candidate = [NSString stringWithFormat:@"%@_%03d", pfx, idx];
        NSString *path = [stationsRoot stringByAppendingPathComponent:candidate];
        if (![fm fileExistsAtPath:path]) {
            return candidate;
        }
    }
    return [NSString stringWithFormat:@"%@_%llu", pfx, (unsigned long long)time(NULL)];
}

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

    NSString *resolvedId = (stationId.length > 0) ? stationId : [self nextAvailableStationIdWithPrefix:(isFlash ? @"flash_st" : @"st")];
    _activeTerrestrialStationId = [resolvedId copy];
    _lastCapturedRgbStopIndex = -1;

    avscan::terrestrial::TerrestrialStationConfig config;
    config.project_id = _activeProjectId ? _activeProjectId.UTF8String : "PROJ_001";
    config.station_id = resolvedId.UTF8String;
    config.station_name = stationName ? stationName.UTF8String : resolvedId.UTF8String;
    if (isFlash) {
        // Single-shot stationary Flash Scan during Mobile SLAM:
        // Zero motor/turntable required, 1 stationary pose in exact IMU body frame, auto-completes after 5 s
        config.is_single_shot_flash = true;
        config.nominal_step_deg = 360.0;
        config.total_expected_stops = 1;
        config.accumulation_sec_per_stop = 5.0;
        config.settling_delay_sec = 0.6;
        config.enable_overlap_refinement = false;
        std::lock_guard<std::mutex> lock(_stateMutex);
        config.T_imu_lidar = _factoryCalib.T_imu_lidar;
    } else {
        // Dedicated Mode B Terrestrial Station: multi-stop 360-degree tripod capture (manual detent or motorized head)
        config.is_single_shot_flash = false;
        config.nominal_step_deg = (stepDeg > 5.0) ? stepDeg : 60.0;
        config.total_expected_stops = static_cast<size_t>(std::round(360.0 / config.nominal_step_deg));
        config.accumulation_sec_per_stop = 5.0;
    }

    // Respect live/calibrated extrinsics for variable detachable mounts rather than overwriting with static defaults
    auto mountProfile = avscan::core::DeviceProfileRegistry::instance().activeProfile();
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        if (_calibProfile.verified || _calibProfile.confidence_score > 0.0) {
            mountProfile.T_cam_lidar = _calibProfile.T_cam_lidar;
            mountProfile.camera_time_offset_sec = _calibProfile.camera_time_offset_sec;
        }
    }

    _stationManager = std::make_unique<avscan::terrestrial::TerrestrialStationManager>(config);
    _stationManager->startNewStation(_activeProjectDir.UTF8String, config, mountProfile);

    _udpReceiver->start();
    if (!_isSlamPausedForStation) {
        // Start active rig camera for per-stop RGB frames (External USB 336L for ScanAR, built-in for iPad mounts)
        AVCameraAcquisitionMode stationCamMode = [self preferredCameraModeForActiveProfileUseArkit:NO];
        [_cameraManager startCaptureWithMode:stationCamMode];
    }
    _isTerrestrialStationActive = YES;
    return YES;
}

- (BOOL)beginTerrestrialStop {
    if (!_stationManager) return NO;
    _lastCapturedRgbStopIndex = -1;
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
    _lastCapturedRgbStopIndex = -1;
    return _stationManager->repeatCurrentStop() ? YES : NO;
}

- (BOOL)finalizeAndSaveTerrestrialStation {
    if (!_stationManager) return NO;
    std::string err;
    BOOL ok = _stationManager->finalizeAndSaveStation(err) ? YES : NO;
    if (ok) {
        std::lock_guard<std::mutex> lock(_stateMutex);
        auto statCloud = _stationManager->buildStationPointCloud();
        if (_isSlamPausedForStation) {
            // Create high-weight anchor keyframe in SLAM pose graph
            avscan::core::KeyframeSubmap anchorSubmap;
            anchorSubmap.keyframe_id = _nextKeyframeId++;
            anchorSubmap.timestamp_sec = CACurrentMediaTime();
            anchorSubmap.T_world_keyframe = _flashStationAnchorPose;
            anchorSubmap.local_points = statCloud;

            _projectStore.appendKeyframeToWal(anchorSubmap);
            _poseGraph.addKeyframeSubmap(anchorSubmap);

            _cachedGlobalCloud = _poseGraph.buildGloballyConsistentPointCloud(2, kLiveDisplayVoxelM);
            rebuildLiveVoxelIndex(_cachedGlobalCloud, _liveDisplayVoxelIndex);
            _latestFloorPlan = avscan::quality::QualityMonitor::generateLiveFloorPlan(
                _cachedGlobalCloud, _poseGraph.getSubmaps(), _poseGraph.getFactors(), 0.08);

            [self resumeSlamAfterFlashStation];
        } else {
            avscan::processing::CleanupConfig stCfg;
            stCfg.enable_dynamic_filter = false;
            stCfg.voxel_size_m = 0.010;  // 1 cm ultra-dense fusion for stationary terrestrial scans
            std::vector<avscan::core::LidarPointXYZIRT> cleanedStation;
            _lastCleanupStats = avscan::processing::PointCloudCleanup::runCloudCleanupPipeline(
                statCloud, cleanedStation, stCfg);
            _cachedGlobalCloud = cleanedStation.empty() ? statCloud : std::move(cleanedStation);
            std::vector<avscan::core::KeyframeSubmap> stSubmaps;
            avscan::core::KeyframeSubmap sm;
            sm.keyframe_id = 1;
            sm.T_world_keyframe = avscan::math::SE3d::Identity();
            sm.local_points = statCloud;
            stSubmaps.push_back(sm);
            std::vector<avscan::backend::PoseGraphFactor> emptyFactors;
            _latestFloorPlan = avscan::quality::QualityMonitor::generateLiveFloorPlan(
                _cachedGlobalCloud, stSubmaps, emptyFactors, 0.08);
            _isTerrestrialStationActive = NO;
            [_cameraManager stopCapture];
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

- (size_t)completedTerrestrialStopCount {
    return _stationManager ? _stationManager->getCompletedStops().size() : 0;
}

- (double)terrestrialAccumulationProgress {
    return _stationManager ? _stationManager->getCurrentAccumulationProgress() : 0.0;
}

- (BOOL)isTerrestrialStationary {
    return _stationManager ? (_stationManager->isStationary() ? YES : NO) : NO;
}

- (avscan::terrestrial::TerrestrialCaptureState)currentTerrestrialCaptureState {
    return _stationManager ? _stationManager->getCaptureState() : avscan::terrestrial::TerrestrialCaptureState::IDLE;
}

- (avscan::terrestrial::TerrestrialStationMetadata)currentTerrestrialStationMetadata {
    return _stationManager ? _stationManager->getStationMetadata() : avscan::terrestrial::TerrestrialStationMetadata{};
}

@end
