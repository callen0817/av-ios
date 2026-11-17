#import "AppleGlassSplashScreenView.h"
#import "AppleGlassTheme.h"

@interface SplashCanvasView : UIView
@end

@implementation SplashCanvasView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor clearColor];
        self.userInteractionEnabled = NO;
    }
    return self;
}

- (void)drawRect:(CGRect)rect {
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    if (!ctx) return;

    CGFloat cx = rect.size.width * 0.5;
    CGFloat cy = 112.0;

    // 1. Concentric LiDAR Horizon Rings (radii 66px, 84px, 102px)
    CGFloat radii[] = {66.0, 84.0, 102.0};
    CGFloat alphas[] = {0.28, 0.18, 0.10};
    for (size_t i = 0; i < 3; ++i) {
        CGContextSetRGBStrokeColor(ctx, 186.0/255.0, 161.0/255.0, 100.0/255.0, alphas[i]);
        CGContextSetLineWidth(ctx, 1.0);
        CGContextAddArc(ctx, cx, cy, radii[i], 0, M_PI * 2, 0);
        CGContextStrokePath(ctx);
    }

    // 2. Soft radial champagne gold optical halo behind the logo
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGFloat locations[3] = {0.0, 0.65, 1.0};
    CGFloat components[12] = {
        186.0/255.0, 161.0/255.0, 100.0/255.0, 0.28, // center gold
        255.0/255.0, 255.0/255.0, 255.0/255.0, 0.08, // mid white
        186.0/255.0, 161.0/255.0, 100.0/255.0, 0.00  // edge transparent
    };
    CGGradientRef gradient = CGGradientCreateWithColorComponents(colorSpace, components, locations, 3);
    CGContextDrawRadialGradient(ctx, gradient, CGPointMake(cx, cy), 4.0, CGPointMake(cx, cy), 68.0, 0);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(colorSpace);
}

@end

