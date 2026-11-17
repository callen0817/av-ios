#import "MainScannerViewController.h"
#import "DiagnosticsViewController.h"
#import "CalibrationViewController.h"

@implementation MainScannerViewController {
    AVScanCoreBridge *_bridge;
    MTKView *_mtkView;
    MetalPointCloudRenderer *_renderer;
    FloorPlanView *_floorPlanView;

    UIVisualEffectView *_hudStatusGlassCard;
    UIVisualEffectView *_hudMetricsGlassCard;
    UIVisualEffectView *_calibPopupCard;
    UILabel *_hudStatusBanner;
    UILabel *_hudMetricsLabel;
    UILabel *_calibPopupLabel;

    UIButton *_startStopBtn;
    UIButton *_saveBtn;
    UIButton *_exportBtn;
    UIButton *_poiPhotoBtn;
    UISegmentedControl *_modeSegmentControl; // [SLAM Mobile | Terrestrial]
    UISegmentedControl *_colorModeControl;
    UIBarButtonItem *_glassOpacityItem;
    UIBarButtonItem *_deviceProfileItem;

    NSTimer *_uiTimer;
    CGPoint _lastPanPt;
    NSInteger _glassOpacityPct; // 0, 12, 25, 50, 80
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"AV Scan — FAST-LIVO2";
    self.view.backgroundColor = [UIColor blackColor];
    _glassOpacityPct = 12; // Default 12% matching scanarMini / AppWindow.qml

    [self setupNavigationBarAppearance];

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

    // Show Apple Glass Splash Screen on startup
    dispatch_async(dispatch_get_main_queue(), ^{
        [self showSplashScreen];
    });
}

