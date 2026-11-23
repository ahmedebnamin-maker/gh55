// Unit tests for analyzer.cpp. Run the program with the argument --test.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "analyzer.h"
#include "diagnosis.h"
#include "disk_scan.h"
#include "json_report.h"
#include "process_types.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL: " << what << "\n";
    }
}

bool hasAdvice(const Assessment& a, const char* component, Urgency u) {
    for (const Advice& x : a.advice) {
        if (x.component == component && x.urgency == u) return true;
    }
    return false;
}

HardwareInfo makeInfo(const char* cpu, double ramGB, double diskTotal, double diskFree,
                      StorageType st, int w, int h, int hz, bool laptop,
                      const char* gpuName, double vram) {
    HardwareInfo hw;
    hw.cpuName = cpu;
    hw.logicalProcessors = 8;
    hw.totalRamGB = ramGB;
    hw.diskTotalGB = diskTotal;
    hw.diskFreeGB = diskFree;
    hw.storageType = st;
    hw.screenWidth = w;
    hw.screenHeight = h;
    hw.refreshHz = hz;
    hw.isLaptop = laptop;
    hw.gpus.push_back({gpuName, vram});
    return hw;
}

void testCpuParsing() {
    CpuProfile p = parseCpuName("13th Gen Intel(R) Core(TM) i3-1315U");
    check(p.recognized && p.vendor == "Intel" && p.tier == 3, "i3-1315U basic fields");
    check(p.generation == "13th Gen" && p.releaseYear == 2023 && p.suffix == "U" && p.mobile, "i3-1315U gen/year(laptop 2023)/suffix");

    p = parseCpuName("Intel(R) Core(TM) i7-4770 CPU @ 3.40GHz");
    check(p.generation == "4th Gen" && p.releaseYear == 2013 && p.suffix.empty(), "i7-4770 (4 digits, no prefix)");

    p = parseCpuName("11th Gen Intel(R) Core(TM) i5-1135G7 @ 2.40GHz");
    check(p.generation == "11th Gen" && p.suffix == "G7" && p.releaseYear == 2020, "i5-1135G7");

    p = parseCpuName("Intel(R) Core(TM) i5-10210U CPU @ 1.60GHz");
    check(p.generation == "10th Gen" && p.suffix == "U", "i5-10210U (5 digits, no prefix)");

    p = parseCpuName("Intel(R) Core(TM) i7-1165G7");
    check(p.generation == "11th Gen", "i7-1165G7 without prefix is 11th gen, not 1st");

    p = parseCpuName("Intel(R) Core(TM) i7-920 CPU");
    check(p.generation == "1st Gen", "i7-920 is 1st gen");

    // Windows reports real 1st-gen CPUs WITHOUT a dash, so "i7-920" alone is not a realistic input.
    p = parseCpuName("Intel(R) Core(TM) i7 CPU         920  @ 2.67GHz");
    check(p.recognized && p.tier == 7 && p.generation == "1st Gen", "i7 CPU 920 (real 1st-gen string, no dash)");

    p = parseCpuName("Intel(R) Core(TM) i5 CPU       M 520  @ 2.40GHz");
    check(p.recognized && p.tier == 5 && p.generation == "1st Gen", "i5 CPU M 520 (1st-gen mobile)");

    p = parseCpuName("12th Gen Intel(R) Core(TM) i7-12700H");
    check(p.suffix == "H" && p.generation == "12th Gen", "i7-12700H");

    p = parseCpuName("AMD Ryzen 5 5600X 6-Core Processor");
    check(p.recognized && p.vendor == "AMD" && p.tier == 5 && p.suffix == "X", "Ryzen 5 5600X");
    check(p.generation == "Ryzen 5000 series" && p.releaseYear == 2020 && !p.mobile && p.socket == "AM4", "Ryzen 5600X: desktop 2020, AM4");

    p = parseCpuName("AMD Ryzen 7 7840HS w/ Radeon 780M Graphics");
    check(p.tier == 7 && p.suffix == "HS", "Ryzen 7 7840HS");

    p = parseCpuName("Intel(R) Core(TM) Ultra 7 155H");
    check(p.recognized && p.family == "Core Ultra 7" && p.suffix == "H" && p.releaseYear == 2023, "Core Ultra 7 155H");

    p = parseCpuName("AMD Ryzen AI 9 HX 370 w/ Radeon 890M");
    check(p.recognized && p.tier == 9 && p.family == "Ryzen AI 9", "Ryzen AI 9");
    check(p.releaseYear == 2024, "Ryzen AI 300 series launched in 2024");

    p = parseCpuName("AMD Ryzen AI Max+ 395 w/ Radeon 8060S");
    check(p.recognized && p.family == "Ryzen AI Max" && p.releaseYear == 2025, "Ryzen AI Max+ 395");

    p = parseCpuName("Intel(R) Pentium(R) Gold G5400 CPU @ 3.70GHz");
    check(p.recognized && p.vendor == "Intel" && p.family == "Pentium" && p.baseScore > 0, "Pentium Gold is recognized by its family");

    p = parseCpuName("");
    check(!p.recognized && p.vendor == "Unknown", "empty name does not crash");
}

