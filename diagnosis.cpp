#include "diagnosis.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace {

double sanitizePercent(double v) {
    if (!std::isfinite(v)) return 0.0;
    return std::clamp(v, 0.0, 100.0);
}

double sanitizeNonNegative(double v) {
    if (!std::isfinite(v) || v < 0.0) return 0.0;
    return v;
}

std::string fmt1(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << sanitizeNonNegative(v);
    return o.str();
}

int severityRank(Urgency u) {
    switch (u) {
        case Urgency::Now: return 3;
        case Urgency::Soon: return 2;
        case Urgency::Ok: return 1;
    }
    return 0;
}

// "chrome.exe (1,2 GB), code.exe (800 MB)" for the first `n` groups.
std::string describeTop(const std::vector<ProcessGroup>& g, bool byRam, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < g.size() && i < n; ++i) {
        if (i) s += ", ";
        s += g[i].name;
        if (g[i].count > 1) s += " x" + std::to_string(g[i].count);
        s += byRam ? " (" + fmt1(g[i].ramMB) + " MB)" : " (" + fmt1(g[i].cpuPercent) + "% CPU)";
    }
    return s;
}

}  // namespace

const char* bottleneckLabel(BottleneckKind kind) {
    switch (kind) {
        case BottleneckKind::Cpu: return "CPU Bottleneck";
        case BottleneckKind::Memory: return "Memory Pressure";
        case BottleneckKind::Storage: return "Storage Bottleneck";
        case BottleneckKind::Network: return "Heavy Network Transfer";
        default: return "Balanced";
    }
}

LiveDiagnosis diagnoseLive(const Assessment& assessment, const LiveStats& live,
                           const std::vector<ProcessGroup>& topRam,
                           const std::vector<ProcessGroup>& topCpu) {
    LiveDiagnosis diag;

    const double cpuPct = sanitizePercent(live.cpuPercent);
    const double ramPct = sanitizePercent(live.ramPercent);
    const double diskPct = sanitizePercent(live.diskUsedPercent);
    const double downKBps = sanitizeNonNegative(live.downKBps);
    const double upKBps = sanitizeNonNegative(live.upKBps);

    const std::string cpuCulprits = describeTop(topCpu, false, 3);
    const std::string ramCulprits = describeTop(topRam, true, 3);

    // 1. CPU check
    if (cpuPct >= 90.0) {
        BottleneckFinding f;
        f.kind = BottleneckKind::Cpu;
        f.component = "CPU";
        f.urgency = (assessment.cpuScore < 45) ? Urgency::Now : Urgency::Soon;
        f.title = "High CPU utilization (" + fmt1(cpuPct) + "%)";
        f.detail = "The processor is near saturation";
        if (assessment.cpu.recognized && !assessment.cpu.generation.empty()) {
            f.detail += " on " + assessment.cpu.generation;
        }
        f.detail += ".";
        if (!cpuCulprits.empty()) f.detail += " Biggest users: " + cpuCulprits + ".";
        f.action = "Close the heaviest app above or wait for it to finish.";
        diag.findings.push_back(std::move(f));
    } else if (cpuPct >= 75.0 && assessment.cpuScore < 50) {
        BottleneckFinding f;
        f.kind = BottleneckKind::Cpu;
        f.component = "CPU";
        f.urgency = Urgency::Soon;
        f.title = "Moderate-to-high CPU load (" + fmt1(cpuPct) + "%)";
        f.detail = "A lower-tier or older CPU is working hard under the current load.";
        if (!cpuCulprits.empty()) f.detail += " Biggest users: " + cpuCulprits + ".";
        f.action = "Watch for lag during multitasking.";
        diag.findings.push_back(std::move(f));
    }

    // 2. RAM / Memory check
    if (ramPct >= 90.0) {
        BottleneckFinding f;
        f.kind = BottleneckKind::Memory;
        f.component = "RAM";
        f.urgency = Urgency::Now;
        f.title = "Critical RAM usage (" + fmt1(ramPct) + "%)";
        f.detail = "System memory is nearly full (" + fmt1(live.ramUsedGB) +
                   " GB used), so Windows starts using the disk as memory (slow).";
        if (!ramCulprits.empty()) f.detail += " Biggest users: " + ramCulprits + ".";
        f.action = "Close memory-heavy apps or browser tabs; consider more RAM.";
        diag.findings.push_back(std::move(f));
    } else if (ramPct >= 78.0) {
        BottleneckFinding f;
        f.kind = BottleneckKind::Memory;
        f.component = "RAM";
        f.urgency = Urgency::Soon;
        f.title = "Elevated RAM usage (" + fmt1(ramPct) + "%)";
        f.detail = "Memory headroom is getting tight (" + fmt1(live.ramUsedGB) + " GB used).";
        if (!ramCulprits.empty()) f.detail += " Biggest users: " + ramCulprits + ".";
        f.action = "Keep an eye on background apps and browser tabs.";
        diag.findings.push_back(std::move(f));
    }

    // 3. Storage check
    if (diskPct >= 92.0) {
        BottleneckFinding f;
        f.kind = BottleneckKind::Storage;
        f.component = "Storage";
        f.urgency = Urgency::Now;
        f.title = "System drive nearly full (" + fmt1(diskPct) + "% used)";
        f.detail = "Very little free space remains, which hurts Windows and the page file.";
        f.action = "Free up disk space immediately.";
        diag.findings.push_back(std::move(f));
    } else if (ramPct >= 80.0 && assessment.storageScore <= 25) {
        BottleneckFinding f;
        f.kind = BottleneckKind::Storage;
        f.component = "Storage";
        f.urgency = Urgency::Now;
        f.title = "Paging slowdown risk on slow storage";
        f.detail = "High RAM usage combined with a hard disk causes noticeable stutters.";
        f.action = "Reduce RAM load or upgrade the system drive to an SSD.";
        diag.findings.push_back(std::move(f));
    }

    // 4. Network: informational only (a fast download is not a fault).
    const double totalNetKBps = downKBps + upKBps;
    if (totalNetKBps >= 51200.0) {
        BottleneckFinding f;
        f.kind = BottleneckKind::Network;
        f.component = "Network";
        f.urgency = Urgency::Ok;
        f.title = "Heavy network transfer (" + fmt1(totalNetKBps / 1024.0) + " MB/s)";
        f.detail = "Down: " + fmt1(downKBps / 1024.0) + " MB/s | Up: " + fmt1(upKBps / 1024.0) + " MB/s.";
        f.action = "Informational: check active downloads, uploads or cloud sync if the connection feels slow.";
        diag.findings.push_back(std::move(f));
    }

    // Only real problems (Soon / Now) can be the "primary" bottleneck.
    const BottleneckFinding* best = nullptr;
    for (const auto& f : diag.findings) {
        if (f.urgency == Urgency::Ok) continue;
        if (!best || severityRank(f.urgency) > severityRank(best->urgency)) best = &f;
    }

    if (!best) {
        diag.primary = BottleneckKind::None;
        diag.summary = "No active bottleneck detected; system load looks balanced.";
    } else {
        diag.primary = best->kind;
        diag.summary = std::string(bottleneckLabel(best->kind)) + ": " + best->title;
    }
    return diag;
}
