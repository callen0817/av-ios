#import "AppleGlassTheme.h"

@implementation AppleGlassTheme

+ (UIColor *)goldPrimary {
    return [UIColor colorWithRed:186.0/255.0 green:161.0/255.0 blue:100.0/255.0 alpha:1.0]; // #BAA164
}

+ (UIColor *)goldBright {
    return [UIColor colorWithRed:229.0/255.0 green:201.0/255.0 blue:131.0/255.0 alpha:1.0]; // #E5C983
}

+ (UIColor *)goldMuted {
    return [UIColor colorWithRed:216.0/255.0 green:191.0/255.0 blue:124.0/255.0 alpha:1.0]; // #D8BF7C
}

+ (UIColor *)whitePure {
    return [UIColor colorWithWhite:1.0 alpha:1.0];
}

+ (UIColor *)whiteWarm {
    return [UIColor colorWithRed:252.0/255.0 green:251.0/255.0 blue:248.0/255.0 alpha:1.0]; // #FCFBF8
}

+ (UIColor *)whiteMuted {
    return [UIColor colorWithRed:208.0/255.0 green:204.0/255.0 blue:194.0/255.0 alpha:1.0]; // #D0CCC2
}

+ (UIColor *)statusOk {
    return [UIColor colorWithRed:74.0/255.0 green:222.0/255.0 blue:128.0/255.0 alpha:1.0]; // #4ADE80
}

+ (UIColor *)statusWarn {
    return [UIColor colorWithRed:245.0/255.0 green:158.0/255.0 blue:11.0/255.0 alpha:1.0]; // #F59E0B
}

+ (UIColor *)statusErr {
    return [UIColor colorWithRed:248.0/255.0 green:113.0/255.0 blue:113.0/255.0 alpha:1.0]; // #F87171
}

+ (UIColor *)statusCyan {
    return [UIColor colorWithRed:56.0/255.0 green:189.0/255.0 blue:248.0/255.0 alpha:1.0]; // #38BDF8
}

+ (UIColor *)glassDarkBase {
    return [UIColor colorWithRed:18.0/255.0 green:18.0/255.0 blue:24.0/255.0 alpha:0.70];
}

+ (UIColor *)glassDarkWithOpacity:(CGFloat)pct {
    CGFloat clamped = MAX(0.0, MIN(100.0, pct)) / 100.0;
    CGFloat alpha = 0.05 + clamped * 0.65;
    return [UIColor colorWithRed:18.0/255.0 green:18.0/255.0 blue:24.0/255.0 alpha:alpha];
}

+ (UIColor *)glassBorderGold {
    return [UIColor colorWithRed:186.0/255.0 green:161.0/255.0 blue:100.0/255.0 alpha:0.65];
}

+ (UIColor *)glassBorderWhite {
    return [UIColor colorWithWhite:1.0 alpha:0.25];
}

+ (UIColor *)glassFill {
    return [UIColor colorWithWhite:1.0 alpha:0.08];
}

+ (UIVisualEffectView *)createGlassCardWithCornerRadius:(CGFloat)radius {
    UIBlurEffect *blur = nil;
    if (@available(iOS 13.0, *)) {
        blur = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark];
    } else {
        blur = [UIBlurEffect effectWithStyle:UIBlurEffectStyleDark];
    }
    UIVisualEffectView *glassView = [[UIVisualEffectView alloc] initWithEffect:blur];
    glassView.layer.cornerRadius = radius;
    glassView.clipsToBounds = YES;
    [self applyGlassDoubleRimToView:glassView cornerRadius:radius];
    return glassView;
}

+ (void)applyGlassDoubleRimToView:(UIView *)view cornerRadius:(CGFloat)radius {
    view.layer.cornerRadius = radius;
    view.layer.borderWidth = 1.5;
    view.layer.borderColor = [self glassBorderGold].CGColor;

    // Remove any previous inner rim
    for (CALayer *sub in [view.layer.sublayers copy]) {
        if ([sub.name isEqualToString:@"AppleGlassInnerRim"]) {
            [sub removeFromSuperlayer];
        }
    }

    // Inner White Specular Refraction Rim (inset 1.5px)
    CALayer *innerRim = [CALayer layer];
    innerRim.name = @"AppleGlassInnerRim";
    innerRim.frame = CGRectInset(view.bounds, 1.5, 1.5);
    innerRim.cornerRadius = (radius > 1.5) ? (radius - 1.5) : 0;
    innerRim.borderWidth = 1.0;
    innerRim.borderColor = [self glassBorderWhite].CGColor;
    innerRim.backgroundColor = [UIColor clearColor].CGColor;
    [view.layer addSublayer:innerRim];
}

+ (UIButton *)createGlassButtonWithTitle:(NSString *)title
                               baseColor:(UIColor *)baseColor
                            cornerRadius:(CGFloat)radius {
    UIButton *btn = [UIButton buttonWithType:UIButtonTypeCustom];
    [btn setTitle:title forState:UIControlStateNormal];
    [btn setTitleColor:[self whiteWarm] forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont systemFontOfSize:13.0 weight:UIFontWeightBold];

    CGFloat r = 0, g = 0, b = 0, a = 0;
    [baseColor getRed:&r green:&g blue:&b alpha:&a];
    UIColor *translucentFill = [UIColor colorWithRed:r green:g blue:b alpha:0.35];

    btn.backgroundColor = translucentFill;
    btn.layer.cornerRadius = radius;
    btn.layer.borderWidth = 1.5;
    btn.layer.borderColor = [UIColor colorWithRed:r green:g blue:b alpha:0.75].CGColor;
    btn.clipsToBounds = YES;

    return btn;
}

+ (UIImage * _Nullable)loadBrandLogo {
    // 1. Try Asset catalog / main bundle
    UIImage *img = [UIImage imageNamed:@"av_logo"];
    if (img) return img;

    // 2. Try file path in bundle
    NSString *bundlePath = [[NSBundle mainBundle] pathForResource:@"av_logo" ofType:@"png"];
    if (bundlePath) {
        img = [UIImage imageWithContentsOfFile:bundlePath];
        if (img) return img;
    }

    // 3. Fallback to documents / relative assets
    NSString *docPath = [[NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject] stringByAppendingPathComponent:@"av_logo.png"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:docPath]) {
        return [UIImage imageWithContentsOfFile:docPath];
    }

    return nil;
}

@end
