#import "FloorPlanView.h"
#import "AppleGlassTheme.h"
#include <mutex>

@implementation FloorPlanView {
    avscan::quality::FloorPlanGrid2D _plan;
    std::mutex _mutex;
    UILabel *_headerLabel;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor colorWithRed:14.0/255.0 green:14.0/255.0 blue:18.0/255.0 alpha:0.75];
        self.layer.cornerRadius = 14.0;
        self.clipsToBounds = YES;
        [AppleGlassTheme applyGlassDoubleRimToView:self cornerRadius:14.0];

        _headerLabel = [[UILabel alloc] initWithFrame:CGRectMake(8, 6, frame.size.width - 16, 16)];
        _headerLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
        _headerLabel.text = @"2D FLOOR PLAN RADAR";
        _headerLabel.textColor = [AppleGlassTheme goldBright];
        _headerLabel.font = [UIFont monospacedSystemFontOfSize:9.5 weight:UIFontWeightBold];
        _headerLabel.textAlignment = NSTextAlignmentLeft;
        [self addSubview:_headerLabel];
    }
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [AppleGlassTheme applyGlassDoubleRimToView:self cornerRadius:14.0];
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

    CGFloat topPadding = 24.0;
    CGFloat usableHeight = rect.size.height - topPadding - 6.0;
    CGFloat usableWidth = rect.size.width - 12.0;

    CGFloat scaleX = usableWidth / static_cast<CGFloat>(_plan.width_cells);
    CGFloat scaleY = usableHeight / static_cast<CGFloat>(_plan.height_cells);
    CGFloat cellScale = MIN(scaleX, scaleY);

    CGFloat offsetX = 6.0 + (usableWidth - (_plan.width_cells * cellScale)) * 0.5;
    CGFloat offsetY = topPadding + (usableHeight - (_plan.height_cells * cellScale)) * 0.5;

    for (int y = 0; y < _plan.height_cells; ++y) {
        for (int x = 0; x < _plan.width_cells; ++x) {
            uint8_t val = _plan.occupancy_cells[static_cast<size_t>(y * _plan.width_cells + x)];
            if (val == 0) continue;
            if (val == 1) {
                // Free observed space: subtle translucent tint
                CGContextSetRGBFillColor(ctx, 255.0/255.0, 255.0/255.0, 255.0/255.0, 0.08);
            } else if (val == 2) {
                // Occupied wall: Champagne gold highlight
                CGContextSetRGBFillColor(ctx, 229.0/255.0, 201.0/255.0, 131.0/255.0, 0.95);
            } else {
                // Dynamic obstacle: Amber warning
                CGContextSetRGBFillColor(ctx, 245.0/255.0, 158.0/255.0, 11.0/255.0, 0.95);
            }
            CGRect cellRect = CGRectMake(offsetX + x * cellScale,
                                         offsetY + (_plan.height_cells - 1 - y) * cellScale,
                                         MAX(1.5, cellScale), MAX(1.5, cellScale));
            CGContextFillRect(ctx, cellRect);
        }
    }

    // Draw trajectory polyline (Cyan #38BDF8)
    if (_plan.trajectory_xy.size() >= 2) {
        CGContextSetRGBStrokeColor(ctx, 56.0/255.0, 189.0/255.0, 248.0/255.0, 0.95);
        CGContextSetLineWidth(ctx, 1.8);
        CGContextBeginPath(ctx);
        for (size_t i = 0; i < _plan.trajectory_xy.size(); ++i) {
            CGFloat px = offsetX + (_plan.trajectory_xy[i].x - _plan.origin_x_m) / _plan.resolution_m * cellScale;
            CGFloat py = offsetY + (_plan.height_cells - (_plan.trajectory_xy[i].y - _plan.origin_y_m) / _plan.resolution_m) * cellScale;
            if (i == 0) CGContextMoveToPoint(ctx, px, py);
            else CGContextAddLineToPoint(ctx, px, py);
        }
        CGContextStrokePath(ctx);
    }

    // Draw loop closure constraints (Emerald #4ADE80)
    if (!_plan.loop_closure_links.empty()) {
        CGContextSetRGBStrokeColor(ctx, 74.0/255.0, 222.0/255.0, 128.0/255.0, 0.95);
        CGContextSetLineWidth(ctx, 2.0);
        for (const auto &link : _plan.loop_closure_links) {
            CGFloat x1 = offsetX + (link.first.x - _plan.origin_x_m) / _plan.resolution_m * cellScale;
            CGFloat y1 = offsetY + (_plan.height_cells - (link.first.y - _plan.origin_y_m) / _plan.resolution_m) * cellScale;
            CGFloat x2 = offsetX + (link.second.x - _plan.origin_x_m) / _plan.resolution_m * cellScale;
            CGFloat y2 = offsetY + (_plan.height_cells - (link.second.y - _plan.origin_y_m) / _plan.resolution_m) * cellScale;
            CGContextMoveToPoint(ctx, x1, y1);
            CGContextAddLineToPoint(ctx, x2, y2);
            CGContextStrokePath(ctx);
        }
    }
}

@end
