#import "MainScannerViewController.h"
#import "DiagnosticsViewController.h"
#import "CalibrationViewController.h"

@implementation MainScannerViewController {
    AVScanCoreBridge *_bridge;
    MTKView *_mtkView;
    MetalPointCloudRenderer *_renderer;
    FloorPlanView *_floorPlanView;

    UILabel *_hudStatusBanner;
    UILabel *_hudMetricsLabel;
    UIButton *_startStopBtn;
    UIButton *_saveBtn;
    UIButton *_exportBtn;
    UIButton *_poiPhotoBtn;
    UISegmentedControl *_colorModeControl;
    NSTimer *_uiTimer;
    CGPoint _lastPanPt;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"AV Scan — FAST-LIVO2";
    self.view.backgroundColor = [UIColor blackColor];

    _bridge = [[AVScanCoreBridge alloc] init];
    [_bridge createOrOpenProjectWithId:@"PROJECT_001" name:@"LEVEL_01"];

    _mtkView = [[MTKView alloc] initWithFrame:self.view.bounds];
    _mtkView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:_mtkView];

    _renderer = [[MetalPointCloudRenderer alloc] initWithMetalKitView:_mtkView];
    _mtkView.delegate = _renderer;

    [self setupGestures];
    [self setupHudAndControls];

    _uiTimer = [NSTimer scheduledTimerWithTimeInterval:0.10
                                                target:self
                                              selector:@selector(onUiRefreshTick)
                                              userInfo:nil
                                               repeats:YES];
}

- (void)setupGestures {
    UIPanGestureRecognizer *orbitGesture = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handlePanGesture:)];
    orbitGesture.maximumNumberOfTouches = 2;
    [_mtkView addGestureRecognizer:orbitGesture];

    UIPinchGestureRecognizer *pinchGesture = [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(handlePinchGesture:)];
    [_mtkView addGestureRecognizer:pinchGesture];
}

- (void)handlePanGesture:(UIPanGestureRecognizer *)recognizer {
    CGPoint pt = [recognizer translationInView:_mtkView];
    if (recognizer.state == UIGestureRecognizerStateBegan) {
        _lastPanPt = pt;
    } else if (recognizer.state == UIGestureRecognizerStateChanged) {
        float dx = static_cast<float>(pt.x - _lastPanPt.x);
        float dy = static_cast<float>(pt.y - _lastPanPt.y);
        _lastPanPt = pt;
        if (recognizer.numberOfTouches >= 2) {
            [_renderer panByDeltaX:dx deltaY:dy];
        } else {
            [_renderer orbitByDeltaYaw:-dx * 0.008f deltaPitch:dy * 0.008f];
        }
    }
}

- (void)handlePinchGesture:(UIPinchGestureRecognizer *)recognizer {
    if (recognizer.state == UIGestureRecognizerStateChanged) {
        [_renderer zoomByScale:static_cast<float>(recognizer.scale)];
        recognizer.scale = 1.0;
    }
}

