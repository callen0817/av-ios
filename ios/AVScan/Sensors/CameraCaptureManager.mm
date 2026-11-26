#import "CameraCaptureManager.h"
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <dlfcn.h>
#include <mutex>

namespace {

// Runtime-safe iOS 17+ External USB UVC device type lookup without requiring Apple compiler-rt __isPlatformVersionAtLeast
bool isIos17OrNewer() {
    NSOperatingSystemVersion v17 = {17, 0, 0};
    return [[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:v17] ? true : false;
}

AVCaptureDeviceType externalUsbCameraDeviceType() {
    if (!isIos17OrNewer()) {
        return nil;
    }
    void *sym = dlsym(RTLD_DEFAULT, "AVCaptureDeviceTypeExternal");
    if (sym) {
        AVCaptureDeviceType val = *reinterpret_cast<AVCaptureDeviceType __unsafe_unretained *>(sym);
        if (val) return val;
    }
    return @"AVCaptureDeviceTypeExternal";
}

}  // namespace

@implementation CameraCaptureManager {
    AVCaptureSession *_avSession;
    AVCaptureDevice *_videoDevice;
    dispatch_queue_t _captureQueue;
    dispatch_queue_t _sessionQueue;
    ARSession *_arSession;

    std::mutex _cbMutex;
    std::function<void(const avscan::core::RgbFrame &)> _onFrame;
    std::function<void(double, const avscan::math::SE3d &)> _onDiagPose;

    std::atomic<uint64_t> _frameCounter;
    std::atomic<uint64_t> _sessionGeneration;
    std::atomic<bool> _runningAtomic;
    std::atomic<bool> _authorizedAtomic;

    double _lastProcessedTimestamp;
    double _lastFrameHostTime;
    double _fpsWindowStart;
    uint32_t _fpsWindowFrames;
    double _measuredFpsValue;
    NSString *_statusSummaryValue;
    NSString *_preferredExternalCamName;
    NSString *_connectedExternalCamNameValue;
    avscan::core::CameraIntrinsics _externalFallbackIntrinsics;
    BOOL _hasExternalFallbackIntrinsics;
    BOOL _waitingForExternalCamera;
    uint64_t _waitingGeneration;
    avscan::core::SlamState _latestArkitDiag;
}

- (instancetype)initWithMode:(AVCameraAcquisitionMode)mode {
    self = [super init];
    if (self) {
        _activeMode = mode;
        _targetFrameRate = 30;
        _frameCounter = 0;
        _sessionGeneration = 0;
        _runningAtomic = false;
        _authorizedAtomic = ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] == AVAuthorizationStatusAuthorized);
        _lastProcessedTimestamp = 0.0;
        _lastFrameHostTime = 0.0;
        _fpsWindowStart = 0.0;
        _fpsWindowFrames = 0;
        _measuredFpsValue = 0.0;
        _statusSummaryValue = @"IDLE";
        _preferredExternalCamName = @"";
        _connectedExternalCamNameValue = @"";
        _hasExternalFallbackIntrinsics = NO;
        _waitingForExternalCamera = NO;
        _waitingGeneration = 0;
        _captureQueue = dispatch_queue_create("com.artificialvision.avscan.camera", DISPATCH_QUEUE_SERIAL);
        _sessionQueue = dispatch_queue_create("com.artificialvision.avscan.camera.session", DISPATCH_QUEUE_SERIAL);

        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(onAvSessionRuntimeError:)
                                                     name:AVCaptureSessionRuntimeErrorNotification
                                                   object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(onAvSessionInterruptionEnded:)
                                                     name:AVCaptureSessionInterruptionEndedNotification
                                                   object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(onCaptureDeviceConnected:)
                                                     name:AVCaptureDeviceWasConnectedNotification
                                                   object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(onCaptureDeviceDisconnected:)
                                                     name:AVCaptureDeviceWasDisconnectedNotification
                                                   object:nil];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self stopCapture];
}

- (BOOL)isRunning {
    return _runningAtomic.load() ? YES : NO;
}

- (BOOL)cameraAuthorized {
    return _authorizedAtomic.load() ? YES : NO;
}

- (uint64_t)framesDelivered {
    return _frameCounter.load();
}

- (double)measuredFps {
    std::lock_guard<std::mutex> lock(_cbMutex);
    if (!_runningAtomic.load()) return 0.0;
    double now = CACurrentMediaTime();
    if (_lastFrameHostTime > 0.0 && (now - _lastFrameHostTime) > 1.5) {
        return 0.0;
    }
    return _measuredFpsValue;
}

- (NSString *)statusSummary {
    std::lock_guard<std::mutex> lock(_cbMutex);
    return _statusSummaryValue ?: @"IDLE";
}

