#include "json_report.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace {

std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string num(double v, int decimals = 1) {
    if (!std::isfinite(v)) return "0";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

const char* storageName(StorageType t) {
    switch (t) {
        case StorageType::SSD: return "SSD";
        case StorageType::HDD: return "HDD";
        default: return "unknown";
    }
}

const char* urgencyKey(Urgency u) {
    switch (u) {
        case Urgency::Ok: return "ok";
        case Urgency::Soon: return "soon";
        default: return "now";
    }
}

const char* kindKey(BottleneckKind k) {
    switch (k) {
        case BottleneckKind::Cpu: return "cpu";
        case BottleneckKind::Memory: return "memory";
        case BottleneckKind::Storage: return "storage";
        case BottleneckKind::Network: return "network";
        default: return "none";
    }
}

}  // namespace

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string toJson(const HardwareInfo& hw) {
    std::ostringstream o;
    o << "{\"cpuName\":" << q(hw.cpuName) << ",\"logicalProcessors\":" << hw.logicalProcessors
      << ",\"isLaptop\":" << (hw.isLaptop ? "true" : "false")
      << ",\"ram\":{\"totalGB\":" << num(hw.totalRamGB) << ",\"nominalGB\":" << nominalRamGB(hw.totalRamGB)
      << ",\"modules\":" << hw.ramModules << ",\"slots\":" << hw.ramSlotsTotal << ",\"speedMHz\":" << hw.ramSpeedMHz
      << ",\"type\":" << q(hw.ramType) << ",\"soldered\":" << (hw.ramLikelySoldered ? "true" : "false") << "}"
      << ",\"storage\":{\"drive\":" << q(hw.systemDrive) << ",\"totalGB\":" << num(hw.diskTotalGB)
      << ",\"freeGB\":" << num(hw.diskFreeGB) << ",\"type\":" << q(storageName(hw.storageType)) << "}"
      << ",\"screen\":{\"width\":" << hw.screenWidth << ",\"height\":" << hw.screenHeight
      << ",\"refreshHz\":" << hw.refreshHz << "}"
      << ",\"gpus\":[";
    for (size_t i = 0; i < hw.gpus.size(); ++i) {
        if (i) o << ",";
        o << "{\"name\":" << q(hw.gpus[i].name) << ",\"vramGB\":" << num(hw.gpus[i].vramGB, 2) << "}";
    }
    o << "]}";
    return o.str();
}

std::string toJson(const Assessment& a) {
    std::ostringstream o;
    o << "{\"useCase\":" << q(useCaseLabel(a.useCase)) << ",\"overallScore\":" << a.overallScore
      << ",\"verdict\":" << q(a.verdict) << ",\"performanceIndex\":" << a.performanceIndex
      << ",\"category\":" << q(a.category)
      << ",\"cpu\":{\"recognized\":" << (a.cpu.recognized ? "true" : "false") << ",\"vendor\":" << q(a.cpu.vendor)
      << ",\"family\":" << q(a.cpu.family) << ",\"generation\":" << q(a.cpu.generation)
      << ",\"releaseYear\":" << a.cpu.releaseYear << ",\"mobile\":" << (a.cpu.mobile ? "true" : "false")
      << ",\"socket\":" << q(a.cpu.socket) << "}"
      << ",\"scores\":{\"cpu\":" << a.cpuScore << ",\"gpu\":" << a.gpuScore << ",\"ram\":" << a.ramScore
      << ",\"storage\":" << a.storageScore << ",\"screen\":" << a.screenScore << "}"
      << ",\"fit\":{\"cpu\":" << a.cpuFit << ",\"gpu\":" << a.gpuFit << ",\"ram\":" << a.ramFit
      << ",\"storage\":" << a.storageFit << ",\"screen\":" << a.screenFit << "}"
      << ",\"fitByUseCase\":{\"office\":" << a.fitByUseCase[kOffice] << ",\"gaming\":" << a.fitByUseCase[kGaming]
      << ",\"creator\":" << a.fitByUseCase[kCreator] << ",\"developer\":" << a.fitByUseCase[kDeveloper] << "}"
      << ",\"yearsLeft\":" << a.yearsLeft << ",\"limitingComponent\":" << q(a.limitingComponent)
      << ",\"primaryGpu\":" << q(a.primaryGpu) << ",\"hasDiscreteGpu\":" << (a.hasDiscreteGpu ? "true" : "false")
      << ",\"screenLabel\":" << q(a.screenLabel)
      << ",\"priceNote\":" << q(priceDisclaimer()) << ",\"advice\":[";
    for (size_t i = 0; i < a.advice.size(); ++i) {
        if (i) o << ",";
        o << "{\"component\":" << q(a.advice[i].component) << ",\"urgency\":" << q(urgencyKey(a.advice[i].urgency))
          << ",\"text\":" << q(a.advice[i].text) << "}";
    }
    o << "],\"recommendations\":[";
    for (size_t i = 0; i < a.recommendations.size(); ++i) {
        const Recommendation& r = a.recommendations[i];
        if (i) o << ",";
        o << "{\"component\":" << q(r.component) << ",\"priority\":" << q(urgencyKey(r.priority))
          << ",\"title\":" << q(r.title) << ",\"detail\":" << q(r.detail) << ",\"examples\":" << q(r.examples)
          << ",\"priceMinUSD\":" << r.priceMinUSD << ",\"priceMaxUSD\":" << r.priceMaxUSD
          << ",\"extraYears\":" << r.extraYears << "}";
    }
    o << "]}";
    return o.str();
}

