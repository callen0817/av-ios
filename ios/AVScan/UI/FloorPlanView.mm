#import "FloorPlanView.h"
#include <mutex>

@implementation FloorPlanView {
    avscan::quality::FloorPlanGrid2D _plan;
    std::mutex _mutex;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor colorWithRed:0.08 green:0.10 blue:0.14 alpha:0.92];
        self.layer.cornerRadius = 10.0;
        self.layer.borderWidth = 1.0;
        self.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.2].CGColor;
        self.clipsToBounds = YES;
    }
    return self;
}

- (void)updateWithFloorPlan:(const avscan::quality::FloorPlanGrid2D &)floorPlan {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _plan = floorPlan;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self setNeedsDisplay];
    });
}

- (void)drawRect:(CGRect)rect {
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    if (!ctx) return;

    std::lock_guard<std::mutex> lock(_mutex);
    if (_plan.width_cells <= 0 || _plan.height_cells <= 0 || _plan.occupancy_cells.empty()) {
        return;
    }

    CGFloat scaleX = rect.size.width / static_cast<CGFloat>(_plan.width_cells);
    CGFloat scaleY = rect.size.height / static_cast<CGFloat>(_plan.height_cells);
    CGFloat cellScale = MIN(scaleX, scaleY);

    for (int y = 0; y < _plan.height_cells; ++y) {
        for (int x = 0; x < _plan.width_cells; ++x) {
            uint8_t val = _plan.occupancy_cells[static_cast<size_t>(y * _plan.width_cells + x)];
            if (val == 0) continue;
            if (val == 1) {
                CGContextSetRGBFillColor(ctx, 0.16, 0.22, 0.30, 0.85);
            } else if (val == 2) {
                CGContextSetRGBFillColor(ctx, 0.90, 0.94, 1.00, 1.00);
            } else {
                CGContextSetRGBFillColor(ctx, 1.00, 0.65, 0.15, 0.95);
            }
            CGRect cellRect = CGRectMake(x * cellScale, ( _plan.height_cells - 1 - y ) * cellScale,
                                         MAX(1.5, cellScale), MAX(1.5, cellScale));
            CGContextFillRect(ctx, cellRect);
        }
    }

    // Draw trajectory polyline
    if (_plan.trajectory_xy.size() >= 2) {
        CGContextSetRGBStrokeColor(ctx, 0.0, 0.88, 1.0, 0.95);
        CGContextSetLineWidth(ctx, 2.0);
        CGContextBeginPath(ctx);
        for (size_t i = 0; i < _plan.trajectory_xy.size(); ++i) {
            CGFloat px = (_plan.trajectory_xy[i].x - _plan.origin_x_m) / _plan.resolution_m * cellScale;
            CGFloat py = (_plan.height_cells - (_plan.trajectory_xy[i].y - _plan.origin_y_m) / _plan.resolution_m) * cellScale;
            if (i == 0) CGContextMoveToPoint(ctx, px, py);
            else CGContextAddLineToPoint(ctx, px, py);
        }
        CGContextStrokePath(ctx);
    }

    // Draw loop closure constraints in green
    if (!_plan.loop_closure_links.empty()) {
        CGContextSetRGBStrokeColor(ctx, 0.15, 0.95, 0.45, 0.95);
        CGContextSetLineWidth(ctx, 2.2);
        for (const auto &link : _plan.loop_closure_links) {
            CGFloat x1 = (link.first.x - _plan.origin_x_m) / _plan.resolution_m * cellScale;
            CGFloat y1 = (_plan.height_cells - (link.first.y - _plan.origin_y_m) / _plan.resolution_m) * cellScale;
            CGFloat x2 = (link.second.x - _plan.origin_x_m) / _plan.resolution_m * cellScale;
            CGFloat y2 = (_plan.height_cells - (link.second.y - _plan.origin_y_m) / _plan.resolution_m) * cellScale;
            CGContextMoveToPoint(ctx, x1, y1);
            CGContextAddLineToPoint(ctx, x2, y2);
            CGContextStrokePath(ctx);
        }
    }
}

@end
