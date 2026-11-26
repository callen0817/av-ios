#import "CalibrationViewController.h"
#import "AppleGlassTheme.h"

@implementation CalibrationViewController {
    AVScanCoreBridge *_bridge;
    UILabel *_statusLabel;
    UITextView *_resultView;
    NSTimer *_refreshTimer;
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

    UIStackView *stack = [[UIStackView alloc] init];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12.0;
    stack.alignment = UIStackViewAlignmentFill;
    stack.distribution = UIStackViewDistributionFill;
    [self.view addSubview:stack];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:16.0],
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:20.0],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-20.0],
        [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-16.0]
    ]];

    _statusLabel = [[UILabel alloc] init];
    _statusLabel.numberOfLines = 0;
    _statusLabel.textColor = [AppleGlassTheme goldBright];
    _statusLabel.font = [UIFont systemFontOfSize:13.5 weight:UIFontWeightMedium];
    _statusLabel.text = @"Step 1: Start a Mobile SLAM scan and slowly rotate around pitch & yaw axes for Hand-Eye (AX=XB) initialization.\nStep 2: Point at structured edges in well-lit areas for DirectVLCalib photometric refinement.";
    [_statusLabel setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    [_statusLabel setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    [stack addArrangedSubview:_statusLabel];

    UIStackView *btnRow = [[UIStackView alloc] init];
    btnRow.axis = UILayoutConstraintAxisHorizontal;
    btnRow.distribution = UIStackViewDistributionFillEqually;
    btnRow.spacing = 10.0;
    [btnRow.heightAnchor constraintEqualToConstant:46.0].active = YES;

    UIButton *recalibBtn = [AppleGlassTheme createGlassButtonWithTitle:@"Reset & Re-Run Auto-Calib"
                                                             baseColor:[AppleGlassTheme statusOk]
                                                          cornerRadius:10.0];
    [recalibBtn addTarget:self action:@selector(onResetAutoCalibTapped) forControlEvents:UIControlEventTouchUpInside];

    UIButton *seedBtn = [AppleGlassTheme createGlassButtonWithTitle:@"Apply Active Rig Seed"
                                                          baseColor:[AppleGlassTheme goldPrimary]
                                                       cornerRadius:10.0];
    [seedBtn addTarget:self action:@selector(onApplyRigSeedTapped) forControlEvents:UIControlEventTouchUpInside];

    [btnRow addArrangedSubview:recalibBtn];
    [btnRow addArrangedSubview:seedBtn];
    [stack addArrangedSubview:btnRow];

    _resultView = [[UITextView alloc] init];
    _resultView.editable = NO;
    _resultView.backgroundColor = [UIColor colorWithRed:16.0/255.0 green:16.0/255.0 blue:22.0/255.0 alpha:0.88];
    _resultView.textColor = [AppleGlassTheme statusCyan];
    _resultView.font = [UIFont monospacedSystemFontOfSize:12.0 weight:UIFontWeightRegular];
    _resultView.textContainerInset = UIEdgeInsetsMake(12, 12, 12, 12);
    _resultView.layer.cornerRadius = 10.0;
    _resultView.clipsToBounds = YES;
    [AppleGlassTheme applyGlassDoubleRimToView:_resultView cornerRadius:10.0];
    [stack addArrangedSubview:_resultView];

    [self refreshCurrentProfileDisplay];
    _refreshTimer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                     target:self
                                                   selector:@selector(refreshCurrentProfileDisplay)
                                                   userInfo:nil
                                                    repeats:YES];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [_refreshTimer invalidate];
    _refreshTimer = nil;
}

- (void)refreshCurrentProfileDisplay {
    auto prof = [_bridge currentCalibrationProfile];
    auto autoStatus = [_bridge currentAutoCalibStatus];
    auto rpy = prof.T_cam_lidar.so3.unit_quaternion().toEulerRollPitchYaw();
    const auto &t = prof.T_cam_lidar.t;

    NSMutableString *txt = [NSMutableString string];
    [txt appendFormat:@"=== [LIVE CAMERA & AUTO-CALIBRATOR] ===\n"];
    [txt appendFormat:@"Camera Source:    %@\n", [_bridge activeProfileUsesExternalUsbCamera] ? @"External USB-C UVC (Orbbec Gemini 336L — iPad cam OFF)" : @"Built-in iPad / iPhone Wide Camera"];
    [txt appendFormat:@"Camera Stream:    %@ (%.1f fps, %llu frames)\n",
     [_bridge cameraStatusSummary], [_bridge cameraMeasuredFps],
     static_cast<unsigned long long>([_bridge cameraFramesDelivered])];
    if ([_bridge activeProfileUsesExternalUsbCamera]) {
        NSArray<NSString *> *usbCams = [_bridge connectedExternalUsbCameras];
        [txt appendFormat:@"USB UVC Detected: %@\n", usbCams.count > 0 ? [usbCams componentsJoinedByString:@", "] : @"None (plug ScanAR USB-C hub into iPadOS 17+)"];
    }
    [txt appendFormat:@"Auto-Calib State: %s\n", autoStatus.popup_title.empty() ? "IDLE (Profile Ready)" : autoStatus.popup_title.c_str()];
    [txt appendFormat:@"Guidance:         %s\n\n", autoStatus.status_message.empty() ? "Ready — selected Device Rig Profile will engage RGB colorization immediately on Start Scan." : autoStatus.status_message.c_str()];

    [txt appendFormat:@"=== [ACTIVE EXTRINSIC CALIBRATION PROFILE] ===\n"];
    [txt appendFormat:@"Device Rig:       %@\n", [_bridge activeDeviceProfileDisplayName]];
    [txt appendFormat:@"Profile ID:       %s\n", prof.profile_id.c_str()];
    [txt appendFormat:@"Status:           %@\n", prof.verified ? @"VERIFIED & ACTIVE (RGB Colorizing)" : @"UNVERIFIED (Awaiting Scratch Auto-Calib Lock)"];
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

- (void)onResetAutoCalibTapped {
    [_bridge resetAutoCalibration];
    _statusLabel.textColor = [AppleGlassTheme statusOk];
    _statusLabel.text = @"Auto-calibrator reset. Return to the scanner, tap Start Scan, and slowly rotate around pitch & yaw axes in a lit environment to lock.";
    [self refreshCurrentProfileDisplay];
}

- (void)onApplyRigSeedTapped {
    NSString *activeId = [_bridge activeDeviceProfileId];
    [_bridge setActiveDeviceProfileWithId:activeId];
    _statusLabel.textColor = [AppleGlassTheme goldBright];
    _statusLabel.text = [NSString stringWithFormat:@"Applied seed extrinsics for %@.", [_bridge activeDeviceProfileDisplayName]];
    [self refreshCurrentProfileDisplay];
}

@end
