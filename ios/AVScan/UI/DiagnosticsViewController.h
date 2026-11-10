#pragma once

#import <UIKit/UIKit.h>
#import "../Bridge/AVScanCoreBridge.h"

@interface DiagnosticsViewController : UIViewController

- (instancetype)initWithBridge:(AVScanCoreBridge *)bridge;

@end
