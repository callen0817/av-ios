#pragma once

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <ARKit/ARKit.h>
#include "avscan/core/Types.hpp"
#include <functional>

typedef NS_ENUM(NSInteger, AVCameraAcquisitionMode) {
    AVCameraAcquisitionModeAVFoundationPure = 0, // Pure AVFoundation 30fps built-in RGB capture
    AVCameraAcquisitionModeARKitSession = 1,     // ARSession camera delivery + diagnostic pose comparison
    AVCameraAcquisitionModeExternalUsbUvc = 2    // External USB-C UVC camera only (e.g. ScanAR Orbbec Gemini 336L; never built-in iPad cam)
};

@interface CameraCaptureManager : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate, ARSessionDelegate>

@property (nonatomic, assign, readonly) AVCameraAcquisitionMode activeMode;
@property (nonatomic, assign, readonly) BOOL isRunning;
@property (nonatomic, assign, readonly) BOOL cameraAuthorized;
@property (nonatomic, assign, readonly) uint64_t framesDelivered;
@property (nonatomic, assign, readonly) double measuredFps;
@property (nonatomic, copy, readonly) NSString *statusSummary;
@property (nonatomic, copy, readonly) NSString *connectedExternalCameraName;
@property (nonatomic, assign) int targetFrameRate;

- (instancetype)initWithMode:(AVCameraAcquisitionMode)mode;
- (void)requestCameraPermissionIfNeeded;
- (void)setPreferredExternalCameraName:(NSString *)nameHint
                    fallbackIntrinsics:(const avscan::core::CameraIntrinsics &)intrinsics;
- (NSArray<NSString *> *)connectedExternalCameraNames;
- (void)setFrameCallback:(std::function<void(const avscan::core::RgbFrame &)>)frameCallback
   diagnosticPoseCallback:(std::function<void(double, const avscan::math::SE3d &)>)diagPoseCallback;

- (BOOL)startCaptureWithMode:(AVCameraAcquisitionMode)mode;
- (void)stopCapture;
- (avscan::core::SlamState)latestArkitDiagnosticState;

@end