std::string toJson(const LiveStats& s) {
    std::ostringstream o;
    o << "{\"cpuPercent\":" << num(s.cpuPercent) << ",\"ramPercent\":" << num(s.ramPercent)
      << ",\"ramUsedGB\":" << num(s.ramUsedGB, 2) << ",\"diskUsedPercent\":" << num(s.diskUsedPercent)
      << ",\"downKBps\":" << num(s.downKBps) << ",\"upKBps\":" << num(s.upKBps) << "}";
    return o.str();
}

std::string toJson(const LiveDiagnosis& d) {
    std::ostringstream o;
    o << "{\"primary\":" << q(kindKey(d.primary)) << ",\"summary\":" << q(d.summary) << ",\"findings\":[";
    for (size_t i = 0; i < d.findings.size(); ++i) {
        const BottleneckFinding& f = d.findings[i];
        if (i) o << ",";
        o << "{\"kind\":" << q(kindKey(f.kind)) << ",\"component\":" << q(f.component)
          << ",\"urgency\":" << q(urgencyKey(f.urgency)) << ",\"title\":" << q(f.title)
          << ",\"detail\":" << q(f.detail) << ",\"action\":" << q(f.action) << "}";
    }
    o << "]}";
    return o.str();
}

std::string toJson(const std::vector<ProcessGroup>& groups) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < groups.size(); ++i) {
        const ProcessGroup& g = groups[i];
        if (i) o << ",";
        o << "{\"name\":" << q(g.name) << ",\"count\":" << g.count << ",\"ramMB\":" << num(g.ramMB)
          << ",\"cpuPercent\":" << num(g.cpuPercent, 2)
          << ",\"restricted\":" << (g.anyRestricted ? "true" : "false") << "}";
    }
    o << "]";
    return o.str();
}

std::string toJson(const ScanResult& r) {
    std::ostringstream o;
    o << "{\"error\":" << q(r.error) << ",\"truncated\":" << (r.truncated ? "true" : "false")
      << ",\"seconds\":" << num(r.seconds) << ",\"totalBytes\":" << r.totalBytes << ",\"folders\":[";
    for (size_t i = 0; i < r.folders.size(); ++i) {
        if (i) o << ",";
        o << "{\"path\":" << q(r.folders[i].path) << ",\"bytes\":" << r.folders[i].bytes
          << ",\"files\":" << r.folders[i].files << "}";
    }
    o << "]}";
    return o.str();
}