- (void)setupHudAndControls {
    UIBarButtonItem *diagItem = [[UIBarButtonItem alloc] initWithTitle:@"Airy Net"
                                                                 style:UIBarButtonItemStylePlain
                                                                target:self
                                                                action:@selector(openDiagnostics)];
    UIBarButtonItem *calibItem = [[UIBarButtonItem alloc] initWithTitle:@"Calibrate"
                                                                  style:UIBarButtonItemStylePlain
                                                                 target:self
                                                                 action:@selector(openCalibration)];
    self.navigationItem.rightBarButtonItems = @[diagItem, calibItem];

    _hudStatusBanner = [[UILabel alloc] initWithFrame:CGRectMake(16, 96, 360, 56)];
    _hudStatusBanner.backgroundColor = [UIColor colorWithRed:0.05 green:0.08 blue:0.12 alpha:0.85];
    _hudStatusBanner.textColor = [UIColor whiteColor];
    _hudStatusBanner.font = [UIFont monospacedSystemFontOfSize:12.0 weight:UIFontWeightSemibold];
    _hudStatusBanner.numberOfLines = 3;
    _hudStatusBanner.layer.cornerRadius = 8.0;
    _hudStatusBanner.clipsToBounds = YES;
    [self.view addSubview:_hudStatusBanner];

    _hudMetricsLabel = [[UILabel alloc] initWithFrame:CGRectMake(16, 160, 360, 44)];
    _hudMetricsLabel.backgroundColor = [UIColor colorWithRed:0.05 green:0.08 blue:0.12 alpha:0.85];
    _hudMetricsLabel.textColor = [UIColor colorWithRed:0.45 green:0.95 blue:0.75 alpha:1.0];
    _hudMetricsLabel.font = [UIFont monospacedSystemFontOfSize:11.0 weight:UIFontWeightRegular];
    _hudMetricsLabel.numberOfLines = 2;
    _hudMetricsLabel.layer.cornerRadius = 8.0;
    _hudMetricsLabel.clipsToBounds = YES;
    [self.view addSubview:_hudMetricsLabel];

    _floorPlanView = [[FloorPlanView alloc] initWithFrame:CGRectMake(self.view.bounds.size.width - 236, 96, 220, 220)];
    _floorPlanView.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin;
    [self.view addSubview:_floorPlanView];

    _colorModeControl = [[UISegmentedControl alloc] initWithItems:@[@"RGB", @"Intensity", @"Elev Z"]];
    _colorModeControl.selectedSegmentIndex = 0;
    _colorModeControl.frame = CGRectMake(16, self.view.bounds.size.height - 130, 260, 34);
    _colorModeControl.autoresizingMask = UIViewAutoresizingFlexibleTopMargin;
    [_colorModeControl addTarget:self action:@selector(onColorModeChanged:) forControlEvents:UIControlEventValueChanged];
    [self.view addSubview:_colorModeControl];

    UIStackView *bottomBar = [[UIStackView alloc] initWithFrame:CGRectMake(16, self.view.bounds.size.height - 80, self.view.bounds.size.width - 32, 50)];
    bottomBar.axis = UILayoutConstraintAxisHorizontal;
    bottomBar.distribution = UIStackViewDistributionFillEqually;
    bottomBar.spacing = 12.0;
    bottomBar.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin;
    [self.view addSubview:bottomBar];

    _startStopBtn = [self makeActionButton:@"Start Scan" color:[UIColor colorWithRed:0.10 green:0.72 blue:0.36 alpha:1.0] action:@selector(onStartStopTapped)];
    _poiPhotoBtn = [self makeActionButton:@"POI Photo" color:[UIColor colorWithRed:0.20 green:0.45 blue:0.85 alpha:1.0] action:@selector(onPoiPhotoTapped)];
    _saveBtn = [self makeActionButton:@"Save & Refine" color:[UIColor colorWithRed:0.85 green:0.55 blue:0.10 alpha:1.0] action:@selector(onSaveTapped)];
    _exportBtn = [self makeActionButton:@"Export PLY/E57" color:[UIColor colorWithRed:0.45 green:0.30 blue:0.85 alpha:1.0] action:@selector(onExportTapped)];

    [bottomBar addArrangedSubview:_startStopBtn];
    [bottomBar addArrangedSubview:_poiPhotoBtn];
    [bottomBar addArrangedSubview:_saveBtn];
    [bottomBar addArrangedSubview:_exportBtn];
}

