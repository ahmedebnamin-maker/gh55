#include <chrono>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "analyzer.h"
#include "diagnosis.h"
#include "disk_scan.h"
#include "json_report.h"
#include "monitor.h"
#include "process_types.h"
#include "sys_extensions.h"

int runAnalyzerTests();   // defined in analyzer_tests.cpp

namespace {

struct Options {
    bool test = false;
    bool json = false;
    bool help = false;
    bool elevate = false;        // ask for Administrator rights up front
    bool elevatedChild = false;  // internal: we were started by --elevate / the prompt
    bool scan = false;
    std::string scanPath;        // empty = <system drive>\Users
    UseCase useCase = UseCase::Auto;
    int seconds = 10;
    int top = 10;
    std::string error;
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                o.error = std::string("Missing value for ") + name;
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--test") o.test = true;
        else if (a == "--json") o.json = true;
        else if (a == "--help" || a == "-h" || a == "/?") o.help = true;
        else if (a == "--elevate") o.elevate = true;
        else if (a == "--elevated-child") o.elevatedChild = true;
        else if (a == "--scan") {
            o.scan = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.scanPath = argv[++i];
        } else if (a == "--usecase") {
            if (const char* v = needValue("--usecase")) {
                if (!parseUseCase(v, o.useCase)) o.error = "Unknown use case (use auto, office, gaming, creator, dev)";
            }
        } else if (a == "--seconds") {
            if (const char* v = needValue("--seconds")) {
                try { o.seconds = std::stoi(v); } catch (...) { o.error = "--seconds needs a number"; }
            }
        } else if (a == "--top") {
            if (const char* v = needValue("--top")) {
                try { o.top = std::stoi(v); } catch (...) { o.error = "--top needs a number"; }
            }
        } else {
            o.error = "Unknown option: " + a;
        }
        if (!o.error.empty()) break;
    }
    o.seconds = std::clamp(o.seconds, 1, 120);
    o.top = std::clamp(o.top, 1, 50);
    return o;
}

void printHelp() {
    std::cout <<
        "SystemMonitor - hardware rating, upgrade advice and live diagnosis\n\n"
        "  --usecase <auto|office|gaming|creator|dev>   what the device is mainly used for (default auto)\n"
        "  --seconds <1-120>   live sampling time, results are averaged (default 10)\n"
        "  --top <1-50>        how many top apps to list (default 10)\n"
        "  --scan [folder]     find the biggest folders (default <system drive>\\Users)\n"
        "  --json              machine-readable output for a back end / web API\n"
        "  --elevate           ask for Administrator rights (UAC) to inspect protected processes\n"
        "  --test              run the built-in unit tests\n";
}