- (void)setupNavigationBarAppearance {
    if (@available(iOS 13.0, *)) {
        UINavigationBarAppearance *appearance = [[UINavigationBarAppearance alloc] init];
        [appearance configureWithDefaultBackground];
        appearance.backgroundEffect = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark];
        appearance.backgroundColor = [UIColor colorWithRed:18.0/255.0 green:18.0/255.0 blue:24.0/255.0 alpha:0.75];
        appearance.titleTextAttributes = @{
            NSForegroundColorAttributeName: [AppleGlassTheme whiteWarm],
            NSFontAttributeName: [UIFont systemFontOfSize:17.0 weight:UIFontWeightSemibold]
        };
        self.navigationController.navigationBar.standardAppearance = appearance;
        self.navigationController.navigationBar.scrollEdgeAppearance = appearance;
        self.navigationController.navigationBar.compactAppearance = appearance;
    }
    self.navigationController.navigationBar.tintColor = [AppleGlassTheme goldBright];
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
    // 1. Left Bar Button: 36x36 Circular Artificial Vision Emblem Badge
    UIButton *badgeBtn = [UIButton buttonWithType:UIButtonTypeCustom];
    badgeBtn.frame = CGRectMake(0, 0, 36, 36);
    badgeBtn.layer.cornerRadius = 18.0;
    badgeBtn.layer.borderWidth = 1.5;
    badgeBtn.layer.borderColor = [AppleGlassTheme goldPrimary].CGColor;
    badgeBtn.clipsToBounds = YES;
    badgeBtn.backgroundColor = [UIColor colorWithRed:18.0/255.0 green:18.0/255.0 blue:24.0/255.0 alpha:0.85];

    UIImage *logoImg = [AppleGlassTheme loadBrandLogo];
    if (logoImg) {
        [badgeBtn setImage:logoImg forState:UIControlStateNormal];
        badgeBtn.imageView.contentMode = UIViewContentModeScaleAspectFit;
        badgeBtn.imageEdgeInsets = UIEdgeInsetsMake(4, 4, 4, 4);
    }
    [badgeBtn addTarget:self action:@selector(showSplashScreen) forControlEvents:UIControlEventTouchUpInside];
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithCustomView:badgeBtn];

    // 2. Right Bar Buttons: Glass Opacity Pill + Device Profile + Airy Diagnostics + Calibration
    _glassOpacityItem = [[UIBarButtonItem alloc] initWithTitle:[NSString stringWithFormat:@"Glass %ld%%", (long)_glassOpacityPct]
                                                         style:UIBarButtonItemStylePlain
                                                        target:self
                                                        action:@selector(onCycleGlassOpacity)];

    _deviceProfileItem = [[UIBarButtonItem alloc] initWithTitle:@"iPad 10 ▾"
                                                          style:UIBarButtonItemStylePlain
                                                         target:self
                                                         action:@selector(openDeviceProfilePicker)];
    [self updateDeviceProfileButtonTitle];

    UIBarButtonItem *diagItem = [[UIBarButtonItem alloc] initWithTitle:@"Airy Net"
                                                                 style:UIBarButtonItemStylePlain
                                                                target:self
                                                                action:@selector(openDiagnostics)];
    UIBarButtonItem *calibItem = [[UIBarButtonItem alloc] initWithTitle:@"Calibrate"
                                                                  style:UIBarButtonItemStylePlain
                                                                 target:self
                                                                action:@selector(openCalibration)];
    self.navigationItem.rightBarButtonItems = @[calibItem, diagItem, _deviceProfileItem, _glassOpacityItem];

    // 3. Apple Glass Status HUD Card (Frosted Glass Card)
    CGFloat topOffset = 96.0;
    _hudStatusGlassCard = [AppleGlassTheme createGlassCardWithCornerRadius:12.0];
    _hudStatusGlassCard.frame = CGRectMake(16, topOffset, 380, 68);
    [self.view addSubview:_hudStatusGlassCard];

    _hudStatusBanner = [[UILabel alloc] initWithFrame:CGRectInset(_hudStatusGlassCard.contentView.bounds, 10, 6)];
    _hudStatusBanner.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _hudStatusBanner.backgroundColor = [UIColor clearColor];
    _hudStatusBanner.textColor = [AppleGlassTheme whiteWarm];
    _hudStatusBanner.font = [UIFont monospacedSystemFontOfSize:11.5 weight:UIFontWeightMedium];
    _hudStatusBanner.numberOfLines = 3;
    [_hudStatusGlassCard.contentView addSubview:_hudStatusBanner];

    // 4. Apple Glass Real-Time Metrics HUD Card
    _hudMetricsGlassCard = [AppleGlassTheme createGlassCardWithCornerRadius:10.0];
    _hudMetricsGlassCard.frame = CGRectMake(16, topOffset + 76, 380, 48);
    [self.view addSubview:_hudMetricsGlassCard];

    _hudMetricsLabel = [[UILabel alloc] initWithFrame:CGRectInset(_hudMetricsGlassCard.contentView.bounds, 10, 4)];
    _hudMetricsLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _hudMetricsLabel.backgroundColor = [UIColor clearColor];
    _hudMetricsLabel.textColor = [AppleGlassTheme goldBright];
    _hudMetricsLabel.font = [UIFont monospacedSystemFontOfSize:11.0 weight:UIFontWeightRegular];
    _hudMetricsLabel.numberOfLines = 2;
    [_hudMetricsGlassCard.contentView addSubview:_hudMetricsLabel];

    // 4b. Calibration Lock Status Popup Banner (White / Red / Yellow / Green)
    _calibPopupCard = [AppleGlassTheme createGlassCardWithCornerRadius:10.0];
    _calibPopupCard.frame = CGRectMake(16, topOffset + 132, 380, 52);
    _calibPopupCard.hidden = YES;
    [self.view addSubview:_calibPopupCard];

    _calibPopupLabel = [[UILabel alloc] initWithFrame:CGRectInset(_calibPopupCard.contentView.bounds, 10, 4)];
    _calibPopupLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _calibPopupLabel.backgroundColor = [UIColor clearColor];
    _calibPopupLabel.textColor = [UIColor whiteColor];
    _calibPopupLabel.font = [UIFont monospacedSystemFontOfSize:11.5 weight:UIFontWeightBold];
    _calibPopupLabel.numberOfLines = 2;
    [_calibPopupCard.contentView addSubview:_calibPopupLabel];

    // 5. 2D Floor Plan Widget (Floating Apple Glass Radar Card)
    CGFloat radarSize = 220.0;
    _floorPlanView = [[FloorPlanView alloc] initWithFrame:CGRectMake(self.view.bounds.size.width - radarSize - 16,
                                                                     topOffset,
                                                                     radarSize,
                                                                     radarSize)];
    _floorPlanView.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin;
    [self.view addSubview:_floorPlanView];

    // 6. Mode & Point Cloud Shading Segmented Controls
    _modeSegmentControl = [[UISegmentedControl alloc] initWithItems:@[@"Mobile SLAM", @"Terrestrial Station"]];
    _modeSegmentControl.selectedSegmentIndex = 0;
    _modeSegmentControl.frame = CGRectMake(16, self.view.bounds.size.height - 180, 280, 32);
    _modeSegmentControl.autoresizingMask = UIViewAutoresizingFlexibleTopMargin;
    if (@available(iOS 13.0, *)) {
        _modeSegmentControl.selectedSegmentTintColor = [AppleGlassTheme goldPrimary];
        [_modeSegmentControl setTitleTextAttributes:@{NSForegroundColorAttributeName: [AppleGlassTheme whiteWarm]} forState:UIControlStateNormal];
        [_modeSegmentControl setTitleTextAttributes:@{NSForegroundColorAttributeName: [UIColor blackColor], NSFontAttributeName: [UIFont systemFontOfSize:11.5 weight:UIFontWeightBold]} forState:UIControlStateSelected];
    }
    [_modeSegmentControl addTarget:self action:@selector(onAcquisitionModeChanged:) forControlEvents:UIControlEventValueChanged];
    [self.view addSubview:_modeSegmentControl];

    _colorModeControl = [[UISegmentedControl alloc] initWithItems:@[@"RGB Sensor", @"Reflectivity", @"Elevation Z"]];
    _colorModeControl.selectedSegmentIndex = 0;
    _colorModeControl.frame = CGRectMake(16, self.view.bounds.size.height - 138, 300, 36);
    _colorModeControl.autoresizingMask = UIViewAutoresizingFlexibleTopMargin;
    if (@available(iOS 13.0, *)) {
        _colorModeControl.selectedSegmentTintColor = [AppleGlassTheme goldPrimary];
        [_colorModeControl setTitleTextAttributes:@{NSForegroundColorAttributeName: [AppleGlassTheme whiteWarm]} forState:UIControlStateNormal];
        [_colorModeControl setTitleTextAttributes:@{NSForegroundColorAttributeName: [UIColor blackColor], NSFontAttributeName: [UIFont systemFontOfSize:12.0 weight:UIFontWeightBold]} forState:UIControlStateSelected];
    }
    [_colorModeControl addTarget:self action:@selector(onColorModeChanged:) forControlEvents:UIControlEventValueChanged];
    [self.view addSubview:_colorModeControl];

    // 7. Frosted Apple Glass Action Buttons Bar
    UIStackView *bottomBar = [[UIStackView alloc] initWithFrame:CGRectMake(16, self.view.bounds.size.height - 84, self.view.bounds.size.width - 32, 52)];
    bottomBar.axis = UILayoutConstraintAxisHorizontal;
    bottomBar.distribution = UIStackViewDistributionFillEqually;
    bottomBar.spacing = 10.0;
    bottomBar.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin;
    [self.view addSubview:bottomBar];

    _startStopBtn = [AppleGlassTheme createGlassButtonWithTitle:@"Start Scan"
                                                      baseColor:[AppleGlassTheme statusOk]
                                                   cornerRadius:12.0];
    [_startStopBtn addTarget:self action:@selector(onStartStopTapped) forControlEvents:UIControlEventTouchUpInside];

    _poiPhotoBtn = [AppleGlassTheme createGlassButtonWithTitle:@"POI Photo"
                                                     baseColor:[AppleGlassTheme statusCyan]
                                                  cornerRadius:12.0];
    [_poiPhotoBtn addTarget:self action:@selector(onPoiPhotoTapped) forControlEvents:UIControlEventTouchUpInside];

    _saveBtn = [AppleGlassTheme createGlassButtonWithTitle:@"Save & Refine"
                                                 baseColor:[AppleGlassTheme goldPrimary]
                                              cornerRadius:12.0];
    [_saveBtn addTarget:self action:@selector(onSaveTapped) forControlEvents:UIControlEventTouchUpInside];

    _exportBtn = [AppleGlassTheme createGlassButtonWithTitle:@"Export PLY/E57"
                                                   baseColor:[UIColor colorWithRed:139.0/255.0 green:92.0/255.0 blue:246.0/255.0 alpha:1.0]
                                                cornerRadius:12.0];
    [_exportBtn addTarget:self action:@selector(onExportTapped) forControlEvents:UIControlEventTouchUpInside];

    [bottomBar addArrangedSubview:_startStopBtn];
    [bottomBar addArrangedSubview:_poiPhotoBtn];
    [bottomBar addArrangedSubview:_saveBtn];
    [bottomBar addArrangedSubview:_exportBtn];
}