@implementation AppleGlassSplashScreenView {
    UIView *_scrimView;
    UIView *_cardContainer;
    UIVisualEffectView *_blurEffectView;
    SplashCanvasView *_canvasView;
    UIImageView *_logoImageView;
    UILabel *_brandPillLabel;
    UILabel *_titleLabel;
    UILabel *_subtitleLabel;
    UIView *_progressBarTrack;
    UIView *_progressBarFill;
    UILabel *_statusLabel;
    UIButton *_closeButton;
    NSTimer *_simTimer;
    CGFloat _currentProgress;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        self.backgroundColor = [UIColor clearColor];

        // 1. Semi-transparent backdrop scrim (Optical see-through compatible)
        _scrimView = [[UIView alloc] initWithFrame:self.bounds];
        _scrimView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        _scrimView.backgroundColor = [UIColor colorWithRed:10.0/255.0 green:10.0/255.0 blue:14.0/255.0 alpha:0.55];
        [self addSubview:_scrimView];

        UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(onScrimTapped:)];
        [_scrimView addGestureRecognizer:tap];

        // 2. Translucent Apple Glass Card (560x380 responsive)
        CGFloat cardW = MIN(560.0, self.bounds.size.width - 32.0);
        CGFloat cardH = MIN(380.0, self.bounds.size.height - 48.0);
        _cardContainer = [[UIView alloc] initWithFrame:CGRectMake((self.bounds.size.width - cardW) * 0.5,
                                                                 (self.bounds.size.height - cardH) * 0.5,
                                                                 cardW, cardH)];
        _cardContainer.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin |
                                          UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
        _cardContainer.layer.cornerRadius = 20.0;
        _cardContainer.clipsToBounds = YES;
        [self addSubview:_cardContainer];

        // Blur effect view
        _blurEffectView = [AppleGlassTheme createGlassCardWithCornerRadius:20.0];
        _blurEffectView.frame = _cardContainer.bounds;
        _blurEffectView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [_cardContainer addSubview:_blurEffectView];

        // Extra dark glass tint
        UIView *glassTint = [[UIView alloc] initWithFrame:_cardContainer.bounds];
        glassTint.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        glassTint.backgroundColor = [AppleGlassTheme glassDarkWithOpacity:15.0];
        glassTint.userInteractionEnabled = NO;
        [_cardContainer addSubview:glassTint];

        // Concentric LiDAR horizon rings & radial halo
        _canvasView = [[SplashCanvasView alloc] initWithFrame:_cardContainer.bounds];
        _canvasView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [_cardContainer addSubview:_canvasView];

        // Top Centered Champagne Gold Specular Pill (104x3px)
        UIView *topPill = [[UIView alloc] initWithFrame:CGRectMake((cardW - 104.0) * 0.5, 3.0, 104.0, 3.0)];
        topPill.backgroundColor = [AppleGlassTheme goldPrimary];
        topPill.layer.cornerRadius = 1.5;
        topPill.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
        [_cardContainer addSubview:topPill];

        // Top-Right Close Button (✕)
        _closeButton = [UIButton buttonWithType:UIButtonTypeCustom];
        _closeButton.frame = CGRectMake(cardW - 38.0, 12.0, 26.0, 26.0);
        _closeButton.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleBottomMargin;
        [_closeButton setTitle:@"✕" forState:UIControlStateNormal];
        [_closeButton setTitleColor:[AppleGlassTheme whiteWarm] forState:UIControlStateNormal];
        _closeButton.titleLabel.font = [UIFont systemFontOfSize:11.0 weight:UIFontWeightBold];
        _closeButton.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.10];
        _closeButton.layer.cornerRadius = 13.0;
        _closeButton.layer.borderWidth = 1.0;
        _closeButton.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.25].CGColor;
        [_closeButton addTarget:self action:@selector(onCloseTapped) forControlEvents:UIControlEventTouchUpInside];
        [_cardContainer addSubview:_closeButton];

        // Brand Logo Image (118x118 centered around y=112)
        _logoImageView = [[UIImageView alloc] initWithFrame:CGRectMake((cardW - 118.0) * 0.5, 112.0 - 59.0, 118.0, 118.0)];
        _logoImageView.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
        _logoImageView.contentMode = UIViewContentModeScaleAspectFit;
        _logoImageView.image = [AppleGlassTheme loadBrandLogo];
        [_cardContainer addSubview:_logoImageView];

        // Brand Pill: "VERIFY  ·  DIRECT  ·  CLARIFY"
        _brandPillLabel = [[UILabel alloc] initWithFrame:CGRectMake(16.0, 196.0, cardW - 32.0, 18.0)];
        _brandPillLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
        _brandPillLabel.textAlignment = NSTextAlignmentCenter;
        _brandPillLabel.textColor = [AppleGlassTheme goldBright];
        _brandPillLabel.font = [UIFont monospacedSystemFontOfSize:10.0 weight:UIFontWeightBold];

        // Apply letter spacing
        NSMutableAttributedString *brandAttr = [[NSMutableAttributedString alloc] initWithString:@"VERIFY  ·  DIRECT  ·  CLARIFY"];
        [brandAttr addAttribute:NSKernAttributeName value:@(3.2) range:NSMakeRange(0, brandAttr.length)];
        _brandPillLabel.attributedText = brandAttr;
        [_cardContainer addSubview:_brandPillLabel];

        // Title: "AV Scan"
        _titleLabel = [[UILabel alloc] initWithFrame:CGRectMake(16.0, 218.0, cardW - 32.0, 32.0)];
        _titleLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
        _titleLabel.textAlignment = NSTextAlignmentCenter;
        _titleLabel.textColor = [AppleGlassTheme whiteWarm];
        _titleLabel.font = [UIFont systemFontOfSize:26.0 weight:UIFontWeightSemibold];
        _titleLabel.text = @"AV Scan";
        [_cardContainer addSubview:_titleLabel];

        // Subtitle: "an Artificial Vision product"
        _subtitleLabel = [[UILabel alloc] initWithFrame:CGRectMake(16.0, 252.0, cardW - 32.0, 18.0)];
        _subtitleLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
        _subtitleLabel.textAlignment = NSTextAlignmentCenter;
        _subtitleLabel.textColor = [AppleGlassTheme whiteMuted];
        _subtitleLabel.font = [UIFont systemFontOfSize:12.0 weight:UIFontWeightRegular];
        _subtitleLabel.text = @"an Artificial Vision product";
        [_cardContainer addSubview:_subtitleLabel];

        // Progress Bar (340px wide x 4px tall)
        CGFloat barW = MIN(340.0, cardW - 48.0);
        _progressBarTrack = [[UIView alloc] initWithFrame:CGRectMake((cardW - barW) * 0.5, 290.0, barW, 4.0)];
        _progressBarTrack.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
        _progressBarTrack.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.18];
        _progressBarTrack.layer.cornerRadius = 2.0;
        _progressBarTrack.clipsToBounds = YES;
        [_cardContainer addSubview:_progressBarTrack];

        _progressBarFill = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 0, 4.0)];
        _progressBarFill.backgroundColor = [AppleGlassTheme goldPrimary];
        _progressBarFill.layer.cornerRadius = 2.0;
        [_progressBarTrack addSubview:_progressBarFill];

        // Status Caption Label
        _statusLabel = [[UILabel alloc] initWithFrame:CGRectMake(16.0, 308.0, cardW - 32.0, 20.0)];
        _statusLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
        _statusLabel.textAlignment = NSTextAlignmentCenter;
        _statusLabel.textColor = [AppleGlassTheme goldBright];
        _statusLabel.font = [UIFont systemFontOfSize:11.0 weight:UIFontWeightRegular];
        _statusLabel.text = @"Initializing Spatial Compute Engine & Apple Glass AR HUD...";
        [_cardContainer addSubview:_statusLabel];

        // Double border on card
        [AppleGlassTheme applyGlassDoubleRimToView:_cardContainer cornerRadius:20.0];

        // Start progressive startup animation
        _currentProgress = 0.05;
        [self startStartupSequence];
    }
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [AppleGlassTheme applyGlassDoubleRimToView:_cardContainer cornerRadius:20.0];
}

