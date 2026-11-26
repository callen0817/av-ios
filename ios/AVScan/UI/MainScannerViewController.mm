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
    UIBarButtonItem *_projectPickerItem;
    UIBarButtonItem *_cslamConnectItem;

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
    UIBarButtonItem *badgeItem = [[UIBarButtonItem alloc] initWithCustomView:badgeBtn];
    _projectPickerItem = [[UIBarButtonItem alloc] initWithTitle:@"Projects ▾"
                                                          style:UIBarButtonItemStylePlain
                                                         target:self
                                                         action:@selector(openProjectScanMenu)];
    _cslamConnectItem = [[UIBarButtonItem alloc] initWithTitle:@"Connect"
                                                         style:UIBarButtonItemStylePlain
                                                        target:self
                                                        action:@selector(openCslamConnectMenu)];
    self.navigationItem.leftBarButtonItems = @[badgeItem, _projectPickerItem, _cslamConnectItem];
    [self updateProjectButtonTitle];

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

    [self applyGlassOpacityToCards];
}

- (void)applyGlassOpacityToCards {
    UIBlurEffect *blur = nil;
    UIColor *tint = [UIColor clearColor];
    CGFloat borderAlpha = 0.25;
    UIColor *radarBg = [UIColor colorWithRed:14.0/255.0 green:14.0/255.0 blue:18.0/255.0 alpha:0.12];

    if (_glassOpacityPct == 0) {
        blur = nil;
        tint = [UIColor clearColor];
        borderAlpha = 0.28;
        radarBg = [UIColor colorWithRed:14.0/255.0 green:14.0/255.0 blue:18.0/255.0 alpha:0.10];
    } else if (_glassOpacityPct <= 12) {
        if (@available(iOS 13.0, *)) blur = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark];
        tint = [AppleGlassTheme glassDarkWithOpacity:12.0];
        borderAlpha = 0.45;
        radarBg = [AppleGlassTheme glassDarkWithOpacity:12.0];
    } else if (_glassOpacityPct <= 25) {
        if (@available(iOS 13.0, *)) blur = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterialDark];
        tint = [AppleGlassTheme glassDarkWithOpacity:25.0];
        borderAlpha = 0.60;
        radarBg = [AppleGlassTheme glassDarkWithOpacity:25.0];
    } else if (_glassOpacityPct <= 50) {
        if (@available(iOS 13.0, *)) blur = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterialDark];
        tint = [AppleGlassTheme glassDarkWithOpacity:50.0];
        borderAlpha = 0.78;
        radarBg = [AppleGlassTheme glassDarkWithOpacity:50.0];
    } else {
        if (@available(iOS 13.0, *)) blur = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThickMaterialDark];
        tint = [AppleGlassTheme glassDarkWithOpacity:80.0];
        borderAlpha = 0.95;
        radarBg = [AppleGlassTheme glassDarkWithOpacity:80.0];
    }

    NSArray<UIVisualEffectView *> *cards = @[_hudStatusGlassCard, _hudMetricsGlassCard, _calibPopupCard];
    UIColor *goldBorder = [[AppleGlassTheme goldPrimary] colorWithAlphaComponent:borderAlpha];
    for (UIVisualEffectView *card in cards) {
        if (!card) continue;
        card.alpha = 1.0; // Never lower alpha on UIVisualEffectView; adjust blur + tint instead
        card.effect = blur;
        card.contentView.backgroundColor = tint;
        if (card != _calibPopupCard) {
            card.layer.borderColor = goldBorder.CGColor;
        }
    }

    if (_floorPlanView) {
        _floorPlanView.alpha = 1.0;
        _floorPlanView.backgroundColor = radarBg;
        _floorPlanView.layer.borderColor = goldBorder.CGColor;
        [_floorPlanView setNeedsDisplay];
    }
}

- (void)onCycleGlassOpacity {
    if (_glassOpacityPct == 0) _glassOpacityPct = 12;
    else if (_glassOpacityPct == 12) _glassOpacityPct = 25;
    else if (_glassOpacityPct == 25) _glassOpacityPct = 50;
    else if (_glassOpacityPct == 50) _glassOpacityPct = 80;
    else _glassOpacityPct = 0;

    _glassOpacityItem.title = [NSString stringWithFormat:@"Glass %ld%%", (long)_glassOpacityPct];
    [self applyGlassOpacityToCards];
}

#pragma mark - Project & Scan Creation Structure (AV Scan / AV Scanar GUI)

