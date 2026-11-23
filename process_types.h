#pragma once
// Pure C++ process data + aggregation (no Windows code, testable anywhere).

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct ProcessSnapshot {
    uint32_t pid = 0;
    std::string name;            // UTF-8 executable name (control characters already replaced)
    double ramMB = 0.0;          // working set in MB
    double cpuPercent = 0.0;     // 0.0 - 100.0 normalized across logical CPUs
    bool accessRestricted = false;
};

// All processes sharing one executable name (Chrome has ~20 of them), summed.
struct ProcessGroup {
    std::string name;
    int count = 0;               // number of processes
    double ramMB = 0.0;
    double cpuPercent = 0.0;
    bool anyRestricted = false;  // at least one process could not be inspected (needs Administrator)
};

// Replaces control characters (and invalid leftovers) so a hostile process name
// cannot inject terminal escape sequences or break JSON/log output.
std::string sanitizeProcessName(const std::string& raw);

// Groups by case-insensitive executable name. Result is unsorted.
std::vector<ProcessGroup> groupProcesses(const std::vector<ProcessSnapshot>& procs);

std::vector<ProcessGroup> topByRam(std::vector<ProcessGroup> groups, std::size_t limit);
std::vector<ProcessGroup> topByCpu(std::vector<ProcessGroup> groups, std::size_t limit);

// Averages groups over several samples, so one short spike does not decide the
// result. A group missing from a sample counts as 0 for that sample.
class ProcessAverager {
public:
    void add(const std::vector<ProcessGroup>& sample);
    int samples() const { return samples_; }
    std::vector<ProcessGroup> averaged() const;   // unsorted

private:
    struct Acc {
        std::string name;
        double ramSum = 0.0;
        double cpuSum = 0.0;
        int maxCount = 0;
        bool restricted = false;
    };
    std::unordered_map<std::string, Acc> acc_;
    int samples_ = 0;
};
