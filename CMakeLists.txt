cmake_minimum_required(VERSION 3.20)
project(AVScan VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AVSCAN_BUILD_TESTS "Build automated unit, integration, and regression tests" ON)
option(AVSCAN_BUILD_IOS_APP "Build native iOS/iPadOS application target" OFF)
option(AVSCAN_ENABLE_RESEARCH_FASTLIVO2 "Enable isolated GPLv2 evaluation FAST-LIVO2 front-end adapter" ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(AVSCAN_BUILD_IOS_APP ON)
    enable_language(OBJCXX)
endif()

# Core portable C++17 sources (M1 - M11)
set(AVSCAN_CORE_SOURCES
    src/sensors/AiryDiscovery.cpp
    src/sensors/AiryDecoder.cpp
    src/sensors/AiryUdpReceiver.cpp
    src/sync/TimeSynchronizer.cpp
    src/slam/VoxelHashMap.cpp
    src/slam/FastLivo2Engine.cpp
    src/calibration/TargetlessCalibrator.cpp
    src/backend/ScanContext.cpp
    src/backend/GicpVerifier.cpp
    src/backend/PoseGraphOptimizer.cpp
    src/quality/QualityMonitor.cpp
    src/storage/ProjectStore.cpp
    src/export/PointCloudExporter.cpp
    src/system/ResourceController.cpp
)

add_library(avscan_core STATIC ${AVSCAN_CORE_SOURCES})

target_include_directories(avscan_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(WIN32)
    target_compile_definitions(avscan_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(avscan_core PUBLIC ws2_32 iphlpapi)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(avscan_core PUBLIC pthread m)
endif()

if(AVSCAN_ENABLE_RESEARCH_FASTLIVO2)
    target_compile_definitions(avscan_core PUBLIC AVSCAN_HAS_EVAL_FASTLIVO2=1)
endif()

# Native iOS / iPadOS Application Target (Universal iPhone + iPad A2696)
if(AVSCAN_BUILD_IOS_APP)
    set(AVSCAN_IOS_SOURCES
        ios/AVScan/main.mm
        ios/AVScan/AppDelegate.mm
        ios/AVScan/Bridge/AVScanCoreBridge.mm
        ios/AVScan/Sensors/CameraCaptureManager.mm
        ios/AVScan/Rendering/MetalPointCloudRenderer.mm
        ios/AVScan/UI/MainScannerViewController.mm
        ios/AVScan/UI/DiagnosticsViewController.mm
        ios/AVScan/UI/CalibrationViewController.mm
        ios/AVScan/UI/FloorPlanView.mm
    )

    add_executable(AVScan MACOSX_BUNDLE ${AVSCAN_IOS_SOURCES})

    target_include_directories(AVScan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/ios/AVScan
    )

    target_compile_options(AVScan PRIVATE -fobjc-arc)

    target_link_libraries(AVScan PRIVATE
        avscan_core
        "-framework UIKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework AVFoundation"
        "-framework ARKit"
        "-framework CoreMotion"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreGraphics"
        "-framework QuartzCore"
        "-framework Network"
        "-framework SystemConfiguration"
        "-framework UniformTypeIdentifiers"
        "-lsqlite3"
    )

    set_target_properties(AVScan PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/AVScan/Info.plist"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.artificialvision.avscan"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
    )
endif()

# Automated Test Suite Target (M0 - M11)
if(AVSCAN_BUILD_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_testing()

    add_executable(avscan_tests
        tests/test_main.cpp
        tests/test_m1_airy_discovery.cpp
        tests/test_m2_airy_decoder.cpp
        tests/test_m4_fast_livo2.cpp
        tests/test_m5_m6_sync_fusion.cpp
        tests/test_m7_calibration.cpp
        tests/test_m8_loop_closure_pgo.cpp
        tests/test_m9_quality_reloc.cpp
        tests/test_m10_storage_export.cpp
        tests/test_m11_hardening.cpp
    )

    target_link_libraries(avscan_tests PRIVATE avscan_core)

    add_test(NAME AVScanFullTestSuite COMMAND avscan_tests)
endif()