- (UIButton *)makeActionButton:(NSString *)title color:(UIColor *)color action:(SEL)action {
    UIButton *btn = [UIButton buttonWithType:UIButtonTypeSystem];
    [btn setTitle:title forState:UIControlStateNormal];
    btn.backgroundColor = color;
    [btn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont systemFontOfSize:14.0 weight:UIFontWeightBold];
    btn.layer.cornerRadius = 10.0;
    [btn addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return btn;
}

- (void)onColorModeChanged:(UISegmentedControl *)seg {
    _renderer.colorMode = static_cast<int>(seg.selectedSegmentIndex);
}

- (void)onStartStopTapped {
    if (_bridge.isScanning) {
        [_bridge stopScanningSession];
        [_startStopBtn setTitle:@"Start Scan" forState:UIControlStateNormal];
        _startStopBtn.backgroundColor = [UIColor colorWithRed:0.10 green:0.72 blue:0.36 alpha:1.0];
    } else {
        [_bridge startScanningSessionWithVisualFusion:YES useArkitCamera:NO];
        [_startStopBtn setTitle:@"Stop Scan" forState:UIControlStateNormal];
        _startStopBtn.backgroundColor = [UIColor colorWithRed:0.88 green:0.22 blue:0.22 alpha:1.0];
    }
}

- (void)onPoiPhotoTapped {
    [_bridge capturePointOfInterestPhotoWithNote:@"Operator POI inspection snapshot"];
}

- (void)onSaveTapped {
    [_bridge saveAndRefineSession];
    [_startStopBtn setTitle:@"Start Scan" forState:UIControlStateNormal];
    _startStopBtn.backgroundColor = [UIColor colorWithRed:0.10 green:0.72 blue:0.36 alpha:1.0];
}

- (void)onExportTapped {
    NSString *projDir = _bridge.activeProjectDir;
    if (!projDir) return;

    NSString *plyPath = [projDir stringByAppendingPathComponent:@"avscan_map.ply"];
    NSString *pcdPath = [projDir stringByAppendingPathComponent:@"avscan_map.pcd"];
    NSString *e57Path = [projDir stringByAppendingPathComponent:@"avscan_map.e57"];

    BOOL plyOk = [_bridge exportCurrentMapToPly:plyPath];
    BOOL pcdOk = [_bridge exportCurrentMapToPcd:pcdPath];
    BOOL e57Ok = [_bridge exportCurrentMapToE57:e57Path];

    NSMutableArray<NSURL *> *items = [NSMutableArray array];
    if (plyOk) [items addObject:[NSURL fileURLWithPath:plyPath]];
    if (pcdOk) [items addObject:[NSURL fileURLWithPath:pcdPath]];
    if (e57Ok) [items addObject:[NSURL fileURLWithPath:e57Path]];

    if (items.count > 0) {
        UIActivityViewController *shareVC = [[UIActivityViewController alloc] initWithActivityItems:items applicationActivities:nil];
        shareVC.popoverPresentationController.sourceView = _exportBtn;
        [self presentViewController:shareVC animated:YES completion:nil];
    }
}

- (void)openDiagnostics {
    DiagnosticsViewController *vc = [[DiagnosticsViewController alloc] initWithBridge:_bridge];
    [self.navigationController pushViewController:vc animated:YES];
}

- (void)openCalibration {
    CalibrationViewController *vc = [[CalibrationViewController alloc] initWithBridge:_bridge];
    [self.navigationController pushViewController:vc animated:YES];
}

- (void)onUiRefreshTick {
    auto cloud = [_bridge latestGlobalPointCloud];
    auto traj = [_bridge currentTrajectory];
    [_renderer updatePointCloud:cloud trajectory:traj];

    auto plan = [_bridge latestFloorPlan];
    [_floorPlanView updateWithFloorPlan:plan];

    auto pkt = [_bridge currentPacketStats];
    auto qual = [_bridge latestQualityReport];
    size_t loops = [_bridge loopClosureCount];

    _hudStatusBanner.text = [NSString stringWithFormat:
        @"  PROJECT: %@ | Loops: %zu | Pts: %zu\n  MSOP: %llu | IMU: %llu | Loss: %.2f%%\n  %s",
        _bridge.activeProjectId ?: @"PROJECT_001",
        loops,
        cloud.size(),
        pkt.msop_packets_valid,
        pkt.imu_packets_valid,
        pkt.msopPacketLossRatio() * 100.0,
        qual.operator_guidance_message.c_str()];

    _hudMetricsLabel.text = [NSString stringWithFormat:
        @"  Quality: %.0f%% | Overlap: %.0f%% | RMSE: %.3f m | Drift Risk: %.2f m",
        qual.overall_quality_score * 100.0,
        qual.scan_overlap_ratio * 100.0,
        qual.registration_rmse_m,
        qual.estimated_drift_risk_m];
}

@end