- (void)onCycleGlassOpacity {
    if (_glassOpacityPct == 0) _glassOpacityPct = 12;
    else if (_glassOpacityPct == 12) _glassOpacityPct = 25;
    else if (_glassOpacityPct == 25) _glassOpacityPct = 50;
    else if (_glassOpacityPct == 50) _glassOpacityPct = 80;
    else _glassOpacityPct = 0;

    _glassOpacityItem.title = [NSString stringWithFormat:@"Glass %ld%%", (long)_glassOpacityPct];

    CGFloat alpha = (_glassOpacityPct == 0) ? 0.0 : (0.20 + (_glassOpacityPct / 100.0) * 0.60);
    _hudStatusGlassCard.alpha = MAX(0.20, alpha);
    _hudMetricsGlassCard.alpha = MAX(0.20, alpha);
    _floorPlanView.alpha = MAX(0.25, alpha);
}

- (void)showSplashScreen {
    // Avoid double presentation
    for (UIView *sub in self.view.subviews) {
        if ([sub isKindOfClass:[AppleGlassSplashScreenView class]]) {
            return;
        }
    }

    AppleGlassSplashScreenView *splash = [[AppleGlassSplashScreenView alloc] initWithFrame:self.view.bounds];
    [self.view addSubview:splash];
}