- (NSString *)connectedExternalCameraName {
    std::lock_guard<std::mutex> lock(_cbMutex);
    return _connectedExternalCamNameValue ?: @"";
}

- (void)setPreferredExternalCameraName:(NSString *)nameHint
                    fallbackIntrinsics:(const avscan::core::CameraIntrinsics &)intrinsics {
    std::lock_guard<std::mutex> lock(_cbMutex);
    _preferredExternalCamName = nameHint ? [nameHint copy] : @"";
    _externalFallbackIntrinsics = intrinsics;
    _hasExternalFallbackIntrinsics = (intrinsics.width > 0 && intrinsics.height > 0 && intrinsics.fx > 10.0) ? YES : NO;
}

- (NSArray<NSString *> *)connectedExternalCameraNames {
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    AVCaptureDeviceType extType = externalUsbCameraDeviceType();
    if (extType) {
        AVCaptureDeviceDiscoverySession *disc = [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[extType]
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];
        for (AVCaptureDevice *dev in disc.devices) {
            if (dev.localizedName.length > 0) {
                [names addObject:dev.localizedName];
            }
        }
    }
    return names;
}

- (void)setStatusSummaryText:(NSString *)text {
    std::lock_guard<std::mutex> lock(_cbMutex);
    _statusSummaryValue = [text copy];
}

- (void)requestCameraPermissionIfNeeded {
    AVAuthorizationStatus auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (auth == AVAuthorizationStatusAuthorized) {
        _authorizedAtomic.store(true);
        return;
    }
    if (auth == AVAuthorizationStatusNotDetermined) {
        CameraCaptureManager * __weak weakSelf = self;
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
            CameraCaptureManager *strongSelf = weakSelf;
            if (!strongSelf) return;
            strongSelf->_authorizedAtomic.store(granted ? true : false);
            if (!granted) {
                [strongSelf setStatusSummaryText:@"CAM DENIED"];
            }
        }];
    } else {
        _authorizedAtomic.store(false);
        [self setStatusSummaryText:@"CAM DENIED"];
    }
}

- (void)setFrameCallback:(std::function<void(const avscan::core::RgbFrame &)>)frameCallback
   diagnosticPoseCallback:(std::function<void(double, const avscan::math::SE3d &)>)diagPoseCallback {
    std::lock_guard<std::mutex> lock(_cbMutex);
    _onFrame = std::move(frameCallback);
    _onDiagPose = std::move(diagPoseCallback);
}

- (void)stopCaptureLockedOnSessionQueue {
    if (_arSession) {
        _arSession.delegate = nil;
        [_arSession pause];
        _arSession = nil;
    }
    if (_avSession) {
        if (_avSession.isRunning) {
            [_avSession stopRunning];
        }
        _avSession = nil;
    }
    _videoDevice = nil;
    _waitingForExternalCamera = NO;
    _runningAtomic.store(false);
    {
        std::lock_guard<std::mutex> lock(_cbMutex);
        _lastProcessedTimestamp = 0.0;
        _lastFrameHostTime = 0.0;
        _fpsWindowStart = 0.0;
        _fpsWindowFrames = 0;
        _measuredFpsValue = 0.0;
        _connectedExternalCamNameValue = @"";
        _statusSummaryValue = @"IDLE";
    }
}

- (AVCaptureDevice *)discoverBestExternalUsbCamera {
    AVCaptureDeviceType extType = externalUsbCameraDeviceType();
    if (!extType) {
        return nil;
    }
    AVCaptureDeviceDiscoverySession *disc = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[extType]
                              mediaType:AVMediaTypeVideo
                               position:AVCaptureDevicePositionUnspecified];
    if (disc.devices.count == 0) {
        return nil;
    }

    NSString *hintLower = nil;
    {
        std::lock_guard<std::mutex> lock(_cbMutex);
        hintLower = _preferredExternalCamName.length > 0 ? [_preferredExternalCamName lowercaseString] : nil;
    }

    AVCaptureDevice *bestDev = nil;
    int bestScore = -10000;
    for (AVCaptureDevice *dev in disc.devices) {
        NSString *nameLower = [(dev.localizedName ?: @"") lowercaseString];
        int score = 0;
        if ([nameLower containsString:@"color"] || [nameLower containsString:@"rgb"]) score += 100;
        if (hintLower.length > 0 && [nameLower containsString:hintLower]) score += 60;
        if ([nameLower containsString:@"orbbec"] || [nameLower containsString:@"gemini"] || [nameLower containsString:@"336"]) score += 50;
        if ([nameLower containsString:@"depth"] || [nameLower containsString:@"infrared"] ||
            [nameLower containsString:@" ir"] || [nameLower containsString:@"ir "]) score -= 90;

        bool has1280x800 = false;
        bool hasColorFourCC = false;
        for (AVCaptureDeviceFormat *fmt in dev.formats) {
            CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
            FourCharCode fcc = CMFormatDescriptionGetMediaSubType(fmt.formatDescription);
            if (fcc == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                fcc == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
                fcc == kCVPixelFormatType_422YpCbCr8 ||
                fcc == kCVPixelFormatType_422YpCbCr8_yuvs ||
                fcc == kCVPixelFormatType_32BGRA ||
                fcc == 'dmb1' || fcc == 'jpeg') {
                hasColorFourCC = true;
            }
            if (dims.width == 1280 && dims.height == 800) {
                for (AVFrameRateRange *r in fmt.videoSupportedFrameRateRanges) {
                    if (r.maxFrameRate >= 25.0) {
                        has1280x800 = true;
                        break;
                    }
                }
            }
        }
        if (has1280x800) score += 40;
        if (hasColorFourCC) score += 15;
        else score -= 40;

        if (!bestDev || score > bestScore) {
            bestDev = dev;
            bestScore = score;
        }
    }
    return bestDev;
}