int currentYear() {
    std::time_t t = std::time(nullptr);
    std::tm tm = {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm.tm_year + 1900;
}

const char* storageLabel(StorageType t) {
    switch (t) {
        case StorageType::SSD: return "SSD";
        case StorageType::HDD: return "HDD";
        default: return "unknown type";
    }
}

std::string humanBytes(unsigned long long b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    std::ostringstream o;
    o << std::fixed << std::setprecision(u == 0 ? 0 : 1) << v << " " << units[u];
    return o.str();
}

void printDevice(const HardwareInfo& hw, const Assessment& a) {
    std::cout << "===== Your device =====\n";
    std::cout << "Type    : " << (hw.isLaptop ? "Laptop" : "Desktop") << "\n";
    std::cout << "CPU     : " << hw.cpuName << " (" << hw.logicalProcessors << " threads)\n";
    if (a.cpu.recognized) {
        std::cout << "          " << a.cpu.family << ", " << a.cpu.generation;
        if (a.cpu.releaseYear > 0) std::cout << ", released ~" << a.cpu.releaseYear;
        if (!a.cpu.socket.empty()) std::cout << ", socket " << a.cpu.socket;
        std::cout << "\n";
    }
    for (const GpuInfo& g : hw.gpus) {
        std::cout << "GPU     : " << g.name << " (" << g.vramGB << " GB dedicated)\n";
    }
    if (hw.totalRamGB > 0.0) {
        std::cout << "RAM     : " << nominalRamGB(hw.totalRamGB) << " GB (" << hw.totalRamGB << " GB usable)";
        if (!hw.ramType.empty()) std::cout << ", " << hw.ramType;
        if (hw.ramSpeedMHz > 0) std::cout << " @ " << hw.ramSpeedMHz << " MT/s";
        if (hw.ramModules > 0) {
            std::cout << ", " << hw.ramModules << " module(s)";
            if (hw.ramSlotsTotal > 0) std::cout << " in " << hw.ramSlotsTotal << " slot(s)";
        }
        if (hw.ramLikelySoldered) std::cout << ", soldered";
        std::cout << "\n";
    } else {
        std::cout << "RAM     : unknown\n";
    }
    std::cout << "Storage : " << hw.systemDrive << " " << hw.diskTotalGB << " GB, " << hw.diskFreeGB << " GB free, "
              << storageLabel(hw.storageType) << "\n";
    std::cout << "Screen  : " << hw.screenWidth << "x" << hw.screenHeight;
    if (hw.refreshHz > 0) std::cout << " @ " << hw.refreshHz << " Hz";
    std::cout << " (" << a.screenLabel << ")\n";
}

void printRating(const Assessment& a) {
    std::cout << "\n===== Rating for: " << useCaseLabel(a.useCase) << " (an estimate, not a real benchmark) =====\n";
    std::cout << "Fit     : " << a.overallScore << "/100  ->  " << a.verdict << "\n";
    std::cout << "Fit for : Office " << a.fitByUseCase[kOffice] << " | Gaming " << a.fitByUseCase[kGaming]
              << " | Creator " << a.fitByUseCase[kCreator] << " | Developer " << a.fitByUseCase[kDeveloper] << "\n";
    std::cout << "Parts   : CPU " << a.cpuFit << " | GPU " << a.gpuFit << " | RAM " << a.ramFit << " | Storage "
              << a.storageFit << " | Screen " << a.screenFit << "   (85 = meets the need)\n";
    std::cout << "Raw power index: " << a.performanceIndex << "/100 (" << a.category << ")\n";
    if (a.yearsLeft >= 0) {
        std::cout << "Lifespan: about " << a.yearsLeft << " more comfortable year(s); first to run out: "
                  << a.limitingComponent << "\n";
    }
}

void printAdvice(const Assessment& a) {
    std::cout << "\n===== Status =====\n";
    for (const Advice& adv : a.advice) {
        std::cout << "[" << urgencyLabel(adv.urgency) << "] " << adv.component << ": " << adv.text << "\n";
    }
    std::cout << "\n===== What to do / buy =====\n";
    for (const Recommendation& r : a.recommendations) {
        std::cout << "[" << urgencyLabel(r.priority) << "] " << r.component << ": " << r.title << "\n";
        std::cout << "      " << r.detail << "\n";
        if (!r.examples.empty()) std::cout << "      e.g. " << r.examples << "\n";
        if (r.priceMaxUSD > 0) std::cout << "      ~$" << r.priceMinUSD << " - $" << r.priceMaxUSD;
        if (r.extraYears > 0) std::cout << (r.priceMaxUSD > 0 ? "  |  " : "      ") << "buys ~" << r.extraYears << " more year(s)";
        if (r.priceMaxUSD > 0 || r.extraYears > 0) std::cout << "\n";
    }
    std::cout << "(" << priceDisclaimer() << ")\n";
}

void printTop(const char* title, const std::vector<ProcessGroup>& g, bool byRam) {
    std::cout << title << "\n";
    for (const ProcessGroup& p : g) {
        std::cout << "  " << std::left << std::setw(32) << p.name << std::right;
        if (p.count > 1) std::cout << " x" << std::setw(3) << std::left << p.count << std::right;
        else std::cout << "     ";
        std::cout << std::setw(9) << p.ramMB << " MB  " << std::setw(6) << p.cpuPercent << " % CPU";
        if (p.anyRestricted) std::cout << "  (partly restricted)";
        std::cout << "\n";
    }
    (void)byRam;
}

int runScan(const Options& o, const HardwareInfo& hw) {
    const std::string root = o.scanPath.empty() ? hw.systemDrive + "\\Users" : o.scanPath;
    if (!o.json) std::cout << "Scanning " << root << " (up to 20 seconds)...\n";
    const ScanResult r = scanLargestFolders(root, static_cast<std::size_t>(o.top));
    if (o.json) {
        std::cout << toJson(r) << "\n";
        return r.error.empty() ? 0 : 1;
    }
    if (!r.error.empty()) {
        std::cout << r.error << "\n";
        return 1;
    }
    std::cout << "\n===== Biggest folders in " << root << " =====\n";
    for (const FolderSize& f : r.folders) {
        std::cout << std::setw(10) << humanBytes(f.bytes) << "  " << f.path << "\n";
    }
    std::cout << "Total seen: " << humanBytes(r.totalBytes) << " in " << std::fixed << std::setprecision(1) << r.seconds << " s";
    if (r.truncated) std::cout << "  (stopped early: real sizes are larger)";
    std::cout << "\nTip: run again with --scan <one of these folders> to look inside it.\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SystemMonitor::hardenProcess();
    SystemMonitor::enableUtf8Console();

    const Options opt = parseArgs(argc, argv);
    if (!opt.error.empty()) {
        std::cerr << opt.error << "\n\n";
        printHelp();
        return 2;
    }
    if (opt.help) { printHelp(); return 0; }
    if (opt.test) return runAnalyzerTests() == 0 ? 0 : 1;

    // Administrator rights are requested only when asked for (never forced).
    if (opt.elevate && !ProcessMonitor::isElevated()) {
        uint32_t err = 0;
        if (!ProcessMonitor::relaunchAsAdmin("--elevated-child", true, &err)) {
            std::cerr << (err == 1223 ? "Administrator request was cancelled; continuing without it.\n"
                                      : "Could not request Administrator rights; continuing without them.\n");
        }
    }

    const HardwareInfo hw = SystemMonitor::readHardwareInfo();
    if (opt.scan) return runScan(opt, hw);

    SystemMonitor monitor;   // takes the baseline for CPU / network
    const Assessment a = assess(hw, currentYear(), opt.useCase);

    if (!opt.json) {
        std::cout << std::fixed << std::setprecision(1);
        printDevice(hw, a);
        printRating(a);
        printAdvice(a);
        std::cout << "\n===== Live (" << opt.seconds << " seconds, averaged) =====\n" << std::flush;
    }

    // ---- sample live stats + processes, then average them ----
    ProcessMonitor procMon;
    procMon.listProcesses(false, 0);   // baseline for per-process CPU
    ProcessAverager averager;
    LiveStats sum;
    int taken = 0;
    std::size_t restricted = 0;
    for (int i = 0; i < opt.seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const LiveStats s = monitor.readStats();
        const std::vector<ProcessSnapshot> procs = procMon.listProcesses(false, 0);
        averager.add(groupProcesses(procs));
        restricted = 0;
        for (const ProcessSnapshot& p : procs) if (p.accessRestricted) ++restricted;

        sum.cpuPercent += s.cpuPercent;
        sum.ramPercent += s.ramPercent;
        sum.ramUsedGB += s.ramUsedGB;
        sum.diskUsedPercent += s.diskUsedPercent;
        sum.downKBps += s.downKBps;
        sum.upKBps += s.upKBps;
        ++taken;
        if (!opt.json) {
            std::cout << "CPU " << s.cpuPercent << "%  |  RAM " << s.ramPercent << "% (" << s.ramUsedGB
                      << " GB)  |  Disk used " << s.diskUsedPercent << "%  |  Down " << s.downKBps
                      << " KB/s  |  Up " << s.upKBps << " KB/s\n";
        }
    }
    LiveStats avg;
    if (taken > 0) {
        avg.cpuPercent = sum.cpuPercent / taken;
        avg.ramPercent = sum.ramPercent / taken;
        avg.ramUsedGB = sum.ramUsedGB / taken;
        avg.diskUsedPercent = sum.diskUsedPercent / taken;
        avg.downKBps = sum.downKBps / taken;
        avg.upKBps = sum.upKBps / taken;
    }
    const std::vector<ProcessGroup> groups = averager.averaged();
    const std::vector<ProcessGroup> byRam = topByRam(groups, static_cast<std::size_t>(opt.top));
    const std::vector<ProcessGroup> byCpu = topByCpu(groups, static_cast<std::size_t>(opt.top));
    const LiveDiagnosis diag = diagnoseLive(a, avg, byRam, byCpu);
    const bool elevated = ProcessMonitor::isElevated();

    if (opt.json) {
        std::cout << "{\"device\":" << toJson(hw) << ",\"assessment\":" << toJson(a)
                  << ",\"live\":" << toJson(avg) << ",\"diagnosis\":" << toJson(diag)
                  << ",\"topByRam\":" << toJson(byRam) << ",\"topByCpu\":" << toJson(byCpu)
                  << ",\"sampleSeconds\":" << taken << ",\"elevated\":" << (elevated ? "true" : "false")
                  << ",\"restrictedProcesses\":" << restricted << "}\n";
        return 0;
    }

    std::cout << "\nAverage: CPU " << avg.cpuPercent << "%  |  RAM " << avg.ramPercent << "% (" << avg.ramUsedGB
              << " GB)  |  Disk used " << avg.diskUsedPercent << "%\n";
    std::cout << "Diagnosis: " << diag.summary << "\n";
    for (const BottleneckFinding& f : diag.findings) {
        std::cout << "  [" << urgencyLabel(f.urgency) << "] " << f.title << "\n      " << f.detail << "\n      -> "
                  << f.action << "\n";
    }
    std::cout << "\n";
    printTop("===== Top apps by RAM (average) =====", byRam, true);
    std::cout << "\n";
    printTop("===== Top apps by CPU (average) =====", byCpu, false);

    // Offer Administrator mode only when it would reveal more, and only if the user agrees.
    if (!elevated && restricted > 0) {
        std::cout << "\n" << restricted << " process(es) could not be fully inspected without Administrator rights.\n"
                  << "Re-run as Administrator to include them? [y/N] " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (!answer.empty() && (answer[0] == 'y' || answer[0] == 'Y')) {
            uint32_t err = 0;
            if (!ProcessMonitor::relaunchAsAdmin("--elevated-child", true, &err)) {
                std::cout << (err == 1223 ? "Cancelled.\n" : "Could not start the elevated copy.\n");
            }
        }
    }
    if (opt.elevatedChild) {   // the elevated copy opens its own window: keep it open
        std::cout << "\nPress Enter to close...";
        std::string dummy;
        std::getline(std::cin, dummy);
    }
    return 0;
}
