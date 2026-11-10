#pragma once

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <ARKit/ARKit.h>
#include "avscan/core/Types.hpp"
#include <functional>

typedef NS_ENUM(NSInteger, AVCameraAcquisitionMode) {
    AVCameraAcquisitionModeAVFoundationPure = 0, // Pure AVFoundation 30fps RGB capture (default production mode)
    AVCameraAcquisitionModeARKitSession = 1      // ARSession camera delivery + diagnostic pose comparison (Gate 3)
};

@interface CameraCaptureManager : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate, ARSessionDelegate>

@property (nonatomic, assign, readonly) AVCameraAcquisitionMode activeMode;
@property (nonatomic, assign, readonly) BOOL isRunning;
@property (nonatomic, assign) int targetFrameRate;

- (instancetype)initWithMode:(AVCameraAcquisitionMode)mode;
- (void)setFrameCallback:(std::function<void(const avscan::core::RgbFrame &)>)frameCallback
   diagnosticPoseCallback:(std::function<void(double, const avscan::math::SE3d &)>)diagPoseCallback;

- (BOOL)startCaptureWithMode:(AVCameraAcquisitionMode)mode;
- (void)stopCapture;
- (avscan::core::SlamState)latestArkitDiagnosticState;

@end