void testRamAndGpu() {
    check(nominalRamGB(7.71783) == 8, "7.7 GB reported -> 8");
    check(nominalRamGB(15.8) == 16, "15.8 GB reported -> 16");
    check(nominalRamGB(3.9) == 4, "3.9 GB reported -> 4");
    check(nominalRamGB(31.7) == 32, "31.7 GB reported -> 32");

    GpuProfile g = lookupGpu("NVIDIA GeForce RTX 3060 Ti");
    check(g.discrete && g.score == 68, "3060 Ti matches before 3060");

    g = lookupGpu("NVIDIA GeForce RTX 3060 Laptop GPU");
    check(g.discrete && g.score == 49, "3060 laptop gets a penalty");

    g = lookupGpu("AMD Radeon RX 5700 XT");
    check(g.score == 56, "rx 5700 does not match rx 570");

    g = lookupGpu("Intel(R) UHD Graphics");
    check(!g.discrete && g.score == 10, "UHD is integrated");

    g = lookupGpu("Intel(R) Arc(TM) Graphics");
    check(!g.discrete && g.score == 28, "Arc Graphics is integrated");

    g = lookupGpu("Intel(R) Arc(TM) A770 Graphics");
    check(g.discrete && g.score == 60, "Arc A770 is discrete");

    g = lookupGpu("AMD Radeon(TM) 780M Graphics");
    check(!g.discrete && g.score == 32, "Radeon 780M is integrated");

    g = lookupGpu("AMD Radeon(TM) RX Vega 11 Graphics");
    check(!g.discrete && g.score == 22, "RX Vega 11 (Ryzen 2400G iGPU) is integrated, not a card");

    g = lookupGpu("AMD Radeon RX Vega 56");
    check(g.discrete, "RX Vega 56 is a real discrete card");

    g = lookupGpu("NVIDIA Some Future Card");
    check(g.discrete && !g.known && g.score == 45, "unknown NVIDIA card falls back to a discrete guess");

    g = lookupGpu("Totally Unknown Adapter");
    check(!g.discrete && g.score == 12, "unknown adapter falls back to integrated guess");
}

