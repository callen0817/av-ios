#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * Artificial Vision — Apple Glass (visionOS) White & Gold Splash Screen
 * Exact replica of scanarMini ArtificialVisionSplashScreen (560x380 card)
 * as implemented in av-scan (AppWindow.qml) and av-scanar (scanhub_app.py).
 */
@interface AppleGlassSplashScreenView : UIView

@property (nonatomic, copy, nullable) void (^onDismiss)(void);

- (instancetype)initWithFrame:(CGRect)frame;
- (void)setProgress:(CGFloat)progress statusText:(NSString *)statusText;
- (void)dismissAnimated;

@end

NS_ASSUME_NONNULL_END