- (BOOL)startExternalUsbLockedOnSessionQueue:(uint64_t)generation {
    if (_sessionGeneration.load() != generation) {
        return NO;
    }

    if (!isIos17OrNewer()) {
        [self setStatusSummaryText:@"NEEDS iPadOS 17+"];
        return NO;
    }

    AVCaptureDevice *device = [self discoverBestExternalUsbCamera];
    if (!device) {
        _waitingForExternalCamera = YES;
        _waitingGeneration = generation;
        _activeMode = AVCameraAcquisitionModeExternalUsbUvc;
        _runningAtomic.store(false);
        {
            std::lock_guard<std::mutex> lock(_cbMutex);
            _connectedExternalCamNameValue = @"";
            _statusSummaryValue = @"NO USB 336L CAM";
        }
        return NO;
    }

    _waitingForExternalCamera = NO;
    _videoDevice = device;
    {
        std::lock_guard<std::mutex> lock(_cbMutex);
        _connectedExternalCamNameValue = [device.localizedName copy] ?: @"USB Camera";
    }

    AVCaptureSession *session = [[AVCaptureSession alloc] init];
    [session beginConfiguration];

    NSError *err = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&err];
    if (!input || ![session canAddInput:input]) {
        [session commitConfiguration];
        [self setStatusSummaryText:@"USB CAM INPUT ERR"];
        return NO;
    }
    [session addInput:input];

    // Prefer Orbbec Gemini 336L production RGB stream: 1280x800 @ 30fps (config/camera_profiles/production.yaml),
    // otherwise pick the closest color UVC format <= 1920x1200 supporting >= 24fps.
    AVCaptureDeviceFormat *bestFmt = nil;
    int bestFmtScore = -10000;
    for (AVCaptureDeviceFormat *fmt in device.formats) {
        CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
        if (dims.width < 640 || dims.width > 1920 || dims.height < 360 || dims.height > 1200) continue;
        double maxFps = 0.0;
        for (AVFrameRateRange *r in fmt.videoSupportedFrameRateRanges) {
            if (r.maxFrameRate > maxFps) maxFps = r.maxFrameRate;
        }
        if (maxFps < 15.0) continue;

        int score = 0;
        if (dims.width == 1280 && dims.height == 800) score += 200;
        else if (dims.width == 1280 && dims.height == 720) score += 120;
        else if (dims.width == 1920 && dims.height == 1080) score += 90;
        else if (dims.width == 848 && dims.height == 480) score += 60;
        if (maxFps >= 29.0) score += 50;
        if (!bestFmt || score > bestFmtScore) {
            bestFmt = fmt;
            bestFmtScore = score;
        }
    }

    if (bestFmt && [session canSetSessionPreset:AVCaptureSessionPresetInputPriority]) {
        session.sessionPreset = AVCaptureSessionPresetInputPriority;
        NSError *lockErr = nil;
        if ([device lockForConfiguration:&lockErr]) {
            device.activeFormat = bestFmt;
            for (AVFrameRateRange *r in bestFmt.videoSupportedFrameRateRanges) {
                if (r.maxFrameRate >= 29.5 && r.minFrameRate <= 30.5) {
                    device.activeVideoMinFrameDuration = CMTimeMake(1, 30);
                    device.activeVideoMaxFrameDuration = CMTimeMake(1, 30);
                    break;
                }
            }
            if ([device isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]) {
                device.exposureMode = AVCaptureExposureModeContinuousAutoExposure;
            }
            [device unlockForConfiguration];
        }
    } else if ([session canSetSessionPreset:AVCaptureSessionPreset1280x720]) {
        session.sessionPreset = AVCaptureSessionPreset1280x720;
    }

    AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
    };
    [output setSampleBufferDelegate:self queue:_captureQueue];

    if ([session canAddOutput:output]) {
        [session addOutput:output];
    } else {
        [session commitConfiguration];
        [self setStatusSummaryText:@"USB CAM OUT ERR"];
        return NO;
    }

    [session commitConfiguration];
    [session startRunning];

    _avSession = session;
    _activeMode = AVCameraAcquisitionModeExternalUsbUvc;
    _runningAtomic.store(session.isRunning ? true : false);
    [self setStatusSummaryText:(session.isRunning ? @"USB 336L LIVE" : @"USB CAM STARTING")];
    return session.isRunning ? YES : NO;
}

