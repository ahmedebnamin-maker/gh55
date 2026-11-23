#include "process_types.h"

#include <algorithm>
#include <cctype>

std::string sanitizeProcessName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        out.push_back((c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c));
    }
    return out;
}

namespace {
std::string lowerKey(const std::string& s) {
    std::string k = s;
    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}
}  // namespace

std::vector<ProcessGroup> groupProcesses(const std::vector<ProcessSnapshot>& procs) {
    std::unordered_map<std::string, ProcessGroup> map;
    for (const ProcessSnapshot& p : procs) {
        ProcessGroup& g = map[lowerKey(p.name)];
        if (g.name.empty()) g.name = sanitizeProcessName(p.name);
        g.count += 1;
        g.ramMB += p.ramMB;
        g.cpuPercent += p.cpuPercent;
        g.anyRestricted = g.anyRestricted || p.accessRestricted;
    }
    std::vector<ProcessGroup> out;
    out.reserve(map.size());
    for (auto& kv : map) out.push_back(std::move(kv.second));
    return out;
}

std::vector<ProcessGroup> topByRam(std::vector<ProcessGroup> g, std::size_t limit) {
    std::sort(g.begin(), g.end(), [](const ProcessGroup& a, const ProcessGroup& b) {
        if (a.ramMB != b.ramMB) return a.ramMB > b.ramMB;
        return a.name < b.name;
    });
    if (limit > 0 && g.size() > limit) g.resize(limit);
    return g;
}

std::vector<ProcessGroup> topByCpu(std::vector<ProcessGroup> g, std::size_t limit) {
    std::sort(g.begin(), g.end(), [](const ProcessGroup& a, const ProcessGroup& b) {
        if (a.cpuPercent != b.cpuPercent) return a.cpuPercent > b.cpuPercent;
        if (a.ramMB != b.ramMB) return a.ramMB > b.ramMB;
        return a.name < b.name;
    });
    if (limit > 0 && g.size() > limit) g.resize(limit);
    return g;
}

void ProcessAverager::add(const std::vector<ProcessGroup>& sample) {
    for (const ProcessGroup& g : sample) {
        Acc& a = acc_[lowerKey(g.name)];
        if (a.name.empty()) a.name = g.name;
        a.ramSum += g.ramMB;
        a.cpuSum += g.cpuPercent;
        a.maxCount = std::max(a.maxCount, g.count);
        a.restricted = a.restricted || g.anyRestricted;
    }
    ++samples_;
}

std::vector<ProcessGroup> ProcessAverager::averaged() const {
    std::vector<ProcessGroup> out;
    if (samples_ <= 0) return out;
    out.reserve(acc_.size());
    for (const auto& kv : acc_) {
        ProcessGroup g;
        g.name = kv.second.name;
        g.count = kv.second.maxCount;
        g.ramMB = kv.second.ramSum / samples_;
        g.cpuPercent = kv.second.cpuSum / samples_;
        g.anyRestricted = kv.second.restricted;
        out.push_back(std::move(g));
    }
    return out;
}
