#import "DiagnosticsViewController.h"
#import "AppleGlassTheme.h"

@implementation DiagnosticsViewController {
    AVScanCoreBridge *_bridge;
    UITextField *_sensorIpField;
    UITextField *_hostIpField;
    UITextField *_msopPortField;
    UITextField *_difopPortField;
    UITextField *_imuPortField;
    UITextView *_diagnosticLogView;
    NSTimer *_refreshTimer;
}

- (instancetype)initWithBridge:(AVScanCoreBridge *)bridge {
    self = [super init];
    if (self) {
        _bridge = bridge;
        self.title = @"Airy Ethernet & Diagnostics";
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:12.0/255.0 green:12.0/255.0 blue:16.0/255.0 alpha:1.0];

    auto cfg = [_bridge currentNetworkConfig];

    UIStackView *stack = [[UIStackView alloc] init];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10.0;
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

    _sensorIpField = [self makeFieldWithPlaceholder:@"Airy IP" text:[NSString stringWithUTF8String:cfg.sensor_ip.c_str()]];
    _hostIpField = [self makeFieldWithPlaceholder:@"iOS Ethernet IP" text:[NSString stringWithUTF8String:cfg.host_expected_ip.c_str()]];
    _msopPortField = [self makeFieldWithPlaceholder:@"MSOP UDP Port" text:[NSString stringWithFormat:@"%u", cfg.msop_port]];
    _difopPortField = [self makeFieldWithPlaceholder:@"DIFOP UDP Port" text:[NSString stringWithFormat:@"%u", cfg.difop_port]];
    _imuPortField = [self makeFieldWithPlaceholder:@"IMU UDP Port" text:[NSString stringWithFormat:@"%u", cfg.imu_port]];

    for (UITextField *tf in @[_sensorIpField, _hostIpField, _msopPortField, _difopPortField, _imuPortField]) {
        [tf.heightAnchor constraintEqualToConstant:36.0].active = YES;
        [stack addArrangedSubview:tf];
    }

    UIButton *probeBtn = [AppleGlassTheme createGlassButtonWithTitle:@"Run Gate 1 & Gate 2 Airy Hardware Probe"
                                                           baseColor:[AppleGlassTheme goldPrimary]
                                                        cornerRadius:10.0];
    [probeBtn.heightAnchor constraintEqualToConstant:46.0].active = YES;
    [probeBtn addTarget:self action:@selector(onRunProbeTapped) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:probeBtn];

    _diagnosticLogView = [[UITextView alloc] init];
    _diagnosticLogView.editable = NO;
    _diagnosticLogView.backgroundColor = [UIColor colorWithRed:16.0/255.0 green:16.0/255.0 blue:22.0/255.0 alpha:0.88];
    _diagnosticLogView.textColor = [AppleGlassTheme statusOk];
    _diagnosticLogView.font = [UIFont monospacedSystemFontOfSize:11.5 weight:UIFontWeightRegular];
    _diagnosticLogView.layer.cornerRadius = 10.0;
    _diagnosticLogView.clipsToBounds = YES;
    [AppleGlassTheme applyGlassDoubleRimToView:_diagnosticLogView cornerRadius:10.0];
    [stack addArrangedSubview:_diagnosticLogView];

    [self onRunProbeTapped];
    _refreshTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                     target:self
                                                   selector:@selector(refreshLivePacketStats)
                                                   userInfo:nil
                                                    repeats:YES];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [_refreshTimer invalidate];
    _refreshTimer = nil;
}

- (UITextField *)makeFieldWithPlaceholder:(NSString *)placeholder text:(NSString *)text {
    UITextField *tf = [[UITextField alloc] init];
    tf.placeholder = placeholder;
    tf.text = text;
    tf.borderStyle = UITextBorderStyleRoundedRect;
    tf.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.08];
    tf.textColor = [AppleGlassTheme whiteWarm];
    tf.layer.cornerRadius = 8.0;
    tf.layer.borderWidth = 1.0;
    tf.layer.borderColor = [AppleGlassTheme glassBorderGold].CGColor;
    return tf;
}

