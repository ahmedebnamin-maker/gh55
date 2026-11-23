// Upgrade / purchase recommendations. Pure C++ (no OS calls).
//
// PRICES: approximate US-dollar street prices checked around November 2026.
// The RAM / SSD / GPU markets are very volatile (AI-driven DRAM shortage), so
// every number is a rough range, not a quote. Update the constants below.
#include <algorithm>
#include <string>
#include <vector>

#include "analyzer.h"

namespace {

// --- RAM (approx. USD, Nov 2026). DDR4 16GB kit ~105-125, DDR5 16GB kit ~210-300. ---
struct Price { int lo, hi; };

Price ramPrice(const std::string& type, int totalGB) {
    const bool ddr5 = type.find("DDR5") != std::string::npos;
    const bool ddr3 = type == "DDR3";
    if (ddr3) {                       // legacy: mostly used market
        return totalGB <= 8 ? Price{20, 40} : Price{40, 80};
    }
    if (ddr5) {
        if (totalGB <= 8) return {100, 150};
        if (totalGB <= 16) return {210, 300};
        return {380, 480};
    }
    // DDR4 (also the default when the type is unknown)
    if (totalGB <= 8) return {45, 65};
    if (totalGB <= 16) return {95, 125};
    return {190, 230};
}

Price ssdPrice(int sizeGB) {          // NVMe/SATA, 2026 prices
    if (sizeGB <= 500) return {70, 100};
    if (sizeGB <= 1000) return {120, 165};
    return {190, 250};
}

void add(std::vector<Recommendation>& out, const char* component, Urgency u, std::string title,
         std::string detail, std::string examples, int lo, int hi, int extraYears) {
    Recommendation r;
    r.component = component;
    r.priority = u;
    r.title = std::move(title);
    r.detail = std::move(detail);
    r.examples = std::move(examples);
    r.priceMinUSD = lo;
    r.priceMaxUSD = hi;
    r.extraYears = extraYears;
    out.push_back(std::move(r));
}

int rank(Urgency u) { return u == Urgency::Now ? 2 : (u == Urgency::Soon ? 1 : 0); }

}  // namespace

const char* priceDisclaimer() {
    return "Approximate US-dollar street prices (late 2026). RAM, SSD and graphics-card prices are volatile "
           "right now, and local prices (e.g. in Egypt) differ: use these as a budget guide only.";
}

