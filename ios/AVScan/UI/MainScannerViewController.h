#pragma once

#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import "../Bridge/AVScanCoreBridge.h"
#import "../Rendering/MetalPointCloudRenderer.h"
#import "FloorPlanView.h"
#import "AppleGlassTheme.h"
#import "AppleGlassSplashScreenView.h"

NS_ASSUME_NONNULL_BEGIN

@interface MainScannerViewController : UIViewController

- (void)showSplashScreen;

@end

NS_ASSUME_NONNULL_END
