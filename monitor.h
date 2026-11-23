#pragma once
#include <chrono>
#include <string>
#include <unordered_map>

#include "hardware_types.h"

// NOTE: no Windows headers in this file on purpose. They live in monitor.cpp,
// so that including this header next to httplib.h (which pulls in winsock)
// in the server phase does not cause "redefinition" errors.

class SystemMonitor {
public:
    SystemMonitor();                          // takes the first baseline reading

    static HardwareInfo readHardwareInfo();   // one-time static info
    static void enableUtf8Console();          // so UTF-8 device names print correctly
    static void hardenProcess();              // call first thing in main(): safer DLL loading, heap protection
    LiveStats readStats();                    // call about once per second

private:
    struct CpuTimes {
        unsigned long long idle = 0;
        unsigned long long total = 0;
    };
    struct NetCounters {
        unsigned long long rx = 0;   // bytes received
        unsigned long long tx = 0;   // bytes sent
    };
    // One entry per network adapter, keyed by its interface LUID. Speeds are computed
    // per adapter, so an adapter that appears/disappears between two readings cannot
    // inject its whole lifetime byte count into the result (a fake speed spike).
    using NetSnapshot = std::unordered_map<unsigned long long, NetCounters>;

    static bool readCpuTimes(CpuTimes& out);
    static bool readNetSnapshot(NetSnapshot& out);
    static double cpuUsagePercent(const CpuTimes& before, const CpuTimes& now);

    CpuTimes prevCpu_;
    NetSnapshot prevNet_;
    std::chrono::steady_clock::time_point prevTime_;
    std::string sysDrive_;                    // drive Windows is installed on, e.g. "C:"
    bool cpuBaselineOk_ = false;
    bool netBaselineOk_ = false;
};