- (void)startStartupSequence {
    __weak AppleGlassSplashScreenView *weakSelf = self;
    _simTimer = [NSTimer scheduledTimerWithTimeInterval:0.04 repeats:YES block:^(NSTimer * _Nonnull timer) {
        AppleGlassSplashScreenView *strongSelf = weakSelf;
        if (!strongSelf) {
            [timer invalidate];
            return;
        }

        strongSelf->_currentProgress += 0.022;
        if (strongSelf->_currentProgress >= 1.0) {
            strongSelf->_currentProgress = 1.0;
            [timer invalidate];
            strongSelf->_simTimer = nil;

            [strongSelf setProgress:1.0 statusText:@"Ready — Launching Clear AR Passthrough Workspace..."];

            // Auto-fade after 0.5s pause
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.55 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                [strongSelf dismissAnimated];
            });
            return;
        }

        NSString *status = @"Initializing Spatial Compute Engine & Apple Glass AR HUD...";
        if (strongSelf->_currentProgress > 0.45 && strongSelf->_currentProgress < 0.85) {
            status = @"Loading Multi-Sensor Profiles & C-SLAM Mesh...";
        } else if (strongSelf->_currentProgress >= 0.85) {
            status = @"Ready — Launching Clear AR Passthrough Workspace...";
        }

        [strongSelf setProgress:strongSelf->_currentProgress statusText:status];
    }];
}

- (void)setProgress:(CGFloat)progress statusText:(NSString *)statusText {
    _currentProgress = MAX(0.0, MIN(1.0, progress));
    CGFloat trackWidth = _progressBarTrack.bounds.size.width;
    _progressBarFill.frame = CGRectMake(0, 0, trackWidth * _currentProgress, 4.0);
    _statusLabel.text = statusText;
}

- (void)onScrimTapped:(UITapGestureRecognizer *)gesture {
    [self dismissAnimated];
}

- (void)onCloseTapped {
    [self dismissAnimated];
}

- (void)dismissAnimated {
    if (_simTimer) {
        [_simTimer invalidate];
        _simTimer = nil;
    }

    [UIView animateWithDuration:0.38 animations:^{
        self.alpha = 0.0;
        self->_cardContainer.transform = CGAffineTransformMakeScale(0.94, 0.94);
    } completion:^(BOOL finished) {
        [self removeFromSuperview];
        if (self.onDismiss) {
            self.onDismiss();
        }
    }];
}

@end
