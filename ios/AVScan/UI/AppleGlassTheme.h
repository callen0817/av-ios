#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * Artificial Vision — Apple Glass (visionOS) White & Gold AR Design System
 * Matched to https://github.com/callen0817/av-scan (AppWindow.qml)
 * and https://github.com/callen0817/av-scanar (scanhub_app.py)
 */
@interface AppleGlassTheme : NSObject

// Brand Color Palette
+ (UIColor *)goldPrimary;      // #BAA164 (Artificial Vision Primary Gold)
+ (UIColor *)goldBright;       // #E5C983 (Specular luminous gold highlight)
+ (UIColor *)goldMuted;        // #D8BF7C (Warm champagne gold)
+ (UIColor *)whitePure;        // #FFFFFF (Pure specular white)
+ (UIColor *)whiteWarm;        // #FCFBF8 (Warm alabaster white)
+ (UIColor *)whiteMuted;       // #D0CCC2 (Translucent ivory text)

// Status Colors
+ (UIColor *)statusOk;         // #4ADE80 / #22C55E (Emerald green)
+ (UIColor *)statusWarn;       // #F59E0B (Amber warning)
+ (UIColor *)statusErr;        // #EF4444 / #F87171 (Coral red)
+ (UIColor *)statusCyan;       // #38BDF8 (Sky Blue metric highlight)

// Glass Translucency Tokens
+ (UIColor *)glassDarkBase;                    // rgba(18, 18, 24, 0.70)
+ (UIColor *)glassDarkWithOpacity:(CGFloat)pct; // base glass tint scaled by opacity %
+ (UIColor *)glassBorderGold;                  // rgba(186, 161, 100, 0.65)
+ (UIColor *)glassBorderWhite;                 // rgba(255, 255, 255, 0.25)
+ (UIColor *)glassFill;                        // rgba(255, 255, 255, 0.08)

// Component Factory Helpers
+ (UIVisualEffectView *)createGlassCardWithCornerRadius:(CGFloat)radius;
+ (void)applyGlassDoubleRimToView:(UIView *)view cornerRadius:(CGFloat)radius;
+ (UIButton *)createGlassButtonWithTitle:(NSString *)title
                               baseColor:(UIColor *)baseColor
                            cornerRadius:(CGFloat)radius;

+ (UIImage * _Nullable)loadBrandLogo;

@end

NS_ASSUME_NONNULL_END
