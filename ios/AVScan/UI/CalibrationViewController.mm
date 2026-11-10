#import "CalibrationViewController.h"

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
    self.view.backgroundColor = [UIColor colorWithRed:0.07 green:0.09 blue:0.12 alpha:1.0];

    UIStackView *stack = [[UIStackView alloc] initWithFrame:CGRectInset(self.view.bounds, 20, 60)];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12.0;
    stack.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:stack];

    _statusLabel = [[UILabel alloc] init];
    _statusLabel.numberOfLines = 0;
    _statusLabel.textColor = [UIColor whiteColor];
    _statusLabel.text = @"Step 1: Slowly rotate scanner around pitch & yaw axes for Hand-Eye (AX=XB) initialization.\nStep 2: Point at structured edges for DirectVLCalib photometric refinement.";
    [stack addArrangedSubview:_statusLabel];

    UIButton *solveBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [solveBtn setTitle:@"Execute DirectVLCalib + Hand-Eye Calibration" forState:UIControlStateNormal];
    solveBtn.backgroundColor = [UIColor colorWithRed:0.15 green:0.65 blue:0.35 alpha:1.0];
    [solveBtn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    solveBtn.layer.cornerRadius = 8.0;
    [solveBtn addTarget:self action:@selector(onSolveCalibrationTapped) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:solveBtn];

    _resultView = [[UITextView alloc] init];
    _resultView.editable = NO;
    _resultView.backgroundColor = [UIColor colorWithRed:0.04 green:0.05 blue:0.07 alpha:1.0];
    _resultView.textColor = [UIColor colorWithRed:0.75 green:0.92 blue:1.0 alpha:1.0];
    _resultView.font = [UIFont monospacedSystemFontOfSize:12.0 weight:UIFontWeightRegular];
    [stack addArrangedSubview:_resultView];

    [self refreshCurrentProfileDisplay];
}

- (void)refreshCurrentProfileDisplay {
    auto prof = [_bridge currentCalibrationProfile];
    auto rpy = prof.T_cam_lidar.so3.unit_quaternion().toEulerRollPitchYaw();
    const auto &t = prof.T_cam_lidar.t;

    NSMutableString *txt = [NSMutableString string];
    [txt appendFormat:@"[ACTIVE CALIBRATION PROFILE]\n"];
    [txt appendFormat:@"Profile ID:       %s\n", prof.profile_id.c_str()];
    [txt appendFormat:@"Verified Status:  %@\n", prof.verified ? @"VERIFIED" : @"UNVERIFIED (Requires Calibration)"];
    [txt appendFormat:@"Confidence Score: %.3f\n", prof.confidence_score];
    [txt appendFormat:@"NID Metric:       %.4f\n", prof.nid_metric];
    [txt appendFormat:@"Hand-Eye Resid:   %.3f deg\n", prof.hand_eye_rot_residual_deg];
    [txt appendFormat:@"Cam Time Offset:  %.4f s\n", prof.camera_time_offset_sec];
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
        _statusLabel.text = @"Calibration VERIFIED and bound to active mount profile.";
    } else {
        _statusLabel.text = @"Calibration REJECTED: Insufficient multi-axis rotational excitation or visual-LiDAR overlap. Collect additional motion pairs.";
    }
    [self refreshCurrentProfileDisplay];
}

@end