void testAssessment() {
    // Ahmed's own laptop (numbers from his screenshot, SSD assumed).
    HardwareInfo hw = makeInfo("13th Gen Intel(R) Core(TM) i3-1315U", 7.71783, 237.149, 42.0495,
                               StorageType::SSD, 1920, 1080, 60, true, "Intel(R) UHD Graphics", 0.125);
    Assessment a = assess(hw, 2026);
    check(a.cpuScore == 26, "laptop: cpu score 26 (13th gen laptop chip launched 2023)");
    check(a.overallScore >= 70 && a.overallScore <= 80, "laptop: office fit about 75 (8 GB limits it)");
    check(a.performanceIndex >= 20 && a.performanceIndex <= 30, "laptop: raw power index stays low (about 25)");
    check(a.category == "Basic office use", "laptop: basic office");
    check(!a.hasDiscreteGpu, "laptop: no discrete GPU");
    check(hasAdvice(a, "RAM", Urgency::Soon), "laptop: RAM advice is SOON");
    check(hasAdvice(a, "Storage", Urgency::Soon), "laptop: low free space is SOON (17.7% free)");
    check(hasAdvice(a, "CPU", Urgency::Ok), "laptop: CPU still OK");

    // A typical mid-range gaming desktop.
    hw = makeInfo("AMD Ryzen 5 5600X 6-Core Processor", 15.8, 931.0, 500.0, StorageType::SSD,
                  1920, 1080, 144, false, "NVIDIA GeForce RTX 3060", 12.0);
    a = assess(hw, 2026);
    check(a.hasDiscreteGpu && a.gpuScore == 57, "gaming: RTX 3060 detected");
    check(a.category == "Gaming", "gaming: category Gaming");
    check(hasAdvice(a, "RAM", Urgency::Ok), "gaming: 16 GB RAM is OK");

    // Strong CPU, 64 GB RAM, 4K screen but NO discrete GPU: capped.
    hw = makeInfo("Intel(R) Core(TM) i9-14900K", 63.7, 1800.0, 900.0, StorageType::SSD,
                  3840, 2160, 60, false, "Intel(R) UHD Graphics 770", 0.125);
    a = assess(hw, 2026);
    check(a.overallScore >= 55, "no-GPU: overall would reach gaming level");
    check(a.category == "Strong office / light gaming", "no-GPU: category is capped");

    // Old machine with an HDD, 4 GB RAM and almost no free space.
    hw = makeInfo("Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz", 3.9, 465.0, 30.0, StorageType::HDD,
                  1366, 768, 60, false, "Intel(R) HD Graphics 4600", 0.1);
    a = assess(hw, 2026);
    check(hasAdvice(a, "Storage", Urgency::Now), "old: HDD or full disk -> NOW");
    check(hasAdvice(a, "RAM", Urgency::Now), "old: 4 GB RAM -> NOW");
    check(hasAdvice(a, "CPU", Urgency::Now), "old: 13-year-old CPU -> NOW");
    check(a.category == "Basic office use", "old: basic office");

    // Ryzen APU whose iGPU is called "RX Vega 11": must NOT count as a discrete GPU.
    hw = makeInfo("AMD Ryzen 5 2400G with Radeon Vega Graphics", 15.8, 465.0, 300.0, StorageType::SSD,
                  1920, 1080, 60, false, "AMD Radeon(TM) RX Vega 11 Graphics", 2.0);
    a = assess(hw, 2026);
    check(!a.hasDiscreteGpu, "APU: RX Vega 11 is not a discrete GPU");
    check(a.category == "Strong office / light gaming", "APU: capped below gaming");

    // Laptop GTX 10-series has no "Laptop" in its name: the penalty comes from the device type.
    hw = makeInfo("Intel(R) Core(TM) i7-7700HQ CPU @ 2.80GHz", 15.8, 931.0, 500.0, StorageType::SSD,
                  1920, 1080, 60, true, "NVIDIA GeForce GTX 1060", 6.0);
    a = assess(hw, 2026);
    check(a.gpuScore == 32, "laptop GTX 1060: 40 - 8 laptop penalty");
    hw.isLaptop = false;
    a = assess(hw, 2026);
    check(a.gpuScore == 40, "desktop GTX 1060 keeps 40");

    // A name that already says "Laptop GPU" must be penalised only once.
    hw = makeInfo("13th Gen Intel(R) Core(TM) i7-13700H", 15.8, 931.0, 500.0, StorageType::SSD,
                  1920, 1080, 60, true, "NVIDIA GeForce RTX 3060 Laptop GPU", 6.0);
    a = assess(hw, 2026);
    check(a.gpuScore == 49, "RTX 3060 Laptop GPU: penalty applied once (57 - 8)");

    // Windows installed on another drive: advice names that drive, not a hard-coded C:.
    hw = makeInfo("Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz", 15.8, 465.0, 30.0, StorageType::SSD,
                  1920, 1080, 60, false, "Intel(R) UHD Graphics", 0.125);
    hw.systemDrive = "D:";
    a = assess(hw, 2026);
    bool namesD = false;
    for (const Advice& x : a.advice) {
        if (x.component == "Storage" && x.text.find("drive D:") != std::string::npos) namesD = true;
    }
    check(namesD, "low-space advice names the real system drive (D:)");

    // No GPU found at all must not crash.
    hw = makeInfo("Weird CPU", 0.0, 0.0, 0.0, StorageType::Unknown, 0, 0, 0, false, "x", 0.0);
    hw.gpus.clear();
    a = assess(hw, 2026);
    check(a.primaryGpu == "Unknown" && a.cpuScore == 40, "empty hardware info does not crash");
}