- (void)onColorModeChanged:(UISegmentedControl *)seg {
    _renderer.colorMode = static_cast<int>(seg.selectedSegmentIndex);
}

- (void)onAcquisitionModeChanged:(UISegmentedControl *)seg {
    if (seg.selectedSegmentIndex == 0) {
        // Mobile SLAM
        _bridge.acquisitionMode = AVScanAcquisitionModeMobileSlam;
        [_startStopBtn setTitle:@"Start Scan" forState:UIControlStateNormal];
        [_poiPhotoBtn setTitle:@"POI Photo" forState:UIControlStateNormal];
        [_saveBtn setTitle:@"Save & Refine" forState:UIControlStateNormal];
    } else {
        // Terrestrial Station Mode - Check Motor Connection Confirmation
        [self promptMotorConnectionVerificationWithCompletion:^(BOOL confirmed) {
            if (confirmed) {
                self->_bridge.acquisitionMode = AVScanAcquisitionModeTerrestrial;
                [self->_startStopBtn setTitle:@"New Station" forState:UIControlStateNormal];
                [self->_poiPhotoBtn setTitle:@"Station 60°" forState:UIControlStateNormal];
                [self->_saveBtn setTitle:@"Finish Station" forState:UIControlStateNormal];
            } else {
                seg.selectedSegmentIndex = 0;
            }
        }];
    }
}

