#pragma once
// Live bottleneck diagnosis. Pure C++ (no Windows code, testable anywhere).

#include <string>
#include <vector>

#include "analyzer.h"
#include "hardware_types.h"
#include "process_types.h"

enum class BottleneckKind { None, Cpu, Memory, Storage, Network };

struct BottleneckFinding {
    BottleneckKind kind = BottleneckKind::None;
    std::string component;     // "CPU", "RAM", "Storage", "Network"
    Urgency urgency = Urgency::Ok;
    std::string title;
    std::string detail;
    std::string action;
};

struct LiveDiagnosis {
    BottleneckKind primary = BottleneckKind::None;
    std::string summary;
    std::vector<BottleneckFinding> findings;
};

// `live` should be an average over several seconds, not a single instant.
// `topRam` / `topCpu` (optional) name the apps responsible in the advice.
LiveDiagnosis diagnoseLive(const Assessment& assessment, const LiveStats& live,
                           const std::vector<ProcessGroup>& topRam = {},
                           const std::vector<ProcessGroup>& topCpu = {});
const char* bottleneckLabel(BottleneckKind kind);