void testDesktopFairness() {
    // The user's own desktop: i5-10400, 12 GB RAM, integrated graphics. It used to get 36/100.
    HardwareInfo hw = makeInfo("Intel(R) Core(TM) i5-10400 CPU @ 2.90GHz", 11.8, 465.0, 300.0, StorageType::SSD,
                               1920, 1080, 60, false, "Intel(R) UHD Graphics 630", 0.125);
    Assessment a = assess(hw, 2026);
    check(a.cpu.generation == "10th Gen" && a.cpu.releaseYear == 2020 && !a.cpu.mobile, "i5-10400: desktop 10th gen, 2020");
    check(a.cpu.socket == "LGA1200", "i5-10400: socket LGA1200");
    check(a.useCase == UseCase::Office, "auto use case: no dedicated GPU -> office");
    check(a.overallScore >= 85, "i5-10400 desktop is a good office PC (fit >= 85)");
    check(a.fitByUseCase[kGaming] < 55, "same PC is weak for gaming (no GPU)");
    check(a.performanceIndex < 45, "raw power index is still honest about the missing GPU");

    // Same machine judged for gaming explicitly.
    a = assess(hw, 2026, UseCase::Gaming);
    check(a.useCase == UseCase::Gaming && a.overallScore == a.fitByUseCase[kGaming], "explicit use case is honoured");
    bool gpuRec = false;
    for (const Recommendation& r : a.recommendations) if (r.component == "GPU" && r.priceMaxUSD > 0) gpuRec = true;
    check(gpuRec, "gaming: no GPU -> a graphics card recommendation with a price");

    // Dedicated GPU -> auto picks gaming.
    hw = makeInfo("AMD Ryzen 5 5600X 6-Core Processor", 15.8, 931.0, 500.0, StorageType::SSD,
                  1920, 1080, 144, false, "NVIDIA GeForce RTX 3060", 12.0);
    a = assess(hw, 2026);
    check(a.useCase == UseCase::Gaming && a.overallScore >= 70, "RTX 3060 desktop auto -> gaming, decent fit");

    UseCase u = UseCase::Auto;
    check(parseUseCase("DEV", u) && u == UseCase::Developer, "parseUseCase dev");
    check(!parseUseCase("banana", u), "parseUseCase rejects junk");
}

void testCpuTables() {
    CpuProfile p = parseCpuName("Intel(R) Core(TM) i5-10400 CPU @ 2.90GHz");
    check(p.releaseYear == 2020 && p.socket == "LGA1200", "10th gen desktop = 2020");
    p = parseCpuName("Intel(R) Core(TM) i7-10510U CPU @ 1.80GHz");
    check(p.releaseYear == 2019 && p.mobile && p.socket.empty(), "10th gen laptop = 2019, no socket");
    p = parseCpuName("AMD Ryzen 5 5500U with Radeon Graphics");
    check(p.mobile && p.releaseYear == 2021, "Ryzen 5500U is a 2021 laptop chip");
    p = parseCpuName("AMD Ryzen 7 7800X3D 8-Core Processor");
    check(p.socket == "AM5" && p.suffix == "X3D" && !p.mobile, "7800X3D: AM5 desktop");
    p = parseCpuName("Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz");
    check(p.win11Support == 0 && p.socket == "LGA1150", "i5-4590: not Windows 11 capable, LGA1150");
    p = parseCpuName("Intel(R) Core(TM) i5-8400 CPU @ 2.80GHz");
    check(p.win11Support == 1 && p.socket == "LGA1151 (v2)", "i5-8400: Windows 11 capable");
    p = parseCpuName("Intel(R) Celeron(R) N4020 CPU @ 1.10GHz");
    check(p.recognized && p.family == "Celeron", "Celeron recognized");
    check(assess(makeInfo("Intel(R) Celeron(R) N4020 CPU @ 1.10GHz", 3.9, 64, 20, StorageType::SSD, 1366, 768, 60, true,
                          "Intel(R) UHD Graphics 600", 0.1), 2026).cpuScore < 25, "Celeron scores low");
    GpuProfile g = lookupGpu("NVIDIA GeForce GTX 960");
    check(g.known && g.discrete && g.score == 22, "GTX 960 is in the table (no longer a 45 guess)");
    g = lookupGpu("NVIDIA GeForce GTX 1650 SUPER");
    check(g.score == 36, "GTX 1650 SUPER matches before GTX 1650");
}

bool hasRec(const Assessment& a, const char* component) {
    for (const Recommendation& r : a.recommendations) if (r.component == component) return true;
    return false;
}