- (void)promptMotorConnectionVerificationWithCompletion:(void(^)(BOOL confirmed))completion {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Motorized Head Confirmation"
                                                                   message:@"Confirm turntable / motorized tripod head attachment:\n\nAuto-verification is optional and does not block manual indexing on Jetson or iPad."
                                                            preferredStyle:UIAlertControllerStyleAlert];

    [alert addAction:[UIAlertAction actionWithTitle:@"Confirm Manual / Detent Head Attached"
                                             style:UIAlertActionStyleDefault
                                           handler:^(UIAlertAction *action) {
        [self->_bridge confirmMotorAttachedWithAutomatic:NO model:@"Manual Detent Turntable"];
        if (completion) completion(YES);
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"Connect Auto Bluetooth / Serial Motor"
                                             style:UIAlertActionStyleDefault
                                           handler:^(UIAlertAction *action) {
        [self->_bridge confirmMotorAttachedWithAutomatic:YES model:@"Syrp / ZEAPON Motor"];
        if (completion) completion(YES);
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                             style:UIAlertActionStyleCancel
                                           handler:^(UIAlertAction *action) {
        if (completion) completion(NO);
    }]];

    [self presentViewController:alert animated:YES completion:nil];
}

- (void)onStartStopTapped {
    if (_bridge.acquisitionMode == AVScanAcquisitionModeTerrestrial) {
        if (!_bridge.isTerrestrialStationActive) {
            [_bridge startTerrestrialStationWithId:@"st_001" name:@"Station 01" stepDeg:60.0 isFlashDuringSlam:NO];
            [_startStopBtn setTitle:@"Next Stop 60°" forState:UIControlStateNormal];
            _startStopBtn.backgroundColor = [[AppleGlassTheme statusCyan] colorWithAlphaComponent:0.40];
            _startStopBtn.layer.borderColor = [AppleGlassTheme statusCyan].CGColor;
        } else {
            [_bridge advanceToNextTerrestrialStop];
            uint32_t stopIdx = [_bridge currentTerrestrialStopIndex];
            [_startStopBtn setTitle:[NSString stringWithFormat:@"Stop %u (60°)", stopIdx + 1] forState:UIControlStateNormal];
        }
        return;
    }

    // Default Mobile SLAM Mode (Mode A - Preserved)
    if (_bridge.isScanning) {
        [_bridge stopScanningSession];
        [_startStopBtn setTitle:@"Start Scan" forState:UIControlStateNormal];
        [_poiPhotoBtn setTitle:@"POI Photo" forState:UIControlStateNormal];
        _startStopBtn.backgroundColor = [[AppleGlassTheme statusOk] colorWithAlphaComponent:0.35];
        _startStopBtn.layer.borderColor = [AppleGlassTheme statusOk].CGColor;
    } else {
        [_bridge startScanningSessionWithAutoCalib:YES];
        [_startStopBtn setTitle:@"Stop Scan" forState:UIControlStateNormal];
        [_poiPhotoBtn setTitle:@"Flash 60°" forState:UIControlStateNormal]; // Dynamic on-the-fly flash terrestrial button during SLAM!
        _startStopBtn.backgroundColor = [[AppleGlassTheme statusErr] colorWithAlphaComponent:0.40];
        _startStopBtn.layer.borderColor = [AppleGlassTheme statusErr].CGColor;
    }
}

- (void)onPoiPhotoTapped {
    if (_bridge.isScanning && !_bridge.isTerrestrialStationActive) {
        // Dynamic Hybrid Flash Station during active SLAM!
        [self promptMotorConnectionVerificationWithCompletion:^(BOOL confirmed) {
            if (confirmed) {
                NSString *stId = [NSString stringWithFormat:@"flash_st_%03u", arc4random_uniform(900) + 100];
                [self->_bridge startTerrestrialStationWithId:stId name:@"Hybrid Flash Station" stepDeg:60.0 isFlashDuringSlam:YES];
                [self->_poiPhotoBtn setTitle:@"Step 60°" forState:UIControlStateNormal];
            }
        }];
        return;
    } else if (_bridge.isTerrestrialStationActive && _bridge.isSlamPausedForStation) {
        // Step active flash station
        [_bridge advanceToNextTerrestrialStop];
        uint32_t stopIdx = [_bridge currentTerrestrialStopIndex];
        if (stopIdx >= 5) {
            [_bridge finalizeAndSaveTerrestrialStation];
            [_poiPhotoBtn setTitle:@"Flash 60°" forState:UIControlStateNormal];
        } else {
            [_poiPhotoBtn setTitle:[NSString stringWithFormat:@"Step %u/6", stopIdx + 1] forState:UIControlStateNormal];
        }
        return;
    }

    [_bridge capturePointOfInterestPhotoWithNote:@"Operator POI inspection snapshot"];
}

- (void)onSaveTapped {
    if (_bridge.acquisitionMode == AVScanAcquisitionModeTerrestrial) {
        [_bridge finalizeAndSaveTerrestrialStation];
        [_startStopBtn setTitle:@"New Station" forState:UIControlStateNormal];
        _startStopBtn.backgroundColor = [[AppleGlassTheme statusOk] colorWithAlphaComponent:0.35];
        _startStopBtn.layer.borderColor = [AppleGlassTheme statusOk].CGColor;
        return;
    }

    [_bridge saveAndRefineSession];
    [_startStopBtn setTitle:@"Start Scan" forState:UIControlStateNormal];
    _startStopBtn.backgroundColor = [[AppleGlassTheme statusOk] colorWithAlphaComponent:0.35];
    _startStopBtn.layer.borderColor = [AppleGlassTheme statusOk].CGColor;
}

- (void)updateDeviceProfileButtonTitle {
    NSString *name = [_bridge activeDeviceProfileDisplayName];
    if ([name containsString:@"iPad 10th Gen"]) {
        _deviceProfileItem.title = @"iPad 10 ▾";
    } else if ([name containsString:@"iPad Air 4"]) {
        _deviceProfileItem.title = @"Air 4 ▾";
    } else if ([name containsString:@"iPad Air 5"]) {
        _deviceProfileItem.title = @"Air 5 ▾";
    } else if ([name containsString:@"Air 11"]) {
        _deviceProfileItem.title = @"Air 11 M2 ▾";
    } else if ([name containsString:@"Pro 11"]) {
        _deviceProfileItem.title = @"Pro 11 M4 ▾";
    } else if ([name containsString:@"iPhone 15 Pro"]) {
        _deviceProfileItem.title = @"15 Pro ▾";
    } else if ([name containsString:@"iPhone 16 Pro"]) {
        _deviceProfileItem.title = @"16 Pro ▾";
    } else {
        _deviceProfileItem.title = @"Rig Profile ▾";
    }
}

- (void)openDeviceProfilePicker {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Device Rig & Mount Profile"
                                                                   message:@"Select host iPad / iPhone model with USB-C LiDAR clamp configuration:"
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    alert.popoverPresentationController.barButtonItem = _deviceProfileItem;

    NSArray<NSDictionary<NSString *, id> *> *profiles = [_bridge availableDeviceProfiles];
    NSString *currentId = [_bridge activeDeviceProfileId];

    MainScannerViewController * __weak weakSelf = self;
    for (NSDictionary *p in profiles) {
        NSString *pId = p[@"id"];
        NSString *pName = p[@"name"];
        BOOL isCurrent = [pId isEqualToString:currentId];
        NSString *title = isCurrent ? [NSString stringWithFormat:@"✓ %@", pName] : pName;

        [alert addAction:[UIAlertAction actionWithTitle:title
                                                 style:UIAlertActionStyleDefault
                                               handler:^(UIAlertAction *action) {
            MainScannerViewController *strongSelf = weakSelf;
            if (!strongSelf) return;
            [strongSelf->_bridge setActiveDeviceProfileWithId:pId];
            [strongSelf updateDeviceProfileButtonTitle];
        }]];
    }

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)onExportTapped {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Export AV Ecosystem Package"
                                                                   message:@"Choose output format compatible with AV Showcase, AV Structure, and Cloud Ingestion:"
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    alert.popoverPresentationController.sourceView = _exportBtn;

    MainScannerViewController * __weak weakSelf = self;

    [alert addAction:[UIAlertAction actionWithTitle:@"AV Spatial Bundle (Showcase & Structure v1.1)"
                                             style:UIAlertActionStyleDefault
                                           handler:^(UIAlertAction *action) {
        [weakSelf performExportBundleOnly];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"Complete Ecosystem Package (All Formats)"
                                             style:UIAlertActionStyleDefault
                                           handler:^(UIAlertAction *action) {
        [weakSelf performExportAllEcosystemFormats];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"Universal Point Cloud (.PLY)"
                                             style:UIAlertActionStyleDefault
                                           handler:^(UIAlertAction *action) {
        [weakSelf performExportSingleFormat:@"ply"];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"Structured Point Cloud (.E57)"
                                             style:UIAlertActionStyleDefault
                                           handler:^(UIAlertAction *action) {
        [weakSelf performExportSingleFormat:@"e57"];
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)performExportBundleOnly {
    NSString *projDir = _bridge.activeProjectDir;
    if (!projDir) return;

    NSError *err = nil;
    BOOL ok = [_bridge exportCurrentMapToSpatialBundle:projDir error:&err];
    if (ok) {
        NSString *bundlePath = [projDir stringByAppendingPathComponent:@"av_walkthrough_bundle.json"];
        NSString *lod0Path = [projDir stringByAppendingPathComponent:@"cache/lod0.bin"];
        NSString *keyplanPath = [projDir stringByAppendingPathComponent:@"floors/floor_0_keyplan.png"];

        NSMutableArray<NSURL *> *items = [NSMutableArray array];
        if ([[NSFileManager defaultManager] fileExistsAtPath:bundlePath]) {
            [items addObject:[NSURL fileURLWithPath:bundlePath]];
        }
        if ([[NSFileManager defaultManager] fileExistsAtPath:lod0Path]) {
            [items addObject:[NSURL fileURLWithPath:lod0Path]];
        }
        if ([[NSFileManager defaultManager] fileExistsAtPath:keyplanPath]) {
            [items addObject:[NSURL fileURLWithPath:keyplanPath]];
        }

        UIActivityViewController *shareVC = [[UIActivityViewController alloc] initWithActivityItems:items applicationActivities:nil];
        shareVC.popoverPresentationController.sourceView = _exportBtn;
        [self presentViewController:shareVC animated:YES completion:nil];
    }
}

- (void)performExportAllEcosystemFormats {
    NSString *projDir = _bridge.activeProjectDir;
    if (!projDir) return;

    // 1. Export Bundle & LOD0
    NSError *err = nil;
    [_bridge exportCurrentMapToSpatialBundle:projDir error:&err];

    // 2. Export standard point clouds
    NSString *plyPath = [projDir stringByAppendingPathComponent:@"avscan_map.ply"];
    NSString *pcdPath = [projDir stringByAppendingPathComponent:@"avscan_map.pcd"];
    NSString *e57Path = [projDir stringByAppendingPathComponent:@"avscan_map.e57"];
    NSString *bundlePath = [projDir stringByAppendingPathComponent:@"av_walkthrough_bundle.json"];
    NSString *lod0Path = [projDir stringByAppendingPathComponent:@"cache/lod0.bin"];
    NSString *keyplanPath = [projDir stringByAppendingPathComponent:@"floors/floor_0_keyplan.png"];

    [_bridge exportCurrentMapToPly:plyPath];
    [_bridge exportCurrentMapToPcd:pcdPath];
    [_bridge exportCurrentMapToE57:e57Path];

    NSMutableArray<NSURL *> *items = [NSMutableArray array];
    NSArray<NSString *> *allPaths = @[bundlePath, lod0Path, keyplanPath, plyPath, e57Path, pcdPath];
    for (NSString *p in allPaths) {
        if ([[NSFileManager defaultManager] fileExistsAtPath:p]) {
            [items addObject:[NSURL fileURLWithPath:p]];
        }
    }

    if (items.count > 0) {
        UIActivityViewController *shareVC = [[UIActivityViewController alloc] initWithActivityItems:items applicationActivities:nil];
        shareVC.popoverPresentationController.sourceView = _exportBtn;
        [self presentViewController:shareVC animated:YES completion:nil];
    }
}

- (void)performExportSingleFormat:(NSString *)ext {
    NSString *projDir = _bridge.activeProjectDir;
    if (!projDir) return;

    NSString *filePath = [projDir stringByAppendingPathComponent:[NSString stringWithFormat:@"avscan_map.%@", ext]];
    BOOL ok = NO;
    if ([ext isEqualToString:@"ply"]) ok = [_bridge exportCurrentMapToPly:filePath];
    else if ([ext isEqualToString:@"pcd"]) ok = [_bridge exportCurrentMapToPcd:filePath];
    else if ([ext isEqualToString:@"e57"]) ok = [_bridge exportCurrentMapToE57:filePath];

    if (ok && [[NSFileManager defaultManager] fileExistsAtPath:filePath]) {
        UIActivityViewController *shareVC = [[UIActivityViewController alloc] initWithActivityItems:@[[NSURL fileURLWithPath:filePath]] applicationActivities:nil];
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
    auto autoCalib = [_bridge currentAutoCalibStatus];
    size_t loops = [_bridge loopClosureCount];

    _hudStatusBanner.text = [NSString stringWithFormat:
        @"RIG: %@  ·  %@\nLOOPS: %zu  ·  PTS: %zu  ·  LOSS: %.2f%%\n%s",
        [_bridge activeDeviceProfileDisplayName],
        _bridge.activeProjectId ?: @"PROJECT_001",
        loops,
        cloud.size(),
        pkt.msopPacketLossRatio() * 100.0,
        qual.operator_guidance_message.c_str()];

    if (_bridge.isScanning && autoCalib.popup_color != avscan::calibration::AutoCalibPopupColor::NONE) {
        _calibPopupCard.hidden = NO;
        UIColor *popupColor = [UIColor whiteColor];
        switch (autoCalib.popup_color) {
            case avscan::calibration::AutoCalibPopupColor::RED:
                popupColor = [AppleGlassTheme statusErr];
                break;
            case avscan::calibration::AutoCalibPopupColor::YELLOW:
                popupColor = [AppleGlassTheme statusWarn];
                break;
            case avscan::calibration::AutoCalibPopupColor::GREEN:
                popupColor = [AppleGlassTheme statusOk];
                break;
            case avscan::calibration::AutoCalibPopupColor::WHITE:
            default:
                popupColor = [UIColor whiteColor];
                break;
        }
        _calibPopupLabel.textColor = popupColor;
        _calibPopupCard.layer.borderColor = [popupColor colorWithAlphaComponent:0.75].CGColor;
        _calibPopupCard.layer.borderWidth = 1.25;
        _calibPopupLabel.text = [NSString stringWithFormat:
            @"%s\n%s",
            autoCalib.popup_title.c_str(),
            autoCalib.status_message.c_str()];
    } else {
        _calibPopupCard.hidden = YES;
    }

    _hudMetricsLabel.text = [NSString stringWithFormat:
        @"QUALITY: %.0f%%  ·  OVERLAP: %.0f%%\nRMSE: %.3f m  ·  DRIFT RISK: %.2f m",
        qual.overall_quality_score * 100.0,
        qual.scan_overlap_ratio * 100.0,
        qual.registration_rmse_m,
        qual.estimated_drift_risk_m];
}

@end
