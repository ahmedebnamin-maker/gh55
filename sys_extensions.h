#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "analyzer.h"
#include "diagnosis.h"       // live bottleneck diagnosis (pure C++)
#include "monitor.h"
#include "process_types.h"   // ProcessSnapshot / ProcessGroup (pure C++)

// ============================================================================
// Process monitor, working-set trim and UAC elevation (Windows)
// ============================================================================
struct TrimResult {
    bool ok = false;
    bool needsElevation = false;
    uint32_t errorCode = 0;
    double beforeMB = 0.0;
    double afterMB = 0.0;
    double freedMB = 0.0;
    std::string message;
};

class ProcessMonitor {
public:
    ProcessMonitor();

    // Returns processes sorted by RAM (default) or CPU (sortByCpu = true).
    // limit = 0 returns all of them (use this when you group them afterwards).
    // The first call only takes a baseline: CPU% is 0 until the second call.
    std::vector<ProcessSnapshot> listProcesses(bool sortByCpu = false, std::size_t limit = 25);

    // Trims the working set of `pid` and reports before/after memory in MB.
    static TrimResult trimWorkingSet(uint32_t pid);

    // Elevation helpers
    static bool isElevated();
    // Relaunches this program through the UAC prompt with the SAME command-line
    // arguments (plus `extraArg`, e.g. "--elevated-child"). Returns false if the
    // user cancels the prompt or it fails; `errorOut` receives the Win32 error.
    // On success returns true; if closeCurrentProcessOnSuccess the caller's
    // process exits afterwards.
    static bool relaunchAsAdmin(const char* extraArg = nullptr, bool closeCurrentProcessOnSuccess = true,
                                uint32_t* errorOut = nullptr);

private:
    struct CpuSample {
        uint64_t procKernel100ns = 0;
        uint64_t procUser100ns = 0;
        uint64_t sysTotal100ns = 0;
    };

    static bool readSystemTotal100ns(uint64_t& totalOut);

    std::unordered_map<uint32_t, CpuSample> prevSamples_;
    std::chrono::steady_clock::time_point lastSampleTime_{};
    bool hasBaseline_ = false;
};