- (BOOL)startAvFoundationLockedOnSessionQueue:(uint64_t)generation {
    if (_sessionGeneration.load() != generation) {
        return NO;
    }

    AVCaptureDevice *device = [AVCaptureDevice defaultDeviceWithDeviceType:AVCaptureDeviceTypeBuiltInWideAngleCamera
                                                                 mediaType:AVMediaTypeVideo
                                                                  position:AVCaptureDevicePositionBack];
    if (!device) {
        device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    }
    if (!device) {
        [self setStatusSummaryText:@"NO CAMERA"];
        return NO;
    }
    _videoDevice = device;

    AVCaptureSession *session = [[AVCaptureSession alloc] init];
    [session beginConfiguration];

    NSError *err = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&err];
    if (!input || ![session canAddInput:input]) {
        [session commitConfiguration];
        [self setStatusSummaryText:@"CAM INPUT ERR"];
        return NO;
    }
    [session addInput:input];

    // Prefer crisp 4:3 wide-FOV 30fps format (1920x1440 or 1440x1080 -> 2x downsampled 960x720/720x540)
    // to maximize vertical RGB coverage on floors & ceilings; fall back cleanly to 1920x1080.
    AVCaptureDeviceFormat *best4x3Format = nil;
    for (AVCaptureDeviceFormat *fmt in device.formats) {
        CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
        const bool isTarget4x3 = ((dims.width == 1920 && dims.height == 1440) ||
                                  (dims.width == 1440 && dims.height == 1080));
        if (!isTarget4x3) continue;
        bool supports30 = false;
        for (AVFrameRateRange *r in fmt.videoSupportedFrameRateRanges) {
            if (r.maxFrameRate >= 29.5 && r.minFrameRate <= 30.5) {
                supports30 = true;
                break;
            }
        }
        if (!supports30) continue;
        if (!best4x3Format ||
            CMVideoFormatDescriptionGetDimensions(fmt.formatDescription).width >
            CMVideoFormatDescriptionGetDimensions(best4x3Format.formatDescription).width) {
            best4x3Format = fmt;
        }
    }

    BOOL applied4x3 = NO;
    if (best4x3Format && [session canSetSessionPreset:AVCaptureSessionPresetInputPriority]) {
        session.sessionPreset = AVCaptureSessionPresetInputPriority;
        NSError *lockErr = nil;
        if ([device lockForConfiguration:&lockErr]) {
            device.activeFormat = best4x3Format;
            device.activeVideoMinFrameDuration = CMTimeMake(1, 30);
            device.activeVideoMaxFrameDuration = CMTimeMake(1, 30);
            if ([device isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus]) {
                device.focusMode = AVCaptureFocusModeContinuousAutoFocus;
            }
            if ([device isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]) {
                device.exposureMode = AVCaptureExposureModeContinuousAutoExposure;
            }
            [device unlockForConfiguration];
            applied4x3 = YES;
        }
    }
    if (!applied4x3) {
        if ([session canSetSessionPreset:AVCaptureSessionPreset1920x1080]) {
            session.sessionPreset = AVCaptureSessionPreset1920x1080;
        } else if ([session canSetSessionPreset:AVCaptureSessionPreset1280x720]) {
            session.sessionPreset = AVCaptureSessionPreset1280x720;
        }
        NSError *lockErr = nil;
        if ([device lockForConfiguration:&lockErr]) {
            if ([device isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus]) {
                device.focusMode = AVCaptureFocusModeContinuousAutoFocus;
            }
            if ([device isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]) {
                device.exposureMode = AVCaptureExposureModeContinuousAutoExposure;
            }
            [device unlockForConfiguration];
        }
    }

    AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
    };
    [output setSampleBufferDelegate:self queue:_captureQueue];

    if ([session canAddOutput:output]) {
        [session addOutput:output];
    } else {
        [session commitConfiguration];
        [self setStatusSummaryText:@"CAM OUTPUT ERR"];
        return NO;
    }

    AVCaptureConnection *conn = [output connectionWithMediaType:AVMediaTypeVideo];
    if (conn && conn.isCameraIntrinsicMatrixDeliverySupported) {
        conn.cameraIntrinsicMatrixDeliveryEnabled = YES;
    }

    [session commitConfiguration];
    [session startRunning];

    _avSession = session;
    _activeMode = AVCameraAcquisitionModeAVFoundationPure;
    _runningAtomic.store(session.isRunning ? true : false);
    [self setStatusSummaryText:(session.isRunning ? @"AVFOUNDATION LIVE" : @"CAM STARTING")];
    return session.isRunning ? YES : NO;
}

