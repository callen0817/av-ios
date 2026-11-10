#include <cstdio>
#include <exception>

bool run_test_m1_airy_discovery();
bool run_test_m2_airy_decoder();
bool run_test_m4_fast_livo2();
bool run_test_m5_m6_sync_fusion();
bool run_test_m7_calibration();
bool run_test_m8_loop_closure_pgo();
bool run_test_m9_quality_reloc();
bool run_test_m10_storage_export();
bool run_test_m11_hardening();

int main() {
    struct SuiteEntry {
        const char* name;
        bool (*fn)();
    };

    const SuiteEntry suites[] = {
        {"M1 Airy Ethernet Discovery & Diagnostics", run_test_m1_airy_discovery},
        {"M2 Airy MSOP/DIFOP/IMU Packet Decoder & Recorder", run_test_m2_airy_decoder},
        {"M4 FAST-LIVO2 Native C++ Engine & VoxelMap", run_test_m4_fast_livo2},
        {"M5-M6 Sensor Synchronization & LIVO Visual Fusion", run_test_m5_m6_sync_fusion},
        {"M7 Targetless Calibration (DirectVLCalib + Hand-Eye AX=XB)", run_test_m7_calibration},
        {"M8 Scan Context Loop Closure, GICP & Pose Graph Optimization", run_test_m8_loop_closure_pgo},
        {"M9 Mapping Quality, Dynamic Filtering & Relocalization", run_test_m9_quality_reloc},
        {"M10 Project Storage, Crash Recovery & PLY/PCD/E57 Exports", run_test_m10_storage_export},
        {"M11 Commercial Hardening, Thermal Throttling & Memory Budget", run_test_m11_hardening}
    };

    int passed = 0;
    int failed = 0;
    for (const auto& s : suites) {
        bool ok = false;
        try {
            ok = s.fn();
        } catch (const std::exception& e) {
            std::printf("[FAIL] %s: exception: %s\n", s.name, e.what());
            ok = false;
        }
        if (ok) {
            std::printf("[PASS] %s\n", s.name);
            passed++;
        } else {
            std::printf("[FAIL] %s\n", s.name);
            failed++;
        }
    }

    std::printf("\n=== AV Scan C++ Test Summary: %d PASSED, %d FAILED ===\n", passed, failed);
    return (failed == 0) ? 0 : 1;
}
