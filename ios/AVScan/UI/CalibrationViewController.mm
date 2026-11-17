#import "CalibrationViewController.h"
#import "AppleGlassTheme.h"

@implementation CalibrationViewController {
    AVScanCoreBridge *_bridge;
    UILabel *_statusLabel;
    UITextView *_resultView;
    std::vector<avscan::calibration::HandEyeMotionPair> _motionPairs;
    std::vector<avscan::calibration::DirectVLCalibPair> _vlPairs;
    std::vector< std::pair<double, double> > _imuGyroSeries;
    std::vector< std::pair<double, double> > _camRotSeries;
}

- (instancetype)initWithBridge:(AVScanCoreBridge *)bridge {
    self = [super init];
    if (self) {
        _bridge = bridge;
        self.title = @"Targetless Camera-LiDAR Calibration";
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:12.0/255.0 green:12.0/255.0 blue:16.0/255.0 alpha:1.0];

    UIStackView *stack = [[UIStackView alloc] initWithFrame:CGRectInset(self.view.bounds, 20, 72)];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12.0;
    stack.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:stack];

    _statusLabel = [[UILabel alloc] init];
    _statusLabel.numberOfLines = 0;
    _statusLabel.textColor = [AppleGlassTheme goldBright];
    _statusLabel.font = [UIFont systemFontOfSize:13.0 weight:UIFontWeightMedium];
    _statusLabel.text = @"Step 1: Slowly rotate scanner around pitch & yaw axes for Hand-Eye (AX=XB) initialization.\nStep 2: Point at structured edges for DirectVLCalib photometric refinement.";
    [stack addArrangedSubview:_statusLabel];

    UIButton *solveBtn = [AppleGlassTheme createGlassButtonWithTitle:@"Execute DirectVLCalib + Hand-Eye Calibration"
                                                           baseColor:[AppleGlassTheme statusOk]
                                                        cornerRadius:10.0];
    [solveBtn addTarget:self action:@selector(onSolveCalibrationTapped) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:solveBtn];

    _resultView = [[UITextView alloc] init];
    _resultView.editable = NO;
    _resultView.backgroundColor = [UIColor colorWithRed:16.0/255.0 green:16.0/255.0 blue:22.0/255.0 alpha:0.88];
    _resultView.textColor = [AppleGlassTheme statusCyan];
    _resultView.font = [UIFont monospacedSystemFontOfSize:11.5 weight:UIFontWeightRegular];
    _resultView.layer.cornerRadius = 10.0;
    _resultView.clipsToBounds = YES;
    [AppleGlassTheme applyGlassDoubleRimToView:_resultView cornerRadius:10.0];
    [stack addArrangedSubview:_resultView];

    [self refreshCurrentProfileDisplay];
}

- (void)refreshCurrentProfileDisplay {
    auto prof = [_bridge currentCalibrationProfile];
    auto rpy = prof.T_cam_lidar.so3.unit_quaternion().toEulerRollPitchYaw();
    const auto &t = prof.T_cam_lidar.t;

    NSMutableString *txt = [NSMutableString string];
    [txt appendFormat:@"=== [ACTIVE EXTRINSIC CALIBRATION PROFILE] ===\n"];
    [txt appendFormat:@"Profile ID:       %s\n", prof.profile_id.c_str()];
    [txt appendFormat:@"Status:           %@\n", prof.verified ? @"VERIFIED & BOUND" : @"UNVERIFIED (Calibration Required)"];
    [txt appendFormat:@"Confidence Score: %.3f\n", prof.confidence_score];
    [txt appendFormat:@"NID Metric:       %.4f\n", prof.nid_metric];
    [txt appendFormat:@"Hand-Eye Resid:   %.3f deg\n", prof.hand_eye_rot_residual_deg];
    [txt appendFormat:@"Cam Time Offset:  %.4f s\n\n", prof.camera_time_offset_sec];

    [txt appendFormat:@"=== [SPATIAL TRANSFORM (CAMERA <- LIDAR)] ===\n"];
    [txt appendFormat:@"Translation (m):  [%.4f, %.4f, %.4f]\n", t.x, t.y, t.z];
    [txt appendFormat:@"Euler RPY (deg):  [%.2f, %.2f, %.2f]\n",
     rpy.x * avscan::math::kRadToDeg,
     rpy.y * avscan::math::kRadToDeg,
     rpy.z * avscan::math::kRadToDeg];
    _resultView.text = txt;
}

- (void)onSolveCalibrationTapped {
    avscan::core::ExtrinsicCalibrationProfile outProfile;
    NSString *deviceModel = [[UIDevice currentDevice] model] ?: @"iOS_Device";

    bool ok = avscan::calibration::TargetlessCalibrator::runFullTargetlessCalibration(
        _motionPairs,
        _vlPairs,
        _imuGyroSeries,
        _camRotSeries,
        deviceModel.UTF8String,
        "rigid_belkin_mount",
        outProfile);

    if (ok && outProfile.verified) {
        [_bridge applyCalibrationProfile:outProfile];
        _statusLabel.textColor = [AppleGlassTheme statusOk];
        _statusLabel.text = @"Calibration VERIFIED and bound to active mount profile.";
    } else {
        _statusLabel.textColor = [AppleGlassTheme statusWarn];
        _statusLabel.text = @"Calibration REJECTED: Insufficient multi-axis rotational excitation or visual-LiDAR overlap. Collect additional motion pairs.";
    }
    [self refreshCurrentProfileDisplay];
}

@end