- (void)startCaptureInternalMode:(AVCameraAcquisitionMode)mode generation:(uint64_t)generation {
    CameraCaptureManager * __weak weakSelf = self;
    dispatch_async(_sessionQueue, ^{
        CameraCaptureManager *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_sessionGeneration.load() != generation) return;

        [strongSelf stopCaptureLockedOnSessionQueue];
        if (strongSelf->_sessionGeneration.load() != generation) return;

        if (mode == AVCameraAcquisitionModeExternalUsbUvc) {
            [strongSelf startExternalUsbLockedOnSessionQueue:generation];
            return;
        }

        if (mode == AVCameraAcquisitionModeARKitSession) {
            if (![ARWorldTrackingConfiguration isSupported]) {
                [strongSelf startAvFoundationLockedOnSessionQueue:generation];
                return;
            }
            ARSession *ar = [[ARSession alloc] init];
            ar.delegate = strongSelf;
            ar.delegateQueue = strongSelf->_captureQueue;
            ARWorldTrackingConfiguration *config = [[ARWorldTrackingConfiguration alloc] init];
            config.worldAlignment = ARWorldAlignmentGravity;
            [ar runWithConfiguration:config options:ARSessionRunOptionResetTracking];

            strongSelf->_arSession = ar;
            strongSelf->_activeMode = AVCameraAcquisitionModeARKitSession;
            strongSelf->_runningAtomic.store(true);
            [strongSelf setStatusSummaryText:@"ARKIT LIVE"];
            return;
        }

        [strongSelf startAvFoundationLockedOnSessionQueue:generation];
    });
}

- (BOOL)startCaptureWithMode:(AVCameraAcquisitionMode)mode {
    const uint64_t gen = ++_sessionGeneration;
    _activeMode = mode;

    AVAuthorizationStatus auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (auth == AVAuthorizationStatusDenied || auth == AVAuthorizationStatusRestricted) {
        _authorizedAtomic.store(false);
        [self setStatusSummaryText:@"CAM DENIED"];
        return NO;
    }

    if (auth == AVAuthorizationStatusNotDetermined) {
        [self setStatusSummaryText:@"AUTH REQUEST..."];
        CameraCaptureManager * __weak weakSelf = self;
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
            CameraCaptureManager *strongSelf = weakSelf;
            if (!strongSelf) return;
            strongSelf->_authorizedAtomic.store(granted ? true : false);
            if (!granted) {
                [strongSelf setStatusSummaryText:@"CAM DENIED"];
                return;
            }
            if (strongSelf->_sessionGeneration.load() == gen) {
                [strongSelf startCaptureInternalMode:mode generation:gen];
            }
        }];
        return YES;
    }

    _authorizedAtomic.store(true);
    [self setStatusSummaryText:@"STARTING CAM..."];
    [self startCaptureInternalMode:mode generation:gen];
    return YES;
}

- (void)stopCapture {
    ++_sessionGeneration;
    _runningAtomic.store(false);
    dispatch_sync(_sessionQueue, ^{
        [self stopCaptureLockedOnSessionQueue];
    });
}

- (void)onAvSessionRuntimeError:(NSNotification *)note {
    AVCaptureSession *failed = note.object;
    if (!failed || failed != _avSession) return;
    const uint64_t gen = _sessionGeneration.load();
    CameraCaptureManager * __weak weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)), _sessionQueue, ^{
        CameraCaptureManager *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_sessionGeneration.load() != gen) return;
        if (strongSelf->_avSession && !strongSelf->_avSession.isRunning) {
            [strongSelf->_avSession startRunning];
            strongSelf->_runningAtomic.store(strongSelf->_avSession.isRunning ? true : false);
        }
    });
}

- (void)onAvSessionInterruptionEnded:(NSNotification *)note {
    AVCaptureSession *sess = note.object;
    if (!sess || sess != _avSession) return;
    const uint64_t gen = _sessionGeneration.load();
    CameraCaptureManager * __weak weakSelf = self;
    dispatch_async(_sessionQueue, ^{
        CameraCaptureManager *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_sessionGeneration.load() != gen) return;
        if (strongSelf->_avSession && !strongSelf->_avSession.isRunning) {
            [strongSelf->_avSession startRunning];
            strongSelf->_runningAtomic.store(strongSelf->_avSession.isRunning ? true : false);
        }
    });
}