- (void)updateProjectButtonTitle {
    NSString *proj = _bridge.activeProjectName.length > 0 ? _bridge.activeProjectName : @"Project";
    if (proj.length > 14) {
        proj = [[proj substringToIndex:13] stringByAppendingString:@"…"];
    }
    _projectPickerItem.title = [NSString stringWithFormat:@"%@ ▾", proj];
}

- (void)openProjectScanMenu {
    NSString *projName = _bridge.activeProjectName ?: @"Default Project";
    NSString *scanName = _bridge.activeScanName ?: @"Scan 01";
    NSString *floorLvl = _bridge.activeScanFloorLevel ?: @"Level 01";
    NSArray *projects = [_bridge listProjects];
    NSArray *scans = [_bridge listScansInActiveProject];

    NSString *msg = [NSString stringWithFormat:@"Project: %@\nActive Scan: %@ (%@)", projName, scanName, floorLvl];
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Project & Scan Manager"
                                                                   message:msg
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    sheet.popoverPresentationController.barButtonItem = _projectPickerItem;

    MainScannerViewController * __weak weakSelf = self;

    [sheet addAction:[UIAlertAction actionWithTitle:@"+ New Project..."
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        [weakSelf promptCreateNewProject];
    }]];

    [sheet addAction:[UIAlertAction actionWithTitle:@"+ New Scan in Current Project..."
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        [weakSelf promptCreateNewScan];
    }]];

    [sheet addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Switch Scan (%lu in project)...", (unsigned long)scans.count]
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        [weakSelf openScanSwitcher];
    }]];

    [sheet addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Switch Project (%lu total)...", (unsigned long)projects.count]
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        [weakSelf openProjectSwitcher];
    }]];

    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)promptCreateNewProject {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Create New Project"
                                                                   message:@"Enter project details (AV Scan / AV Scanar hierarchy):"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.placeholder = @"Project Name (e.g. Warehouse_North)";
    }];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.placeholder = @"Site / Client / Building (optional)";
    }];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.placeholder = @"Notes (optional)";
    }];

    MainScannerViewController * __weak weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Create Project"
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        MainScannerViewController *strongSelf = weakSelf;
        if (!strongSelf) return;
        NSString *name = alert.textFields[0].text;
        NSString *site = alert.textFields[1].text;
        NSString *notes = alert.textFields[2].text;
        [strongSelf->_bridge createProjectWithName:name site:site notes:notes outProjectId:nil];
        [strongSelf updateProjectButtonTitle];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)promptCreateNewScan {
    NSUInteger nextNum = [_bridge listScansInActiveProject].count + 1;
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Create New Scan"
                                                                   message:[NSString stringWithFormat:@"Project: %@", _bridge.activeProjectName ?: @"Project"]
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.placeholder = @"Scan Name";
        tf.text = [NSString stringWithFormat:@"Scan %02lu", (unsigned long)nextNum];
    }];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.placeholder = @"Floor / Level (e.g. Level 01)";
        tf.text = self->_bridge.activeScanFloorLevel ?: @"Level 01";
    }];

    MainScannerViewController * __weak weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Create & Open Scan"
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        MainScannerViewController *strongSelf = weakSelf;
        if (!strongSelf) return;
        NSString *scanName = alert.textFields[0].text;
        NSString *floorLvl = alert.textFields[1].text;
        [strongSelf->_bridge createAndSelectScanWithName:scanName
                                              floorLevel:floorLvl
                                                    mode:strongSelf->_bridge.acquisitionMode
                                               outScanId:nil];
        [strongSelf updateProjectButtonTitle];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)openScanSwitcher {
    NSArray<NSDictionary<NSString *, id> *> *scans = [_bridge listScansInActiveProject];
    NSString *currentId = _bridge.activeScanId;
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Select Scan"
                                                                   message:_bridge.activeProjectName
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    sheet.popoverPresentationController.barButtonItem = _projectPickerItem;

    MainScannerViewController * __weak weakSelf = self;
    for (NSDictionary *s in scans) {
        NSString *sId = s[@"id"];
        BOOL isCur = [sId isEqualToString:currentId];
        NSString *title = [NSString stringWithFormat:@"%@%@ (%@ · %@)",
                           isCur ? @"✓ " : @"",
                           s[@"name"],
                           s[@"floorLevel"],
                           s[@"mode"]];
        [sheet addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(UIAlertAction *action) {
            MainScannerViewController *strongSelf = weakSelf;
            if (!strongSelf) return;
            [strongSelf->_bridge selectScanWithId:sId];
            [strongSelf updateProjectButtonTitle];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)openProjectSwitcher {
    NSArray<NSDictionary<NSString *, id> *> *projects = [_bridge listProjects];
    NSString *currentId = _bridge.activeProjectId;
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Select Project"
                                                                   message:@"Choose an existing scan project:"
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    sheet.popoverPresentationController.barButtonItem = _projectPickerItem;

    MainScannerViewController * __weak weakSelf = self;
    for (NSDictionary *p in projects) {
        NSString *pId = p[@"id"];
        BOOL isCur = [pId isEqualToString:currentId];
        NSString *site = [p[@"site"] length] > 0 ? [NSString stringWithFormat:@" · %@", p[@"site"]] : @"";
        NSString *title = [NSString stringWithFormat:@"%@%@%@ (%@ scans)",
                           isCur ? @"✓ " : @"",
                           p[@"name"],
                           site,
                           p[@"scanCount"]];
        [sheet addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(UIAlertAction *action) {
            MainScannerViewController *strongSelf = weakSelf;
            if (!strongSelf) return;
            [strongSelf->_bridge openProjectWithId:pId];
            [strongSelf updateProjectButtonTitle];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:sheet animated:YES completion:nil];
}

#pragma mark - Collaborative SLAM (C-SLAM) Device Connection UI

- (void)updateCslamButtonTitle {
    if (!_bridge.isCslamEnabled) {
        _cslamConnectItem.title = @"Connect";
        return;
    }
    auto stats = [_bridge collaborativeSlamStats];
    _cslamConnectItem.title = [NSString stringWithFormat:@"C-SLAM (%zu)", stats.discovered_peers];
}

- (void)openCslamConnectMenu {
    NSArray<NSDictionary<NSString *, id> *> *peers = [_bridge connectedCollaborativePeers];
    NSMutableString *msg = [NSMutableString string];
    if (_bridge.isCslamEnabled) {
        [msg appendFormat:@"C-SLAM ACTIVE (Bluetooth + Wi-Fi Ports 48765/48766)\nCompatible: AV Scan iOS · AV SLAM · AV Scanar · AV Scan\n"];
    } else {
        [msg appendString:@"C-SLAM OFF — Enable to discover nearby iPads/iPhones (Bluetooth + P2P Wi-Fi) and Jetson/Windows devices (LAN Wi-Fi).\n"];
    }
    if (peers.count > 0) {
        [msg appendString:@"\nDiscovered Devices:\n"];
        for (NSDictionary *p in peers) {
            BOOL aligned = [p[@"aligned"] boolValue];
            [msg appendFormat:@"• %@ [%@ · %@] — %@ (%@ KFs)\n",
                p[@"name"], p[@"platform"], p[@"app"],
                aligned ? @"ALIGNED" : @"SYNCING", p[@"keyframes"]];
        }
    } else if (_bridge.isCslamEnabled) {
        [msg appendString:@"\nSearching for peers over Bluetooth & Wi-Fi..."];
    }

    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Collaborative SLAM (C-SLAM)"
                                                                   message:msg
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    sheet.popoverPresentationController.barButtonItem = _cslamConnectItem;

    MainScannerViewController * __weak weakSelf = self;
    NSString *toggleTitle = _bridge.isCslamEnabled ? @"Disconnect C-SLAM" : @"Enable C-SLAM (Bluetooth + Wi-Fi)";
    [sheet addAction:[UIAlertAction actionWithTitle:toggleTitle
                                              style:(_bridge.isCslamEnabled ? UIAlertActionStyleDestructive : UIAlertActionStyleDefault)
                                            handler:^(UIAlertAction *action) {
        MainScannerViewController *strongSelf = weakSelf;
        if (!strongSelf) return;
        [strongSelf->_bridge setCollaborativeSlamEnabled:!strongSelf->_bridge.isCslamEnabled];
        [strongSelf updateCslamButtonTitle];
    }]];

    [sheet addAction:[UIAlertAction actionWithTitle:@"Connect Device by IP (Jetson / Windows / iOS)..."
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        [weakSelf promptConnectPeerByIp];
    }]];

    if (_bridge.isCslamEnabled) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"Sync Shared Origin with Connected Peers"
                                                  style:UIAlertActionStyleDefault
                                                handler:^(UIAlertAction *action) {
            MainScannerViewController *strongSelf = weakSelf;
            if (strongSelf) [strongSelf->_bridge syncSharedOriginWithPeers];
        }]];
    }

    [sheet addAction:[UIAlertAction actionWithTitle:@"Close" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)promptConnectPeerByIp {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Connect C-SLAM Device by IP"
                                                                   message:@"Enter the Wi-Fi IP of an iPad/iPhone, Jetson (AV SLAM / AV Scanar), or Windows PC (AV Scan):"
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.placeholder = @"e.g. 192.168.1.50 or 172.20.10.2";
        tf.keyboardType = UIKeyboardTypeNumbersAndPunctuation;
    }];

    MainScannerViewController * __weak weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Connect"
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        MainScannerViewController *strongSelf = weakSelf;
        if (!strongSelf) return;
        NSString *ip = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (ip.length > 0) {
            [strongSelf->_bridge connectCollaborativePeerByIp:ip];
            [strongSelf updateCslamButtonTitle];
        }
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
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
            NSString *stId = [_bridge nextAvailableStationIdWithPrefix:@"st"];
            NSString *stName = [NSString stringWithFormat:@"Station %@", [stId stringByReplacingOccurrencesOfString:@"st_" withString:@""]];
            [_bridge startTerrestrialStationWithId:stId name:stName stepDeg:60.0 isFlashDuringSlam:NO];
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
        [_poiPhotoBtn setTitle:@"Flash Scan" forState:UIControlStateNormal]; // Single-shot stationary Flash Scan (no motor required)
        _startStopBtn.backgroundColor = [[AppleGlassTheme statusErr] colorWithAlphaComponent:0.40];
        _startStopBtn.layer.borderColor = [AppleGlassTheme statusErr].CGColor;
    }
}