void testRecommendations() {
    // Old PC: HDD, 4 GB RAM, Haswell, no Windows 11.
    HardwareInfo hw = makeInfo("Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz", 3.9, 465.0, 300.0, StorageType::HDD,
                               1366, 768, 60, false, "Intel(R) HD Graphics 4600", 0.1);
    hw.ramType = "DDR3";
    hw.ramModules = 1;
    hw.ramSlotsTotal = 2;
    Assessment a = assess(hw, 2026);
    check(hasRec(a, "Storage") && hasRec(a, "RAM") && hasRec(a, "Security") && hasRec(a, "Overall"),
          "old PC: storage, RAM, security and overall recommendations");
    check(!a.recommendations.empty() && a.recommendations.front().priority == Urgency::Now, "most urgent first");
    for (const Recommendation& r : a.recommendations) {
        check(r.priceMinUSD <= r.priceMaxUSD, "price range is ordered");
    }
    check(a.yearsLeft == 0 && a.limitingComponent == "CPU", "old PC: 0 comfortable years, CPU is the limit");

    // Soldered RAM must never be told to "add a stick".
    hw = makeInfo("13th Gen Intel(R) Core(TM) i3-1315U", 7.7, 237.0, 100.0, StorageType::SSD, 1920, 1080, 60, true,
                  "Intel(R) UHD Graphics", 0.1);
    hw.ramLikelySoldered = true;
    hw.ramType = "LPDDR5";
    a = assess(hw, 2026);
    bool addStick = false, solderedNote = false;
    for (const Recommendation& r : a.recommendations) {
        if (r.component == "RAM" && r.title.find("Add") != std::string::npos) addStick = true;
        if (r.component == "RAM" && r.title.find("soldered") != std::string::npos) solderedNote = true;
    }
    check(!addStick && solderedNote, "soldered RAM: explained, not 'add a stick'");

    // Healthy modern desktop: no CPU purchase pushed.
    hw = makeInfo("AMD Ryzen 5 5600X 6-Core Processor", 31.8, 931.0, 500.0, StorageType::SSD, 1920, 1080, 144, false,
                  "NVIDIA GeForce RTX 3060", 12.0);
    a = assess(hw, 2026);
    bool cpuBuy = false;
    for (const Recommendation& r : a.recommendations) if (r.component == "CPU" && r.priceMaxUSD > 0) cpuBuy = true;
    check(!cpuBuy, "healthy PC: no CPU purchase recommended");

    // Drop-in upgrade names the real socket.
    hw = makeInfo("Intel(R) Core(TM) i3-10100 CPU @ 3.60GHz", 15.8, 931.0, 500.0, StorageType::SSD, 1920, 1080, 60, false,
                  "NVIDIA GeForce GTX 1060", 6.0);
    a = assess(hw, 2026, UseCase::Creator);
    bool lga1200 = false;
    for (const Recommendation& r : a.recommendations) if (r.title.find("LGA1200") != std::string::npos) lga1200 = true;
    check(lga1200, "creator on LGA1200: drop-in upgrade is suggested");
    check(std::string(priceDisclaimer()).find("Approximate") != std::string::npos, "price disclaimer exists");
}

void testProcessesAndDiagnosis() {
    std::vector<ProcessSnapshot> procs;
    auto mk = [](unsigned pid, const char* name, double ram, double cpu, bool restricted = false) {
        ProcessSnapshot p;
        p.pid = pid; p.name = name; p.ramMB = ram; p.cpuPercent = cpu; p.accessRestricted = restricted;
        return p;
    };
    procs.push_back(mk(1, "chrome.exe", 300, 2));
    procs.push_back(mk(2, "Chrome.exe", 500, 3));
    procs.push_back(mk(3, "code.exe", 700, 1));
    procs.push_back(mk(4, "svchost.exe", 20, 0, true));
    auto groups = groupProcesses(procs);
    check(groups.size() == 3, "chrome.exe / Chrome.exe are grouped (case-insensitive)");
    auto r = topByRam(groups, 2);
    check(r.size() == 2 && r[0].count == 2 && r[0].ramMB == 800, "top by RAM: Chrome x2 = 800 MB first");
    check(topByCpu(groups, 1)[0].cpuPercent == 5, "top by CPU sums the group");
    bool restricted = false;
    for (const auto& g : groups) if (g.name == "svchost.exe" && g.anyRestricted) restricted = true;
    check(restricted, "restricted flag is kept");
    check(sanitizeProcessName("a\x1b[31mb\n") == "a?[31mb?", "control characters are removed from names");

    // Averaging: a one-second spike must not dominate.
    ProcessAverager avg;
    for (int i = 0; i < 10; ++i) {
        std::vector<ProcessGroup> s;
        ProcessGroup g; g.name = "spike.exe"; g.count = 1; g.cpuPercent = (i == 0) ? 100.0 : 0.0; g.ramMB = 100;
        s.push_back(g);
        avg.add(s);
    }
    auto ag = avg.averaged();
    check(ag.size() == 1 && ag[0].cpuPercent == 10.0, "10-second average: one 100% spike -> 10%");
    ProcessAverager avg2;
    ProcessGroup brief; brief.name = "brief.exe"; brief.count = 1; brief.ramMB = 100;
    avg2.add({brief});
    avg2.add({});
    avg2.add({});
    avg2.add({});
    check(avg2.averaged()[0].ramMB == 25.0, "a process present in 1 of 4 samples averages 25 MB");

    // Diagnosis
    Assessment a = assess(makeInfo("Intel(R) Core(TM) i5-10400 CPU @ 2.90GHz", 15.8, 465, 300, StorageType::SSD, 1920, 1080,
                                   60, false, "Intel(R) UHD Graphics 630", 0.1), 2026);
    LiveStats live;
    live.cpuPercent = 95; live.ramPercent = 50;
    LiveDiagnosis d = diagnoseLive(a, live, {}, r);
    check(d.primary == BottleneckKind::Cpu && !d.findings.empty(), "95% CPU -> CPU bottleneck");
    check(d.findings[0].detail.find("Chrome.exe") != std::string::npos || d.findings[0].detail.find("chrome.exe") != std::string::npos
          || d.findings[0].detail.find("Biggest users") != std::string::npos, "diagnosis names the heaviest apps");
    live = LiveStats();
    live.downKBps = 80000;   // fast download: informational, not a fault
    d = diagnoseLive(a, live);
    check(d.primary == BottleneckKind::None && d.findings.size() == 1, "fast download alone is not a bottleneck");
    live = LiveStats();
    live.ramPercent = 93;
    d = diagnoseLive(a, live);
    check(d.primary == BottleneckKind::Memory && d.findings[0].urgency == Urgency::Now, "93% RAM -> critical memory");
}