- (void)onCaptureDeviceConnected:(NSNotification *)note {
    (void)note;
    if (_activeMode != AVCameraAcquisitionModeExternalUsbUvc) return;
    const uint64_t gen = _sessionGeneration.load();
    CameraCaptureManager * __weak weakSelf = self;
    dispatch_async(_sessionQueue, ^{
        CameraCaptureManager *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_sessionGeneration.load() != gen) return;
        if (strongSelf->_waitingForExternalCamera || !strongSelf->_runningAtomic.load()) {
            [strongSelf startExternalUsbLockedOnSessionQueue:gen];
        }
    });
}

- (void)onCaptureDeviceDisconnected:(NSNotification *)note {
    AVCaptureDevice *disconnected = note.object;
    if (_activeMode != AVCameraAcquisitionModeExternalUsbUvc) return;
    const uint64_t gen = _sessionGeneration.load();
    CameraCaptureManager * __weak weakSelf = self;
    dispatch_async(_sessionQueue, ^{
        CameraCaptureManager *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_sessionGeneration.load() != gen) return;
        if (!disconnected || disconnected == strongSelf->_videoDevice) {
            [strongSelf stopCaptureLockedOnSessionQueue];
            strongSelf->_waitingForExternalCamera = YES;
            strongSelf->_waitingGeneration = gen;
            [strongSelf setStatusSummaryText:@"USB 336L UNPLUGGED"];
        }
    });
}

- (avscan::core::SlamState)latestArkitDiagnosticState {
    std::lock_guard<std::mutex> lock(_cbMutex);
    return _latestArkitDiag;
}