- (void)onPoiPhotoTapped {
    if (_bridge.isScanning && !_bridge.isTerrestrialStationActive) {
        // Single-shot stationary Flash Scan during Mobile SLAM — ZERO motorized head or turntable required!
        NSString *stId = [_bridge nextAvailableStationIdWithPrefix:@"flash_st"];
        [_bridge startTerrestrialStationWithId:stId name:@"Flash Scan Anchor" stepDeg:360.0 isFlashDuringSlam:YES];
        [_poiPhotoBtn setTitle:@"Commit Flash" forState:UIControlStateNormal];
        return;
    } else if (_bridge.isTerrestrialStationActive && _bridge.isSlamPausedForStation) {
        // Operator can tap early to commit the Flash Scan immediately, or let 5 s auto-complete finalize it
        [_bridge finalizeAndSaveTerrestrialStation];
        [_poiPhotoBtn setTitle:@"Flash Scan" forState:UIControlStateNormal];
        return;
    } else if (_bridge.isTerrestrialStationActive && !_bridge.isSlamPausedForStation) {
        // Repeat current terrestrial stop if tapped during standalone Terrestrial Mode
        [_bridge repeatCurrentTerrestrialStop];
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
    if ([name containsString:@"ScanAR"]) {
        _deviceProfileItem.title = @"ScanAR 336L ▾";
    } else if ([name containsString:@"iPad 10th Gen"]) {
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
    [self updateCslamButtonTitle];
    auto cloud = [_bridge latestGlobalPointCloud];
    auto traj = [_bridge currentTrajectory];
    [_renderer updatePointCloud:cloud trajectory:traj];

    auto plan = [_bridge latestFloorPlan];
    [_floorPlanView updateWithFloorPlan:plan];

    auto pkt = [_bridge currentPacketStats];
    auto qual = [_bridge latestQualityReport];
    auto autoCalib = [_bridge currentAutoCalibStatus];
    size_t loops = [_bridge loopClosureCount];

    if (_bridge.isTerrestrialStationActive) {
        double prog = [_bridge terrestrialAccumulationProgress] * 100.0;
        BOOL isStill = [_bridge isTerrestrialStationary];
        auto tState = [_bridge currentTerrestrialCaptureState];

        if (_bridge.isSlamPausedForStation) {
            // Single-shot stationary Flash Scan during Mobile SLAM (no turntable/motor needed)
            if (tState == avscan::terrestrial::TerrestrialCaptureState::STATION_COMPLETED) {
                [_bridge finalizeAndSaveTerrestrialStation];
                [_poiPhotoBtn setTitle:@"Flash Scan" forState:UIControlStateNormal];
                return;
            }
            NSString *flashState = (tState == avscan::terrestrial::TerrestrialCaptureState::ACCUMULATING_STOP)
                ? [NSString stringWithFormat:@"CAPTURING DENSE FLASH ANCHOR (%.0f%%) — HOLD STILL", prog]
                : @"HOLD SCANNER STEADY (NO MOTOR REQUIRED)";
            _hudStatusBanner.text = [NSString stringWithFormat:
                @"RIG: %@  ·  FLASH SCAN (SINGLE-SHOT)\nPTS: %zu  ·  STILL: %@\n%@",
                [_bridge activeDeviceProfileDisplayName],
                cloud.size(),
                isStill ? @"YES" : @"SETTLING",
                flashState];
            _hudMetricsLabel.text = [NSString stringWithFormat:
                @"FLASH PROGRESS: %.0f%%  ·  AUTO-RESUMES SLAM ON COMPLETE\nTAP 'COMMIT FLASH' TO FINISH EARLY",
                prog];
            return;
        }

        uint32_t stopIdx = [_bridge currentTerrestrialStopIndex];
        NSString *stateStr = @"WAITING FOR STILLNESS (HOLD STEADY)";
        if (tState == avscan::terrestrial::TerrestrialCaptureState::ACCUMULATING_STOP) {
            stateStr = [NSString stringWithFormat:@"ACCUMULATING STOP %u/6 (%.0f%%)", stopIdx + 1, prog];
        } else if (tState == avscan::terrestrial::TerrestrialCaptureState::STOP_COMPLETED) {
            stateStr = [NSString stringWithFormat:@"STOP %u/6 COMPLETE — ROTATE +60° & TAP NEXT", stopIdx + 1];
        } else if (tState == avscan::terrestrial::TerrestrialCaptureState::STATION_COMPLETED) {
            stateStr = @"ALL 6 STOPS COMPLETE — TAP FINISH STATION";
        }

        _hudStatusBanner.text = [NSString stringWithFormat:
            @"RIG: %@  ·  TERRESTRIAL STATION\nSTOP: %u/6 (%u°)  ·  PTS: %zu  ·  STILL: %@\n%@",
            [_bridge activeDeviceProfileDisplayName],
            stopIdx + 1,
            stopIdx * 60,
            cloud.size(),
            isStill ? @"YES" : @"SETTLING",
            stateStr];

        auto meta = [_bridge currentTerrestrialStationMetadata];
        _hudMetricsLabel.text = [NSString stringWithFormat:
            @"PROGRESS: %.0f%%  ·  STOPS DONE: %zu/6\nAZ COV: %.0f%%  ·  EL COV: %.0f%%",
            prog,
            meta.total_stops,
            meta.azimuth_coverage_ratio * 100.0,
            meta.elevation_coverage_ratio * 100.0];
        return;
    }

    _hudStatusBanner.text = [NSString stringWithFormat:
        @"%@ · %@ (%@)\nLOOPS: %zu  ·  PTS: %zu  ·  LOSS: %.2f%%\n%s",
        _bridge.activeProjectName ?: @"Project",
        _bridge.activeScanName ?: @"Scan 01",
        _bridge.activeScanFloorLevel ?: @"Level 01",
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

    double camFps = [_bridge cameraMeasuredFps];
    BOOL rgbOn = [_bridge isRgbColorizationActive];
    BOOL extUsb = [_bridge activeProfileUsesExternalUsbCamera];
    NSString *camPrefix = extUsb ? @"USB 336L" : @"CAM";
    NSString *camTag = _bridge.isScanning
        ? ((camFps >= 1.0)
            ? [NSString stringWithFormat:@"%@ %.0ffps · %@", camPrefix, camFps, rgbOn ? @"RGB ON" : @"RGB WAIT"]
            : [NSString stringWithFormat:@"%@: %@", camPrefix, [_bridge cameraStatusSummary]])
        : (extUsb
            ? [NSString stringWithFormat:@"RIG: ScanAR V4 · USB Cams: %lu", (unsigned long)[_bridge connectedExternalUsbCameras].count]
            : [NSString stringWithFormat:@"RIG: %@", [_bridge activeDeviceProfileDisplayName]]);

    _hudMetricsLabel.text = [NSString stringWithFormat:
        @"QUALITY: %.0f%% · RMSE: %.3f m · OVERLAP: %.0f%%\n%@",
        qual.overall_quality_score * 100.0,
        qual.registration_rmse_m,
        qual.scan_overlap_ratio * 100.0,
        camTag];
}

@end