void testJsonAndScan() {
    check(jsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001", "jsonEscape handles quotes, backslash, control chars");
    Assessment a = assess(makeInfo("Intel(R) Core(TM) i5-10400 CPU @ 2.90GHz", 11.8, 465, 300, StorageType::SSD, 1920, 1080, 60,
                                   false, "Intel(R) UHD Graphics 630", 0.1), 2026);
    const std::string j = toJson(a);
    check(!j.empty() && j.front() == '{' && j.back() == '}', "assessment JSON is an object");
    check(j.find("\"recommendations\":[") != std::string::npos && j.find("\"fitByUseCase\"") != std::string::npos,
          "assessment JSON has recommendations and per-use-case fit");
    int braces = 0, brackets = 0; bool inStr = false;
    for (size_t i = 0; i < j.size(); ++i) {
        const char c = j[i];
        if (inStr) { if (c == '\\') ++i; else if (c == '"') inStr = false; continue; }
        if (c == '"') inStr = true;
        else if (c == '{') ++braces; else if (c == '}') --braces;
        else if (c == '[') ++brackets; else if (c == ']') --brackets;
    }
    check(braces == 0 && brackets == 0 && !inStr, "assessment JSON is balanced");

    // Disk scan on a temporary tree.
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "sysmon_scan_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / "big" / "sub", ec);
    fs::create_directories(base / "small", ec);
    auto writeBytes = [](const fs::path& p, size_t n) { std::ofstream f(p, std::ios::binary); f << std::string(n, 'x'); };
    writeBytes(base / "big" / "a.bin", 5000);
    writeBytes(base / "big" / "sub" / "b.bin", 3000);
    writeBytes(base / "small" / "c.bin", 100);
    writeBytes(base / "loose.bin", 50);
    ScanResult s = scanLargestFolders(base.string(), 5, 10.0);
    check(s.error.empty() && !s.truncated, "scan: no error");
    check(s.folders.size() == 3 && s.folders[0].bytes == 8000 && s.folders[0].files == 2, "scan: biggest folder first, nested files counted");
    check(s.totalBytes == 8150, "scan: total includes loose files");
    s = scanLargestFolders((base / "does_not_exist").string());
    check(!s.error.empty(), "scan: missing folder reports an error, no crash");
    fs::remove_all(base, ec);
}

}  // namespace

// Returns the number of failed checks (0 = everything passed).
int runAnalyzerTests() {
    g_checks = 0;
    g_failures = 0;
    testCpuParsing();
    testRamAndGpu();
    testAssessment();
    testDesktopFairness();
    testCpuTables();
    testRecommendations();
    testProcessesAndDiagnosis();
    testJsonAndScan();
    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures;
}