- (void)processPixelBuffer:(CVPixelBufferRef)pixelBuffer
              timestampSec:(double)timestampSec
                intrinsics:(const avscan::core::CameraIntrinsics &)intrinsics
          exposureDuration:(double)exposureSec
                       iso:(float)isoVal {
    if (!pixelBuffer || !_runningAtomic.load()) return;

    // Normalize timestamp into CACurrentMediaTime() monotonic host domain so TimeSynchronizer
    // always aligns RGB frames cleanly with Airy LiDAR/IMU host reception timestamps.
    const double nowHost = CACurrentMediaTime();
    if (!std::isfinite(timestampSec) || std::abs(timestampSec - nowHost) > 2.0) {
        timestampSec = nowHost;
    }

    double minInterval = (_targetFrameRate > 0) ? (1.0 / static_cast<double>(_targetFrameRate)) : 0.033;
    if (_lastProcessedTimestamp > 0.0 && (timestampSec - _lastProcessedTimestamp) < minInterval * 0.85) {
        return;
    }
    _lastProcessedTimestamp = timestampSec;

    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    size_t width = CVPixelBufferGetWidth(pixelBuffer);
    size_t height = CVPixelBufferGetHeight(pixelBuffer);

    // Downsample 2x into crisp working RGB/SLAM resolution (~960x720 or 960x540) for sharp colorization
    const int dsFactor = (width >= 1280) ? 2 : 1;
    const double invDs = 1.0 / static_cast<double>(dsFactor);
    int dstW = static_cast<int>(width / dsFactor);
    int dstH = static_cast<int>(height / dsFactor);
    if (dstW < 64 || dstH < 64) {
        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        return;
    }

    avscan::core::RgbFrame frame;
    frame.frame_id = ++_frameCounter;
    frame.timestamp_sec = timestampSec;
    frame.exposure_duration_sec = exposureSec;
    frame.iso = isoVal;
    frame.intrinsics = intrinsics;
    frame.intrinsics.width = dstW;
    frame.intrinsics.height = dstH;
    frame.intrinsics.fx = intrinsics.fx * invDs;
    frame.intrinsics.fy = intrinsics.fy * invDs;
    frame.intrinsics.cx = intrinsics.cx * invDs;
    frame.intrinsics.cy = intrinsics.cy * invDs;

    frame.rgb_bytes.resize(static_cast<size_t>(dstW * dstH * 3));
    frame.gray_bytes.resize(static_cast<size_t>(dstW * dstH));

    OSType pixFmt = CVPixelBufferGetPixelFormatType(pixelBuffer);
    if (pixFmt == kCVPixelFormatType_32BGRA) {
        const uint8_t *base = reinterpret_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(pixelBuffer));
        size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
        for (int y = 0; y < dstH; ++y) {
            const uint8_t *row = base + static_cast<size_t>(y * dsFactor) * bytesPerRow;
            for (int x = 0; x < dstW; ++x) {
                const uint8_t *px = row + static_cast<size_t>(x * dsFactor) * 4;
                uint8_t b = px[0], g = px[1], r = px[2];
                size_t idx = static_cast<size_t>(y * dstW + x);
                frame.rgb_bytes[idx * 3 + 0] = r;
                frame.rgb_bytes[idx * 3 + 1] = g;
                frame.rgb_bytes[idx * 3 + 2] = b;
                frame.gray_bytes[idx] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
            }
        }
    } else if (CVPixelBufferGetPlaneCount(pixelBuffer) >= 2) {
        // YCbCr Bi-Planar 4:2:0 (ARKit capturedImage default) -> Full-color RGB24
        const uint8_t *yPlane = reinterpret_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
        size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
        const uint8_t *uvPlane = reinterpret_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
        size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
        for (int y = 0; y < dstH; ++y) {
            const int srcY = y * dsFactor;
            const uint8_t *yRow = yPlane + static_cast<size_t>(srcY) * yStride;
            const uint8_t *uvRow = uvPlane + static_cast<size_t>(srcY / 2) * uvStride;
            for (int x = 0; x < dstW; ++x) {
                const int srcX = x * dsFactor;
                const int lum = static_cast<int>(yRow[srcX]);
                const int uvIdx = (srcX / 2) * 2;
                const int cb = static_cast<int>(uvRow[uvIdx + 0]) - 128;
                const int cr = static_cast<int>(uvRow[uvIdx + 1]) - 128;
                const int r = std::clamp(static_cast<int>(lum + 1.402 * cr), 0, 255);
                const int g = std::clamp(static_cast<int>(lum - 0.344136 * cb - 0.714136 * cr), 0, 255);
                const int b = std::clamp(static_cast<int>(lum + 1.772 * cb), 0, 255);
                size_t idx = static_cast<size_t>(y * dstW + x);
                frame.gray_bytes[idx] = static_cast<uint8_t>(lum);
                frame.rgb_bytes[idx * 3 + 0] = static_cast<uint8_t>(r);
                frame.rgb_bytes[idx * 3 + 1] = static_cast<uint8_t>(g);
                frame.rgb_bytes[idx * 3 + 2] = static_cast<uint8_t>(b);
            }
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    std::function<void(const avscan::core::RgbFrame &)> cb;
    {
        std::lock_guard<std::mutex> lock(_cbMutex);
        _lastFrameHostTime = nowHost;
        if (_fpsWindowStart <= 0.0) {
            _fpsWindowStart = nowHost;
            _fpsWindowFrames = 1;
        } else {
            _fpsWindowFrames++;
            const double span = nowHost - _fpsWindowStart;
            if (span >= 0.5) {
                _measuredFpsValue = static_cast<double>(_fpsWindowFrames) / span;
                _fpsWindowStart = nowHost;
                _fpsWindowFrames = 0;
            }
        }
        if (_activeMode == AVCameraAcquisitionModeExternalUsbUvc) {
            _statusSummaryValue = @"USB 336L LIVE";
        } else if (_activeMode == AVCameraAcquisitionModeARKitSession) {
            _statusSummaryValue = @"ARKIT LIVE";
        } else {
            _statusSummaryValue = @"AVFOUNDATION LIVE";
        }
        cb = _onFrame;
    }
    if (cb) {
        cb(frame);
    }
}

#pragma mark - AVCaptureVideoDataOutputSampleBufferDelegate

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection {
    (void)output;
    (void)connection;

    CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixelBuffer) return;

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    double timestampSec = CMTimeGetSeconds(pts);

    size_t w = CVPixelBufferGetWidth(pixelBuffer);
    size_t h = CVPixelBufferGetHeight(pixelBuffer);
    avscan::core::CameraIntrinsics intr;
    intr.width = static_cast<int>(w);
    intr.height = static_cast<int>(h);

    bool usedCalibratedExternal = false;
    if (_activeMode == AVCameraAcquisitionModeExternalUsbUvc) {
        std::lock_guard<std::mutex> lock(_cbMutex);
        if (_hasExternalFallbackIntrinsics) {
            const double sx = static_cast<double>(w) / static_cast<double>(_externalFallbackIntrinsics.width);
            const double sy = static_cast<double>(h) / static_cast<double>(_externalFallbackIntrinsics.height);
            intr.fx = _externalFallbackIntrinsics.fx * sx;
            intr.fy = _externalFallbackIntrinsics.fy * sy;
            intr.cx = _externalFallbackIntrinsics.cx * sx;
            intr.cy = _externalFallbackIntrinsics.cy * sy;
            intr.dist_coeffs = _externalFallbackIntrinsics.dist_coeffs;
            usedCalibratedExternal = true;
        }
    }
    if (!usedCalibratedExternal) {
        intr.fx = static_cast<double>(w) * 0.742;
        intr.fy = intr.fx;
        intr.cx = static_cast<double>(w) * 0.5;
        intr.cy = static_cast<double>(h) * 0.5;
    }

    CFTypeRef matrixAttachment = CMGetAttachment(sampleBuffer, kCMSampleBufferAttachmentKey_CameraIntrinsicMatrix, NULL);
    if (!usedCalibratedExternal && matrixAttachment && CFGetTypeID(matrixAttachment) == CFDataGetTypeID()) {
        CFDataRef dataRef = (CFDataRef)matrixAttachment;
        if (CFDataGetLength(dataRef) == sizeof(matrix_float3x3)) {
            matrix_float3x3 K;
            CFDataGetBytes(dataRef, CFRangeMake(0, sizeof(K)), reinterpret_cast<UInt8 *>(&K));
            if (std::isfinite(K.columns[0].x) && K.columns[0].x > 100.0f) {
                intr.fx = K.columns[0].x;
                intr.fy = K.columns[1].y;
                intr.cx = K.columns[2].x;
                intr.cy = K.columns[2].y;
            }
        }
    }

    double expSec = 0.008;
    float isoVal = 100.0f;
    if (_videoDevice && _activeMode != AVCameraAcquisitionModeExternalUsbUvc) {
        expSec = CMTimeGetSeconds(_videoDevice.exposureDuration);
        isoVal = _videoDevice.ISO;
    }

    [self processPixelBuffer:pixelBuffer
                timestampSec:timestampSec
                  intrinsics:intr
            exposureDuration:expSec
                         iso:isoVal];
}

