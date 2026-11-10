#import "CameraCaptureManager.h"
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <atomic>
#include <mutex>

@implementation CameraCaptureManager {
    AVCaptureSession *_avSession;
    AVCaptureDevice *_videoDevice;
    dispatch_queue_t _captureQueue;
    ARSession *_arSession;

    std::mutex _cbMutex;
    std::function<void(const avscan::core::RgbFrame &)> _onFrame;
    std::function<void(double, const avscan::math::SE3d &)> _onDiagPose;

    std::atomic<uint64_t> _frameCounter;
    double _lastProcessedTimestamp;
    avscan::core::SlamState _latestArkitDiag;
}

- (instancetype)initWithMode:(AVCameraAcquisitionMode)mode {
    self = [super init];
    if (self) {
        _activeMode = mode;
        _isRunning = NO;
        _targetFrameRate = 30;
        _frameCounter = 0;
        _lastProcessedTimestamp = 0.0;
        _captureQueue = dispatch_queue_create("com.artificialvision.avscan.camera", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)setFrameCallback:(std::function<void(const avscan::core::RgbFrame &)>)frameCallback
   diagnosticPoseCallback:(std::function<void(double, const avscan::math::SE3d &)>)diagPoseCallback {
    std::lock_guard<std::mutex> lock(_cbMutex);
    _onFrame = std::move(frameCallback);
    _onDiagPose = std::move(diagPoseCallback);
}

- (BOOL)startCaptureWithMode:(AVCameraAcquisitionMode)mode {
    // Strictly stop any active session first so AVFoundation and ARKit never contend for the camera
    [self stopCapture];
    _activeMode = mode;

    if (mode == AVCameraAcquisitionModeARKitSession) {
        if (![ARWorldTrackingConfiguration isSupported]) {
            return [self startCaptureWithMode:AVCameraAcquisitionModeAVFoundationPure];
        }
        _arSession = [[ARSession alloc] init];
        _arSession.delegate = self;
        _arSession.delegateQueue = _captureQueue;

        ARWorldTrackingConfiguration *config = [[ARWorldTrackingConfiguration alloc] init];
        config.worldAlignment = ARWorldAlignmentGravity;
        [_arSession runWithConfiguration:config options:ARSessionRunOptionResetTracking];
        _isRunning = YES;
        return YES;
    }

    // Pure AVFoundation acquisition mode
    _avSession = [[AVCaptureSession alloc] init];
    [_avSession beginConfiguration];
    if ([_avSession canSetSessionPreset:AVCaptureSessionPreset1920x1080]) {
        _avSession.sessionPreset = AVCaptureSessionPreset1920x1080;
    }

    _videoDevice = [AVCaptureDevice defaultDeviceWithDeviceType:AVCaptureDeviceTypeBuiltInWideAngleCamera
                                                      mediaType:AVMediaTypeVideo
                                                       position:AVCaptureDevicePositionBack];
    if (!_videoDevice) {
        [_avSession commitConfiguration];
        return NO;
    }

    NSError *err = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:_videoDevice error:&err];
    if (!input || ![_avSession canAddInput:input]) {
        [_avSession commitConfiguration];
        return NO;
    }
    [_avSession addInput:input];

    AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
    };
    [output setSampleBufferDelegate:self queue:_captureQueue];

    if ([_avSession canAddOutput:output]) {
        [_avSession addOutput:output];
    }

    AVCaptureConnection *conn = [output connectionWithMediaType:AVMediaTypeVideo];
    if (conn && conn.isCameraIntrinsicMatrixDeliverySupported) {
        conn.cameraIntrinsicMatrixDeliveryEnabled = YES;
    }

    [_avSession commitConfiguration];
    [_avSession startRunning];
    _isRunning = YES;
    return YES;
}