- (void)onRunProbeTapped {
    avscan::sensors::AiryNetworkConfig cfg;
    cfg.sensor_ip = _sensorIpField.text.UTF8String;
    cfg.host_expected_ip = _hostIpField.text.UTF8String;
    cfg.msop_port = static_cast<uint16_t>([_msopPortField.text intValue]);
    cfg.difop_port = static_cast<uint16_t>([_difopPortField.text intValue]);
    cfg.imu_port = static_cast<uint16_t>([_imuPortField.text intValue]);
    [_bridge updateNetworkConfig:cfg];

    auto rep = [_bridge runAiryDiagnostics];
    auto pkt = [_bridge currentPacketStats];
    auto sync = [_bridge currentSyncDiagnostics];

    NSMutableString *log = [NSMutableString string];
    [log appendFormat:@"=== [ARTIFICIAL VISION — AIRY GATE 1 DIAGNOSTICS] ===\n"];
    [log appendFormat:@"Adapter Detected: %@\n", rep.adapter_detected ? @"ONLINE (Belkin GbE)" : @"OFFLINE"];
    [log appendFormat:@"HTTP Reachable:   %@ (Latency: %.2f ms)\n", rep.http_reachable ? @"REACHABLE" : @"UNREACHABLE", rep.probe_latency_ms];
    [log appendFormat:@"Subnet Match:     %@\n", rep.subnet_matched ? @"MATCHED (192.168.1.102/24)" : @"MISMATCH"];
    [log appendFormat:@"DIFOP Received:   %@\n", rep.difop_received ? @"ACTIVE" : @"WAITING"];
    [log appendFormat:@"Device MAC:       %s\n", rep.device_info.mac_address.c_str()];
    [log appendFormat:@"Firmware Version: %s\n", rep.device_info.mainboard_firmware.c_str()];
    [log appendFormat:@"Diagnostics:      %s\n\n", rep.diagnostic_message.c_str()];

    [log appendFormat:@"=== [LIVE UDP STREAM TELEMETRY] ===\n"];
    [log appendFormat:@"MSOP Packets:     %llu valid (%llu gaps)\n", pkt.msop_packets_valid, pkt.msop_sequence_gaps];
    [log appendFormat:@"DIFOP Packets:    %llu valid (%llu malformed)\n", pkt.difop_packets_valid, pkt.difop_packets_malformed];
    [log appendFormat:@"IMU Packets:      %llu valid (%llu gaps)\n", pkt.imu_packets_valid, pkt.imu_sequence_gaps];
    [log appendFormat:@"Packet Loss:      %.2f%%\n", pkt.msopPacketLossRatio() * 100.0];
    [log appendFormat:@"Bytes Received:   %llu bytes\n\n", pkt.total_bytes_received];

    [log appendFormat:@"=== [HARDWARE CLOCK SYNCHRONIZATION] ===\n"];
    [log appendFormat:@"LiDAR Frames In:  %llu\n", sync.lidar_frames_received];
    [log appendFormat:@"IMU Samples In:   %llu\n", sync.imu_samples_received];
    [log appendFormat:@"Clock Offset:     %.4f s\n", sync.estimated_clock_offset_sec];
    [log appendFormat:@"Camera Offset:    %.4f s\n", sync.calibrated_cam_offset_sec];
    [log appendFormat:@"Clock Jitter:     %.3f ms\n", sync.clock_jitter_ms];
    [log appendFormat:@"Clock Locked:     %@\n", sync.clock_locked ? @"YES" : @"SOFTWARE ES-EKF ESTIMATION"];

    _diagnosticLogView.text = log;
}

- (void)refreshLivePacketStats {
    auto pkt = [_bridge currentPacketStats];
    if (pkt.msop_packets_valid > 0) {
        [self onRunProbeTapped];
    }
}

@end
