#pragma once

#import <UIKit/UIKit.h>
#include "avscan/quality/QualityMonitor.hpp"

@interface FloorPlanView : UIView

- (void)updateWithFloorPlan:(const avscan::quality::FloorPlanGrid2D &)floorPlan;

@end