std::vector<Recommendation> buildRecommendations(const HardwareInfo& hw, const Assessment& a, int currentYear) {
    std::vector<Recommendation> out;
    const UseCase uc = a.useCase;
    const bool office = (uc == UseCase::Office);
    const bool gamer = (uc == UseCase::Gaming);
    const bool creator = (uc == UseCase::Creator);

    const bool ramKnown = hw.totalRamGB > 0.0;
    const int ramGB = ramKnown ? nominalRamGB(hw.totalRamGB) : 0;
    const std::string ramType = hw.ramType.empty() ? "DDR4" : hw.ramType;
    const bool typeGuessed = hw.ramType.empty();
    const bool soldered = hw.ramLikelySoldered;
    const int minGB = office ? 8 : 16;
    const int idealGB = (office || gamer) ? 16 : 32;
    const int freeSlots = (hw.ramSlotsTotal > 0 && hw.ramModules > 0) ? hw.ramSlotsTotal - hw.ramModules : -1;

    // ------------------------------------------------------------------ RAM
    if (ramKnown && ramGB < idealGB) {
        Urgency u = Urgency::Ok;
        if (ramGB <= 4) u = Urgency::Now;
        else if (ramGB <= 8) u = office ? Urgency::Soon : Urgency::Now;
        else if (ramGB < minGB || (!office && ramGB <= 12)) u = Urgency::Soon;
        const int target = (ramGB < minGB || office) ? 16 : idealGB;
        if (soldered) {
            add(out, "RAM", u, "RAM is soldered: it cannot be upgraded",
                std::string("This device uses on-board memory (") + ramType + "). The only fix is a new device, "
                "so when you buy one choose at least " + std::to_string(target) + " GB.",
                "", 0, 0, 0);
        } else {
            const int addGB = std::max(8, target - ramGB);
            std::string title, detail;
            if (hw.ramModules == 1 && freeSlots != 0) {
                title = "Add a second " + std::to_string(ramGB >= 8 ? ramGB : 8) + " GB stick (dual channel)";
                detail = "Matching a second stick to the existing one gives " + std::to_string(ramGB + (ramGB >= 8 ? ramGB : 8)) +
                         " GB and dual-channel speed. Use the same type and speed.";
            } else if (freeSlots > 0) {
                title = "Add " + std::to_string(addGB) + " GB in a free slot";
                detail = "You have a free memory slot. Use the same type and speed as the installed sticks.";
            } else {
                title = "Replace the sticks with a " + std::to_string(target) + " GB kit";
                detail = "The slots look full (or the slot count is unknown): swap in a larger matching kit.";
            }
            if (typeGuessed) detail += " Check your RAM type first (DDR3/DDR4/DDR5): the price assumes DDR4.";
            if (hw.isLaptop) detail += " Laptops use SODIMM sticks and some models have only one slot: check the model's manual.";
            const int buyGB = (hw.ramModules == 1 && ramGB >= 8) ? ramGB : addGB;
            const Price p = ramPrice(ramType, buyGB);
            add(out, "RAM", u, title, detail, ramType + (hw.isLaptop ? " SODIMM" : " DIMM") + ", same speed as installed",
                p.lo, p.hi, ramGB < minGB ? 2 : 1);
        }
    } else if (ramKnown && hw.ramModules == 1 && !soldered && ramGB >= 8) {
        const Price p = ramPrice(ramType, ramGB);
        add(out, "RAM", Urgency::Ok, "Add a matching stick for dual channel",
            "One stick runs in single-channel mode. A second identical stick can give a noticeable boost, "
            "especially with integrated graphics.",
            ramType + ", same size and speed", p.lo, p.hi, 0);
    }

    // -------------------------------------------------------------- Storage
    if (hw.storageType == StorageType::HDD) {
        const double used = std::max(0.0, hw.diskTotalGB - hw.diskFreeGB);
        const int size = used < 350.0 ? 500 : (used < 800.0 ? 1000 : 2000);
        const Price p = ssdPrice(size);
        add(out, "Storage", Urgency::Now, "Replace the system HDD with a " + std::to_string(size) + " GB SSD",
            "The single biggest speed-up for an older PC: boot and app start times drop from minutes to seconds. "
            "Clone the existing Windows install with the SSD maker's free cloning tool instead of reinstalling." +
            std::string(hw.isLaptop ? " Check whether the laptop takes a 2.5\" SATA or an M.2 drive." : ""),
            "Any reputable SATA or NVMe SSD (Crucial, Samsung, WD, Kingston)", p.lo, p.hi, 3);
    } else if (hw.diskTotalGB > 0.0 && hw.diskFreeGB / hw.diskTotalGB < 0.20) {
        const int size = hw.diskTotalGB <= 520.0 ? 1000 : 2000;   // at least double the current size
        const Price p = ssdPrice(size);
        const Urgency u = (hw.diskFreeGB / hw.diskTotalGB < 0.10) ? Urgency::Now : Urgency::Soon;
        add(out, "Storage", u, "Free up space (free), or move to a " + std::to_string(size) + " GB SSD",
            "Start free: remove unused apps, empty Downloads, and run Disk Cleanup. If it fills up again, a larger "
            "SSD is the permanent fix.",
            "Free cleanup first; then any NVMe/SATA SSD", p.lo, p.hi, 2);
    }

    // ------------------------------------------------------------------ CPU
    const bool cpuKnown = a.cpu.recognized;
    const int age = (cpuKnown && a.cpu.releaseYear > 0) ? std::max(0, currentYear - a.cpu.releaseYear) : 0;
    if (cpuKnown && a.cpuFit < 85) {
        const Urgency u = a.cpuFit < 60 ? Urgency::Now : Urgency::Soon;
        const std::string& s = a.cpu.socket;
        if (hw.isLaptop || a.cpu.mobile) {
            if (office) {
                add(out, "Device", u, "The laptop CPU is soldered: plan a new laptop",
                    "A modern Core i5 / Ryzen 5 laptop with 16 GB RAM and a 512 GB SSD covers office work for 6-8 years.",
                    "Core i5 13th gen+ / Core Ultra 5 / Ryzen 5 7000+ with 16 GB + 512 GB SSD", 500, 800, 6);
            } else if (gamer) {
                add(out, "Device", u, "The laptop CPU/GPU are soldered: plan a new gaming laptop",
                    "Look for an RTX 5050/5060-class GPU, a Ryzen 7 / Core i7 H-series CPU, 16 GB+ RAM and a 1 TB SSD.",
                    "RTX 5060 laptops with 16 GB + 1 TB", 1000, 1600, 5);
            } else {
                add(out, "Device", u, "The laptop CPU is soldered: plan a new workstation-class laptop",
                    "Look for 8+ performance-heavy cores (Ryzen 7/9 HS/HX or Core i7/Ultra 7 H), 32 GB RAM and a 1 TB SSD.",
                    "Ryzen 7 8845HS / Core Ultra 7 H class with 32 GB + 1 TB", 1100, 1800, 5);
            }
        } else {
            // Desktop: first the cheap in-socket path, then a platform rebuild.
            if (s == "AM4") {
                add(out, "CPU", u, "Drop-in CPU upgrade on your AM4 board",
                    "No new board or RAM needed (update the BIOS first on A320/B350/X370 boards).",
                    gamer ? "Ryzen 7 5800X3D / 5700X3D (best gaming)" : "Ryzen 5 5600 / Ryzen 7 5700X",
                    gamer ? 220 : 100, gamer ? 330 : 190, 3);
            } else if (s == "LGA1200") {
                add(out, "CPU", u, "Drop-in CPU upgrade on your LGA1200 board",
                    "A used or new Core i7/i9 of the 10th/11th gen fits your board. Gain is real but modest; "
                    "worth it only if the board and RAM are otherwise fine.",
                    "Core i7-10700 / i7-11700 / i9-11900", 110, 230, 2);
            } else if (s == "LGA1700") {
                add(out, "CPU", u, "Drop-in CPU upgrade on your LGA1700 board",
                    "12th-14th gen chips share the socket: a higher model gives a large gain without a new board.",
                    "Core i5-13600K / i5-14600K / i7-14700K", 200, 380, 3);
            } else if (s == "AM5") {
                add(out, "CPU", u, "Drop-in CPU upgrade on your AM5 board",
                    "AM5 is a long-life socket: a faster Ryzen drops straight in.",
                    "Ryzen 7 7800X3D / Ryzen 7 9700X", 280, 420, 4);
            } else if (s == "LGA1151" || s == "LGA1151 (v2)" || s == "LGA1150" || s == "LGA1155" || s == "LGA1156") {
                add(out, "CPU", Urgency::Soon, "Stretch plan: used CPU in the same socket (cheap, +1-2 years)",
                    "Your platform is old, so a used high-end chip of the same socket is the cheapest way to stretch "
                    "it for office-level work. Not recommended for gaming or heavy creation.",
                    s == "LGA1155" ? "Core i7-3770 (used)" : (s == "LGA1150" ? "Core i7-4790 (used)"
                                      : (s == "LGA1151" ? "Core i7-7700 (used)" : "Core i7-8700 / i7-9700 (used)")),
                    30, 140, 2);
            }
            // Platform options (shown for sockets without a good drop-in or when the gap is big).
            if (s.empty() || s == "LGA1151" || s == "LGA1151 (v2)" || s == "LGA1150" || s == "LGA1155" || s == "LGA1156" ||
                a.cpuFit < 60) {
                add(out, "CPU", u, "Budget platform rebuild (CPU + board + RAM, keep case/PSU/drives)",
                    "With DDR5 prices very high in 2026, a DDR4 platform is the value route: it gives a big jump for "
                    "much less money.",
                    "Ryzen 5 5600 + B550 board + 16 GB DDR4, or Core i5-12400F + B660 (DDR4) + 16 GB DDR4", 300, 450, 4);
                add(out, "CPU", Urgency::Ok, "Long-life platform rebuild (AM5)",
                    "Costs more today because of DDR5, but AM5 will accept new CPUs for years.",
                    "Ryzen 5 7600 / 9600X + B650 board + 16 GB DDR5", 520, 760, 6);
            }
        }
    } else if (cpuKnown && a.cpu.releaseYear > 0) {
        add(out, "CPU", Urgency::Ok, "No CPU upgrade needed",
            std::string("The ") + a.cpu.generation + " CPU (~" + std::to_string(age) + " years old) still meets your needs. "
            "Spend on RAM/SSD first if anything.",
            "", 0, 0, 0);
    }

    // ------------------------------------------------------------------ GPU
    if (gamer || creator) {
        const int target = gamer ? 60 : 45;
        if (a.gpuScore * 100 < target * 70) {   // clearly below what this use wants
            const Urgency u = a.gpuFit < 40 ? Urgency::Now : Urgency::Soon;
            std::string examples;
            int lo = 0, hi = 0;
            if (a.gpuScore < 35) { examples = "RTX 5050 / Arc B580 (new) or used RX 6600 / RTX 3060"; lo = 150; hi = 310; }
            else if (a.gpuScore < 55) { examples = "RTX 5060 / RX 9060 XT 8 GB"; lo = 280; hi = 360; }
            else { examples = "RX 9060 XT 16 GB / RTX 5060 Ti 16 GB"; lo = 440; hi = 560; }
            if (hw.isLaptop) {
                add(out, "Device", u, "The graphics chip is soldered: choose a laptop with a better GPU next time",
                    "Aim for RTX 5060-class or better with 8 GB of video memory.", "RTX 5060 laptop", 1000, 1600, 5);
            } else {
                add(out, "GPU", u, a.hasDiscreteGpu ? "Upgrade the graphics card" : "Add a dedicated graphics card",
                    "Before buying, check the power supply (450-550 W with the needed PCIe power connector) and that "
                    "the case has room. Prefer 8 GB of video memory or more; 16 GB if you play at high settings.",
                    examples, lo, hi, 3);
            }
        }
    }

    // ------------------------------------------------------------- Security
    if (a.cpu.win11Support == 0) {
        add(out, "Security", Urgency::Now, "This PC can no longer get Windows security updates",
            "The CPU is not on Microsoft's Windows 11 list and Windows 10 is out of support. For an internet-connected "
            "PC that is a real risk. Options: replace the platform (see the CPU options), or switch this PC to a "
            "supported Linux distribution for browsing and office work (free). Do not use it for banking or work accounts "
            "while it is unpatched.",
            "Linux Mint / Ubuntu LTS on old hardware, or a new platform", 0, 0, 0);
    }

    // -------------------------------------------------------------- Overall
    {
        std::string detail;
        if (a.yearsLeft >= 0) {
            detail = "For " + std::string(useCaseLabel(uc)) + " use this device should stay comfortable for roughly " +
                     std::to_string(a.yearsLeft) + " more year(s); the first part to run out is the " +
                     a.limitingComponent + ".";
        } else {
            detail = "Not enough information to estimate how long this device will last.";
        }
        const Urgency u = (a.yearsLeft >= 0 && a.yearsLeft <= 0) ? Urgency::Now
                         : (a.yearsLeft >= 0 && a.yearsLeft < 2 ? Urgency::Soon : Urgency::Ok);
        // Cheapest way to keep an aging device: fix RAM + storage, which are the parts you can change.
        int lo = 0, hi = 0;
        for (const Recommendation& r : out) {
            if (r.component == "RAM" || r.component == "Storage") { lo += r.priceMinUSD; hi += r.priceMaxUSD; }
        }
        if (lo > 0) detail += " The RAM/storage steps above cost about $" + std::to_string(lo) + "-$" + std::to_string(hi) +
                              " in total and are the cheapest way to extend its life.";
        add(out, "Overall", u, "How long to keep this device", detail, "", lo, hi, 0);
    }

    std::stable_sort(out.begin(), out.end(), [](const Recommendation& x, const Recommendation& y) {
        return rank(x.priority) > rank(y.priority);
    });
    return out;
}