- (void)stopCapture {
    if (_arSession) {
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
    _isRunning = NO;
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
    if (!pixelBuffer) return;

    double minInterval = (_targetFrameRate > 0) ? (1.0 / static_cast<double>(_targetFrameRate)) : 0.033;
    if ((timestampSec - _lastProcessedTimestamp) < minInterval * 0.85) {
        return;
    }
    _lastProcessedTimestamp = timestampSec;

    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    size_t width = CVPixelBufferGetWidth(pixelBuffer);
    size_t height = CVPixelBufferGetHeight(pixelBuffer);

    // Downsample 4x into working SLAM resolution (480x270 or 480x360) for real-time A14 direct visual tracking
    int dstW = static_cast<int>(width / 4);
    int dstH = static_cast<int>(height / 4);
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
    frame.intrinsics.fx = intrinsics.fx * 0.25;
    frame.intrinsics.fy = intrinsics.fy * 0.25;
    frame.intrinsics.cx = intrinsics.cx * 0.25;
    frame.intrinsics.cy = intrinsics.cy * 0.25;

    frame.rgb_bytes.resize(static_cast<size_t>(dstW * dstH * 3));
    frame.gray_bytes.resize(static_cast<size_t>(dstW * dstH));

    OSType pixFmt = CVPixelBufferGetPixelFormatType(pixelBuffer);
    if (pixFmt == kCVPixelFormatType_32BGRA) {
        const uint8_t *base = reinterpret_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(pixelBuffer));
        size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
        for (int y = 0; y < dstH; ++y) {
            const uint8_t *row = base + (y * 4) * bytesPerRow;
            for (int x = 0; x < dstW; ++x) {
                const uint8_t *px = row + (x * 4) * 4;
                uint8_t b = px[0], g = px[1], r = px[2];
                size_t idx = static_cast<size_t>(y * dstW + x);
                frame.rgb_bytes[idx * 3 + 0] = r;
                frame.rgb_bytes[idx * 3 + 1] = g;
                frame.rgb_bytes[idx * 3 + 2] = b;
                frame.gray_bytes[idx] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
            }
        }
    } else if (CVPixelBufferGetPlaneCount(pixelBuffer) >= 2) {
        // YCbCr Bi-Planar (ARKit capturedImage default)
        const uint8_t *yPlane = reinterpret_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
        size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
        for (int y = 0; y < dstH; ++y) {
            const uint8_t *row = yPlane + (y * 4) * yStride;
            for (int x = 0; x < dstW; ++x) {
                uint8_t lum = row[x * 4];
                size_t idx = static_cast<size_t>(y * dstW + x);
                frame.gray_bytes[idx] = lum;
                frame.rgb_bytes[idx * 3 + 0] = lum;
                frame.rgb_bytes[idx * 3 + 1] = lum;
                frame.rgb_bytes[idx * 3 + 2] = lum;
            }
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    std::function<void(const avscan::core::RgbFrame &)> cb;
    {
        std::lock_guard<std::mutex> lock(_cbMutex);
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
    intr.fx = static_cast<double>(w) * 0.78;
    intr.fy = intr.fx;
    intr.cx = static_cast<double>(w) * 0.5;
    intr.cy = static_cast<double>(h) * 0.5;

    CFTypeRef matrixAttachment = CMGetAttachment(sampleBuffer, kCMSampleBufferAttachmentKey_CameraIntrinsicMatrix, NULL);
    if (matrixAttachment && CFGetTypeID(matrixAttachment) == CFDataGetTypeID()) {
        CFDataRef dataRef = (CFDataRef)matrixAttachment;
        if (CFDataGetLength(dataRef) == sizeof(matrix_float3x3)) {
            matrix_float3x3 K;
            CFDataGetBytes(dataRef, CFRangeMake(0, sizeof(K)), reinterpret_cast<UInt8 *>(&K));
            intr.fx = K.columns[0].x;
            intr.fy = K.columns[1].y;
            intr.cx = K.columns[2].x;
            intr.cy = K.columns[2].y;
        }
    }

    double expSec = _videoDevice ? CMTimeGetSeconds(_videoDevice.exposureDuration) : 0.008;
    float isoVal = _videoDevice ? _videoDevice.ISO : 100.0f;

    [self processPixelBuffer:pixelBuffer
                timestampSec:timestampSec
                  intrinsics:intr
            exposureDuration:expSec
                         iso:isoVal];
}

#pragma mark - ARSessionDelegate

- (void)session:(ARSession *)session didUpdateFrame:(ARFrame *)frame {
    (void)session;
    if (!frame) return;

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

    // Extract ARKit pose strictly for diagnostic comparison (never fed as estimator state into FAST-LIVO2)
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
        _latestArkitDiag.timestamp_sec = frame.timestamp;
        _latestArkitDiag.T_world_body = pose;
        diagCb = _onDiagPose;
    }
    if (diagCb) {
        diagCb(frame.timestamp, pose);
    }

    [self processPixelBuffer:pix
                timestampSec:frame.timestamp
                  intrinsics:intr
            exposureDuration:frame.camera.exposureDuration
                         iso:100.0f];
}

@end
