#pragma once

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#include "avscan/core/Types.hpp"
#include <vector>

@interface MetalPointCloudRenderer : NSObject <MTKViewDelegate>

@property (nonatomic, assign) int colorMode; // 0 = RGB, 1 = Intensity, 2 = Elevation Z
@property (nonatomic, assign) float pointSize;
@property (nonatomic, assign) float cameraDistance;
@property (nonatomic, assign) float cameraYawRad;
@property (nonatomic, assign) float cameraPitchRad;

- (instancetype)initWithMetalKitView:(MTKView *)mtkView;
- (void)updatePointCloud:(const std::vector<avscan::core::LidarPointXYZIRT> &)points
              trajectory:(const std::vector<avscan::core::SlamState> &)trajectory;
- (void)panByDeltaX:(float)dx deltaY:(float)dy;
- (void)orbitByDeltaYaw:(float)dyaw deltaPitch:(float)dpitch;
- (void)zoomByScale:(float)scale;
- (void)resetCamera;

@end