#pragma mark - ARSessionDelegate

- (void)session:(ARSession *)session didUpdateFrame:(ARFrame *)frame {
    (void)session;
    if (!frame || !_runningAtomic.load()) return;

    CVPixelBufferRef pix = frame.capturedImage;
    size_t w = CVPixelBufferGetWidth(pix);
    size_t h = CVPixelBufferGetHeight(pix);

    matrix_float3x3 K = frame.camera.intrinsics;
    avscan::core::CameraIntrinsics intr;
    intr.width = static_cast<int>(w);
    intr.height = static_cast<int>(h);
    intr.fx = K.columns[0].x;
    intr.fy = K.columns[1].y;
    intr.cx = K.columns[2].x;
    intr.cy = K.columns[2].y;

    const double nowHost = CACurrentMediaTime();
    const double ts = (std::isfinite(frame.timestamp) && std::abs(frame.timestamp - nowHost) <= 2.0)
        ? frame.timestamp
        : nowHost;

    // Extract ARKit pose strictly for diagnostic comparison / scratch Hand-Eye calibration
    simd_float4x4 T_ar = frame.camera.transform;
    avscan::math::Mat3d R;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            R(r, c) = T_ar.columns[c][r];
    avscan::math::Vec3d t(T_ar.columns[3].x, T_ar.columns[3].y, T_ar.columns[3].z);
    avscan::math::SE3d pose(R, t);

    std::function<void(double, const avscan::math::SE3d &)> diagCb;
    {
        std::lock_guard<std::mutex> lock(_cbMutex);
        _latestArkitDiag.timestamp_sec = ts;
        _latestArkitDiag.T_world_body = pose;
        diagCb = _onDiagPose;
    }
    if (diagCb) {
        diagCb(ts, pose);
    }

    [self processPixelBuffer:pix
                timestampSec:ts
                  intrinsics:intr
            exposureDuration:frame.camera.exposureDuration
                         iso:100.0f];
}

- (void)session:(ARSession *)session didFailWithError:(NSError *)error {
    (void)session;
    (void)error;
    // Never leave the camera dark if ARKit fails/contends: immediately fall back to pure AVFoundation
    const uint64_t gen = _sessionGeneration.load();
    CameraCaptureManager * __weak weakSelf = self;
    dispatch_async(_sessionQueue, ^{
        CameraCaptureManager *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_sessionGeneration.load() != gen) return;
        [strongSelf stopCaptureLockedOnSessionQueue];
        [strongSelf startAvFoundationLockedOnSessionQueue:gen];
    });
}

- (void)sessionWasInterrupted:(ARSession *)session {
    (void)session;
    [self setStatusSummaryText:@"CAM INTERRUPTED"];
}

- (void)sessionInterruptionEnded:(ARSession *)session {
    (void)session;
    const uint64_t gen = _sessionGeneration.load();
    CameraCaptureManager * __weak weakSelf = self;
    dispatch_async(_sessionQueue, ^{
        CameraCaptureManager *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_sessionGeneration.load() != gen) return;
        if (strongSelf->_arSession) {
            ARWorldTrackingConfiguration *config = [[ARWorldTrackingConfiguration alloc] init];
            config.worldAlignment = ARWorldAlignmentGravity;
            [strongSelf->_arSession runWithConfiguration:config options:0];
        }
    });
}

@end
