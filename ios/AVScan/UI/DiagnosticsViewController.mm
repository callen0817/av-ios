#import "DiagnosticsViewController.h"

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
        self.title = @"Airy Ethernet & Packet Diagnostics";
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.07 green:0.09 blue:0.12 alpha:1.0];

    auto cfg = [_bridge currentNetworkConfig];

    UIStackView *stack = [[UIStackView alloc] initWithFrame:CGRectInset(self.view.bounds, 20, 60)];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10.0;
    stack.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:stack];

    _sensorIpField = [self makeFieldWithPlaceholder:@"Airy IP" text:[NSString stringWithUTF8String:cfg.sensor_ip.c_str()]];
    _hostIpField = [self makeFieldWithPlaceholder:@"iOS Ethernet IP" text:[NSString stringWithUTF8String:cfg.host_expected_ip.c_str()]];
    _msopPortField = [self makeFieldWithPlaceholder:@"MSOP UDP Port" text:[NSString stringWithFormat:@"%u", cfg.msop_port]];
    _difopPortField = [self makeFieldWithPlaceholder:@"DIFOP UDP Port" text:[NSString stringWithFormat:@"%u", cfg.difop_port]];
    _imuPortField = [self makeFieldWithPlaceholder:@"IMU UDP Port" text:[NSString stringWithFormat:@"%u", cfg.imu_port]];

    [stack addArrangedSubview:_sensorIpField];
    [stack addArrangedSubview:_hostIpField];
    [stack addArrangedSubview:_msopPortField];
    [stack addArrangedSubview:_difopPortField];
    [stack addArrangedSubview:_imuPortField];

    UIButton *probeBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [probeBtn setTitle:@"Run Gate 1 & Gate 2 Airy Hardware Probe" forState:UIControlStateNormal];
    probeBtn.backgroundColor = [UIColor colorWithRed:0.0 green:0.48 blue:1.0 alpha:1.0];
    [probeBtn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    probeBtn.layer.cornerRadius = 8.0;
    [probeBtn addTarget:self action:@selector(onRunProbeTapped) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:probeBtn];

    _diagnosticLogView = [[UITextView alloc] init];
    _diagnosticLogView.editable = NO;
    _diagnosticLogView.backgroundColor = [UIColor colorWithRed:0.04 green:0.05 blue:0.07 alpha:1.0];
    _diagnosticLogView.textColor = [UIColor colorWithRed:0.45 green:0.95 blue:0.65 alpha:1.0];
    _diagnosticLogView.font = [UIFont monospacedSystemFontOfSize:12.0 weight:UIFontWeightRegular];
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
    tf.backgroundColor = [UIColor colorWithWhite:0.15 alpha:1.0];
    tf.textColor = [UIColor whiteColor];
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
    [log appendFormat:@"[GATE 1 DIAGNOSTICS]\n"];
    [log appendFormat:@"Status:           %s\n", rep.diagnostic_message.c_str()];
    [log appendFormat:@"Active Interface: %s (%s)\n", rep.active_interface_name.c_str(), rep.active_host_ip.c_str()];
    [log appendFormat:@"Adapter Detected: %@\n", rep.adapter_detected ? @"YES" : @"NO"];
    [log appendFormat:@"Subnet Matched:   %@\n", rep.subnet_matched ? @"YES" : @"NO"];
    [log appendFormat:@"HTTP Reachable:   %@ (%.2f ms)\n", rep.http_reachable ? @"YES" : @"NO", rep.probe_latency_ms];
    [log appendFormat:@"DIFOP Received:   %@\n\n", rep.difop_received ? @"YES" : @"NO"];

    if (rep.device_info.valid) {
        [log appendFormat:@"[AIRY DEVICE METADATA]\n"];
        [log appendFormat:@"Mainboard FW:     %s\n", rep.device_info.mainboard_firmware.c_str()];
        [log appendFormat:@"Baseboard FW:     %s\n", rep.device_info.baseboard_firmware.c_str()];
        [log appendFormat:@"MAC Address:      %s\n", rep.device_info.mac_address.c_str()];
        [log appendFormat:@"Configured Ports: MSOP=%u DIFOP=%u IMU=%u\n\n",
         rep.device_info.msop_port, rep.device_info.difop_port, rep.device_info.imu_port];
    }

    [log appendFormat:@"[GATE 2 PACKET & SYNC COUNTERS]\n"];
    [log appendFormat:@"Valid MSOP Pkts:  %llu (Malformed: %llu, Loss: %.3f%%)\n",
     pkt.msop_packets_valid, pkt.msop_packets_malformed, pkt.msopPacketLossRatio() * 100.0];
    [log appendFormat:@"Valid DIFOP Pkts: %llu\n", pkt.difop_packets_valid];
    [log appendFormat:@"Valid IMU Pkts:   %llu (Seq Gaps: %llu)\n", pkt.imu_packets_valid, pkt.imu_sequence_gaps];
    [log appendFormat:@"Clock Offset Est: %.4f s (Jitter: %.2f ms, Locked: %@)\n",
     sync.estimated_clock_offset_sec, sync.clock_jitter_ms, sync.clock_locked ? @"YES" : @"NO"];

    _diagnosticLogView.text = log;
}

- (void)refreshLivePacketStats {
    auto pkt = [_bridge currentPacketStats];
    if (pkt.msop_packets_valid > 0 || pkt.imu_packets_valid > 0) {
        [self onRunProbeTapped];
    }
}

@end
