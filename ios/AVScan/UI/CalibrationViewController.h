#pragma once

#import <UIKit/UIKit.h>
#import "../Bridge/AVScanCoreBridge.h"

@interface CalibrationViewController : UIViewController

- (instancetype)initWithBridge:(AVScanCoreBridge *)bridge;

@end
