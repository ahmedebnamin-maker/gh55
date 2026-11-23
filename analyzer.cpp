#include "analyzer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <regex>
#include <sstream>

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::string num1(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << v;
    return o.str();
}

// Lowercase and remove "(TM)" / "(R)" so "Radeon(TM) 780M" == "Radeon 780M".
std::string normalizeName(std::string s) {
    s = toLower(std::move(s));
    for (const char* junk : {"(tm)", "(r)"}) {
        size_t pos;
        while ((pos = s.find(junk)) != std::string::npos) s.erase(pos, std::strlen(junk));
    }
    return s;
}

// True if `token` is in `text` and NOT directly followed by a digit.
// Bug this prevents: "rx 570" wrongly matching inside "rx 5700".
bool containsToken(const std::string& text, const std::string& token) {
    size_t pos = text.find(token);
    while (pos != std::string::npos) {
        size_t end = pos + token.size();
        if (end >= text.size() || !std::isdigit(static_cast<unsigned char>(text[end]))) return true;
        pos = text.find(token, pos + 1);
    }
    return false;
}

std::string ordinal(int n) {
    const char* suffix = "th";
    const int mod100 = n % 100;
    if (mod100 < 11 || mod100 > 13) {
        switch (n % 10) {
            case 1: suffix = "st"; break;
            case 2: suffix = "nd"; break;
            case 3: suffix = "rd"; break;
            default: break;
        }
    }
    return std::to_string(n) + suffix;
}

// ---------------------------------------------------------------------------
// GPU lookup table.  Scores and years are APPROXIMATE (rough relative gaming
// performance). Extend it freely. Longer names must come BEFORE shorter ones
// ("3060 ti" before "3060"), because the first match wins.
// ---------------------------------------------------------------------------
struct GpuEntry {
    const char* key;   // lowercase, without (TM)/(R)
    int score;
    int year;
    bool integrated;
};

const GpuEntry kGpuTable[] = {
    // NVIDIA RTX 50
    {"rtx 5090", 100, 2025, false}, {"rtx 5080", 96, 2025, false},
    {"rtx 5070 ti", 90, 2025, false}, {"rtx 5070", 84, 2025, false},
    {"rtx 5060 ti", 75, 2025, false}, {"rtx 5060", 68, 2025, false},
    {"rtx 5050", 58, 2025, false},
    // NVIDIA RTX 40
    {"rtx 4090", 98, 2022, false}, {"rtx 4080", 92, 2022, false},
    {"rtx 4070 ti", 84, 2023, false}, {"rtx 4070", 78, 2023, false},
    {"rtx 4060 ti", 66, 2023, false}, {"rtx 4060", 60, 2023, false},
    {"rtx 4050", 52, 2023, false},
    // NVIDIA RTX 30
    {"rtx 3090", 88, 2020, false}, {"rtx 3080", 84, 2020, false},
    {"rtx 3070", 76, 2020, false}, {"rtx 3060 ti", 68, 2020, false},
    {"rtx 3060", 57, 2021, false}, {"rtx 3050", 42, 2022, false},
    // NVIDIA RTX 20
    {"rtx 2080", 70, 2018, false}, {"rtx 2070", 62, 2018, false},
    {"rtx 2060", 52, 2019, false},
    // NVIDIA GTX
    {"gtx 1660", 42, 2019, false}, {"gtx 1650 super", 36, 2019, false}, {"gtx 1650", 32, 2019, false},
    {"gtx 1080", 58, 2016, false}, {"gtx 1070", 50, 2016, false},
    {"gtx 1060", 40, 2016, false}, {"gtx 1050", 26, 2016, false},
    {"gtx 980", 42, 2014, false}, {"gtx 970", 34, 2014, false}, {"gtx 960", 22, 2015, false},
    {"gtx 780", 32, 2013, false}, {"gtx 770", 28, 2013, false}, {"gtx 750", 14, 2014, false},
    {"gt 1030", 12, 2017, false}, {"gt 730", 6, 2014, false}, {"gt 710", 5, 2014, false},
    // AMD Radeon RX
    {"rx 9070 xt", 88, 2025, false}, {"rx 9070", 82, 2025, false},
    {"rx 9060 xt", 66, 2025, false}, {"rx 9060", 56, 2025, false},
    {"rx 7900 xtx", 92, 2022, false}, {"rx 7900 xt", 86, 2022, false},
    {"rx 7800 xt", 78, 2023, false}, {"rx 7700 xt", 70, 2023, false},
    {"rx 7600 xt", 58, 2024, false}, {"rx 7600", 54, 2023, false},
    {"rx 6950 xt", 82, 2022, false}, {"rx 6900 xt", 78, 2020, false}, {"rx 6800", 72, 2020, false},
    {"rx 6750 xt", 66, 2022, false}, {"rx 6700 xt", 64, 2021, false},
    {"rx 6650 xt", 55, 2022, false}, {"rx 6600 xt", 54, 2021, false},
    {"rx 6600", 50, 2021, false}, {"rx 6500", 28, 2022, false}, {"rx 6400", 22, 2022, false},
    {"rx 5700", 56, 2019, false}, {"rx 5600", 48, 2020, false}, {"rx 5500", 36, 2019, false},
    {"rx vega 64", 55, 2017, false}, {"rx vega 56", 50, 2017, false},
    {"rx 590", 36, 2018, false},
    {"rx 580", 34, 2017, false}, {"rx 570", 30, 2017, false}, {"rx 560", 20, 2017, false},
    {"rx 480", 32, 2016, false}, {"rx 470", 28, 2016, false},
    // Intel Arc (discrete)
    {"arc a770", 60, 2022, false}, {"arc a750", 54, 2022, false},
    {"arc a580", 46, 2023, false}, {"arc a380", 24, 2022, false}, {"arc a310", 16, 2022, false},
    {"arc b580", 58, 2024, false}, {"arc b570", 52, 2025, false},
    // Integrated graphics
    {"arc 140v", 36, 2024, true}, {"arc 130v", 32, 2024, true},
    {"arc graphics", 28, 2023, true},
    {"radeon 890m", 40, 2024, true}, {"radeon 880m", 36, 2024, true},
    {"radeon 780m", 32, 2023, true}, {"radeon 760m", 28, 2023, true},
    {"radeon 680m", 28, 2022, true}, {"radeon 660m", 22, 2022, true},
    {"rx vega 11", 22, 2018, true},   // iGPU of Ryzen 2400G/3400G: the name says "RX" but it is NOT a card
    {"radeon vega", 15, 0, true}, {"radeon graphics", 18, 0, true},
    {"iris xe", 20, 2020, true}, {"iris plus", 14, 2019, true},
    {"uhd graphics", 10, 0, true}, {"hd graphics", 7, 0, true},
};

// Laptop drivers usually (not always) write "Laptop GPU" / "Max-Q" in the name.
bool hasLaptopMarker(const std::string& normalizedName) {
    return normalizedName.find("laptop") != std::string::npos ||
           normalizedName.find("max-q") != std::string::npos;
}

bool isDiscreteByName(const std::string& n) {
    static const char* const keys[] = {"nvidia", "geforce", "rtx", "gtx", "quadro",
                                       " rx ", "radeon pro", "arc a", "arc b"};
    for (const char* k : keys) {
        if (n.find(k) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// CPU helpers
// ---------------------------------------------------------------------------
// Launch year of an Intel Core generation. Laptop and desktop chips of the same
// generation often launched in different years (10th gen: laptops 2019, desktops 2020).
int intelGenYear(int gen, bool mobile) {
    //                                gen: 0  1     2     3     4     5     6     7     8     9     10    11    12    13    14
    static const int desktop[] =          {0, 2010, 2011, 2012, 2013, 2015, 2015, 2017, 2017, 2018, 2020, 2021, 2021, 2022, 2023};
    static const int laptop[] =           {0, 2010, 2011, 2012, 2013, 2015, 2015, 2017, 2017, 2019, 2019, 2020, 2022, 2023, 2024};
    if (gen >= 1 && gen <= 14) return mobile ? laptop[gen] : desktop[gen];
    if (gen > 14 && gen < 30) return 2023 + (gen - 14);
    return 0;
}

bool intelMobileSuffix(const std::string& s) {
    if (s.empty()) return false;
    const char c = s[0];
    return c == 'U' || c == 'H' || c == 'P' || c == 'Y' || c == 'G' || c == 'M';
}

std::string intelDesktopSocket(int gen) {
    switch (gen) {
        case 1: return "LGA1156";
        case 2: case 3: return "LGA1155";
        case 4: case 5: return "LGA1150";
        case 6: case 7: return "LGA1151";
        case 8: case 9: return "LGA1151 (v2)";
        case 10: case 11: return "LGA1200";
        case 12: case 13: case 14: return "LGA1700";
        default: return "";
    }
}

int ryzenSeriesYear(int firstDigit, bool mobile) {
    // approximate (+/- 1 year)
    if (mobile) {
        switch (firstDigit) {
            case 1: return 2017; case 2: return 2018; case 3: return 2019; case 4: return 2020;
            case 5: return 2021; case 6: return 2022; case 7: return 2023; case 8: return 2024;
            case 9: return 2025; default: return 0;
        }
    }
    switch (firstDigit) {
        case 1: return 2017; case 2: return 2018; case 3: return 2019; case 4: return 2022;
        case 5: return 2020; case 6: return 2022; case 7: return 2022; case 8: return 2024;
        case 9: return 2024; default: return 0;
    }
}

int tierBase(int tier) {
    switch (tier) {
        case 3: return 45;
        case 5: return 65;
        case 7: return 80;
        case 9: return 95;
        default: return 50;
    }
}

// Low-power chips are slower than their tier suggests; H/HX/X3D are faster.
int powerAdjust(const CpuProfile& p) {
    const std::string& s = p.suffix;
    if (s.find("X3D") != std::string::npos) return 10;
    if (startsWith(s, "HX")) return 8;
    if (startsWith(s, "H")) return 3;
    if (startsWith(s, "P") || startsWith(s, "V")) return -3;
    if (startsWith(s, "U")) return -10;
    if (startsWith(s, "Y")) return -15;
    if (p.vendor == "Intel" && startsWith(s, "G") && p.mobile) return -8;   // 10th/11th gen mobile (G1..G7)
    if (!p.mobile && s == "T") return -5;                                   // low-power desktop chips
    return 0;   // desktop chips
}

int cpuScoreFor(const CpuProfile& p, int currentYear) {
    if (!p.recognized) return 40;   // neutral guess
    int s = (p.baseScore > 0) ? p.baseScore : tierBase(p.tier) + powerAdjust(p);
    // -3 points per year of age, capped: an old CPU is slower, but not infinitely so.
    if (p.releaseYear > 0) s -= std::min(30, 3 * std::max(0, currentYear - p.releaseYear));
    return std::clamp(s, 5, 100);
}

// ---------------------------------------------------------------------------
// Other component scores
// ---------------------------------------------------------------------------
int ramScoreFor(int gb) {
    if (gb <= 4) return 10;
    if (gb <= 6) return 25;
    if (gb <= 8) return 40;
    if (gb <= 12) return 55;
    if (gb <= 16) return 70;
    if (gb <= 24) return 80;
    if (gb <= 32) return 90;
    return 100;
}

int storageScoreFor(StorageType t) {
    switch (t) {
        case StorageType::SSD: return 75;
        case StorageType::HDD: return 20;
        default: return 50;
    }
}

int screenScoreFor(int w, int hz) {
    if (w <= 0) return 40;
    int s = 30;
    if (w >= 3840) s = 90;
    else if (w >= 2560) s = 75;
    else if (w >= 1920) s = 55;
    if (hz >= 144) s += 10;
    else if (hz >= 90) s += 5;
    return std::min(s, 100);
}

std::string screenLabelFor(int w) {
    if (w <= 0) return "Unknown";
    if (w >= 3840) return "4K";
    if (w >= 2560) return "QHD";
    if (w >= 1920) return "Full HD";
    if (w >= 1280) return "HD";
    return "Low resolution";
}

Urgency urgencyFromRemaining(int yearsRemaining) {
    if (yearsRemaining >= 2) return Urgency::Ok;
    if (yearsRemaining >= 0) return Urgency::Soon;
    return Urgency::Now;
}

void addAdvice(std::vector<Advice>& list, const char* component, Urgency u, std::string text) {
    Advice a;
    a.component = component;
    a.urgency = u;
    a.text = std::move(text);
    list.push_back(std::move(a));
}

// ---------------------------------------------------------------------------
// Use-case profiles. Each component is judged against what the use needs:
// "target" is the raw score at which that component fully meets the need.
// ---------------------------------------------------------------------------
struct UseProfile {
    int cpuTarget, gpuTarget, ramTarget, storageTarget, screenTarget;
    double weight[5];          // cpu, gpu, ram, storage, screen (sum = 1)
    int cpuLifespanYears;      // how long a CPU stays comfortable for this use
    int minRamGB;              // below this the machine is held back
    int idealRamGB;
    const char* phrase;        // used in sentences
};

const UseProfile kProfiles[4] = {
    //  cpu gpu ram sto scr   weights cpu  gpu   ram   sto   scr           life min ideal
    /* Office    */ {30, 10, 55, 75, 55, {0.35, 0.05, 0.25, 0.25, 0.10},  8,  8, 16, "office and daily use"},
    /* Gaming    */ {45, 60, 70, 75, 55, {0.25, 0.45, 0.15, 0.08, 0.07},  5, 16, 16, "gaming"},
    /* Creator   */ {70, 45, 80, 75, 55, {0.35, 0.20, 0.25, 0.10, 0.10},  6, 16, 32, "video / design work"},
    /* Developer */ {50, 10, 80, 75, 55, {0.30, 0.03, 0.30, 0.27, 0.10},  7, 16, 32, "software development"},
};

int profileIndex(UseCase u) {
    switch (u) {
        case UseCase::Gaming: return kGaming;
        case UseCase::Creator: return kCreator;
        case UseCase::Developer: return kDeveloper;
        default: return kOffice;
    }
}

// 85 = exactly meets the need; ~100 = comfortable headroom (>= 118% of the target).
int fitScore(int score, int target) {
    if (target <= 0) return 100;
    const double f = 85.0 * static_cast<double>(score) / static_cast<double>(target);
    return std::clamp(static_cast<int>(f + 0.5), 0, 100);
}

const char* verdictFor(int fit) {
    if (fit >= 85) return "Comfortably meets the needs";
    if (fit >= 70) return "Good, with some limits";
    if (fit >= 50) return "Usable, but expect slowdowns in demanding tasks";
    return "Struggles: an upgrade is recommended";
}

}  // namespace

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------
const char* urgencyLabel(Urgency u) {
    switch (u) {
        case Urgency::Ok: return "OK";
        case Urgency::Soon: return "SOON";
        default: return "NOW";
    }
}

const char* useCaseLabel(UseCase u) {
    switch (u) {
        case UseCase::Office: return "Office";
        case UseCase::Gaming: return "Gaming";
        case UseCase::Creator: return "Creator";
        case UseCase::Developer: return "Developer";
        default: return "Auto";
    }
}

bool parseUseCase(const std::string& text, UseCase& out) {
    const std::string t = toLower(text);
    if (t == "auto") out = UseCase::Auto;
    else if (t == "office") out = UseCase::Office;
    else if (t == "gaming" || t == "games") out = UseCase::Gaming;
    else if (t == "creator" || t == "editing") out = UseCase::Creator;
    else if (t == "dev" || t == "developer") out = UseCase::Developer;
    else return false;
    return true;
}

int nominalRamGB(double reportedGB) {
    // Windows reports a bit LESS than the installed size (8 GB -> 7.7).
    static const int sizes[] = {2, 4, 6, 8, 12, 16, 20, 24, 32, 48, 64, 96, 128, 192, 256};
    for (int sz : sizes) {
        if (reportedGB <= sz) return sz;
    }
    return static_cast<int>(reportedGB + 0.5);
}

GpuProfile lookupGpu(const std::string& rawName) {
    const std::string name = normalizeName(rawName);
    GpuProfile g;
    for (const GpuEntry& e : kGpuTable) {
        if (containsToken(name, e.key)) {
            g.known = true;
            g.discrete = !e.integrated;
            g.score = e.score;
            g.year = e.year;
            break;
        }
    }
    if (!g.known) {
        g.discrete = isDiscreteByName(name);
        g.score = g.discrete ? 45 : 12;   // unknown card: neutral guess
    }
    // Laptop versions of a card are slower than the desktop card of the same name.
    if (g.discrete && hasLaptopMarker(name)) {
        g.score = std::max(5, g.score - 8);
    }
    return g;
}

CpuProfile parseCpuName(const std::string& name) {
    CpuProfile p;
    if (name.find("Intel") != std::string::npos) p.vendor = "Intel";
    else if (name.find("AMD") != std::string::npos) p.vendor = "AMD";

    std::smatch m;

    // Intel Core i3/i5/i7/i9: "13th Gen Intel(R) Core(TM) i3-1315U"
    // 1st gen has no dash in the real brand string: "Core(TM) i7 CPU 920", "Core(TM) i5 CPU M 520"
    static const std::regex intelCore(
        R"re(Core(?:\(TM\))?\s+i([3579])(?:-|\s+CPU\s+(?:[A-Z]\s+)?)(\d{3,5})([A-Z]{0,3}\d?))re",
        std::regex::icase);
    static const std::regex genPrefix(R"re((\d{1,2})(?:st|nd|rd|th)\s+Gen)re", std::regex::icase);

    if (std::regex_search(name, m, intelCore)) {
        p.recognized = true;
        p.vendor = "Intel";
        p.tier = m[1].str()[0] - '0';
        p.family = "Core i" + m[1].str();
        p.suffix = toUpper(m[3].str());

        const std::string digits = m[2].str();
        int gen = 0;
        std::smatch g;
        if (std::regex_search(name, g, genPrefix)) {
            gen = std::stoi(g[1].str());                    // the name says it directly
        } else if (digits.size() == 3) {
            gen = 1;                                        // i7-920
        } else if (digits.size() == 4) {
            // i7-4770 -> 4th gen, but i5-1135G7 -> 11th gen
            gen = (digits[0] == '1') ? std::stoi(digits.substr(0, 2)) : (digits[0] - '0');
        } else {
            gen = std::stoi(digits.substr(0, 2));           // i7-12700 -> 12th gen
        }
        p.generation = ordinal(gen) + " Gen";
        p.mobile = intelMobileSuffix(p.suffix);
        p.releaseYear = intelGenYear(gen, p.mobile);
        if (!p.mobile) p.socket = intelDesktopSocket(gen);
        p.win11Support = (gen >= 8) ? 1 : 0;   // Windows 11 officially needs 8th gen or newer
        return p;
    }

    // Intel Core Ultra / Core (series) : "Core(TM) Ultra 7 155H", "Core(TM) 5 120U"
    static const std::regex coreSeries(
        R"re(Core(?:\(TM\))?\s+(Ultra\s+)?([3579])\s+(\d{3})([A-Z]{0,2}))re", std::regex::icase);
    if (std::regex_search(name, m, coreSeries)) {
        const bool ultra = m[1].matched;
        const int series = m[3].str()[0] - '0';
        p.recognized = true;
        p.vendor = "Intel";
        p.tier = m[2].str()[0] - '0';
        p.family = std::string(ultra ? "Core Ultra " : "Core ") + m[2].str();
        p.suffix = toUpper(m[4].str());
        p.generation = std::string(ultra ? "Core Ultra" : "Core") + " Series " + std::to_string(series);
        p.releaseYear = ultra ? 2022 + series : 2023 + series;   // approximate
        p.mobile = intelMobileSuffix(p.suffix) || p.suffix == "V";
        p.win11Support = 1;
        if (!p.mobile && ultra && series == 2) p.socket = "LGA1851";
        else if (!p.mobile && !ultra) p.socket = "LGA1700";
        return p;
    }

    // AMD Ryzen: "AMD Ryzen 5 5600X 6-Core Processor"
    static const std::regex ryzen(
        R"re(Ryzen\s+([3579])\s+(?:PRO\s+)?(\d{4})([A-Z0-9]{0,3}))re", std::regex::icase);
    if (std::regex_search(name, m, ryzen)) {
        const std::string digits = m[2].str();
        p.recognized = true;
        p.vendor = "AMD";
        p.tier = m[1].str()[0] - '0';
        p.family = "Ryzen " + m[1].str();
        p.suffix = toUpper(m[3].str());
        p.generation = std::string("Ryzen ") + digits[0] + "000 series";
        // U / H / HS / HX / C are laptop chips; X, X3D, G, GE, F and plain numbers are desktop.
        p.mobile = startsWith(p.suffix, "U") || startsWith(p.suffix, "H") || startsWith(p.suffix, "C");
        p.releaseYear = ryzenSeriesYear(digits[0] - '0', p.mobile);
        {
            const int d = digits[0] - '0';
            p.win11Support = (d == 1) ? 0 : (d >= 3 ? 1 : -1);   // Ryzen 1000: no; 2000: depends on the exact model
        }
        if (!p.mobile) {
            const int d = digits[0] - '0';
            if (d >= 1 && d <= 5) p.socket = "AM4";
            else if (d >= 7 && d <= 9) p.socket = "AM5";
        }
        return p;
    }

    // AMD Ryzen AI: "AMD Ryzen AI 9 HX 370"
    static const std::regex ryzenAi(R"re(Ryzen\s+AI\s+([3579]))re", std::regex::icase);
    if (std::regex_search(name, m, ryzenAi)) {
        p.recognized = true;
        p.vendor = "AMD";
        p.tier = m[1].str()[0] - '0';
        p.family = "Ryzen AI " + m[1].str();
        p.generation = "Ryzen AI 300 series";
        p.releaseYear = 2024;   // approximate
        p.mobile = true;
        p.win11Support = 1;
        if (name.find("HX") != std::string::npos) p.suffix = "HX";
        return p;
    }

    // AMD Ryzen AI Max: "AMD Ryzen AI Max+ 395 w/ Radeon 8060S"
    static const std::regex ryzenAiMax(R"re(Ryzen\s+AI\s+Max\+?\s+(?:PRO\s+)?(\d{3}))re", std::regex::icase);
    if (std::regex_search(name, m, ryzenAiMax)) {
        p.recognized = true;
        p.vendor = "AMD";
        p.tier = 9;   // top-end mobile part
        p.family = "Ryzen AI Max";
        p.generation = std::string("Ryzen AI Max ") + m[1].str()[0] + "00 series";
        p.releaseYear = 2025;   // approximate
        p.mobile = true;
        p.win11Support = 1;
        return p;
    }

    // Families without i3/i5/i7 tiers: scored by a fixed base value (year unknown).
    struct Fallback { const char* word; const char* family; int base; };
    static const Fallback fallbacks[] = {
        {"Celeron", "Celeron", 18}, {"Pentium", "Pentium", 28}, {"Athlon", "Athlon", 26}, {"Xeon", "Xeon", 60},
    };
    for (const Fallback& f : fallbacks) {
        if (name.find(f.word) != std::string::npos) {
            p.recognized = true;
            p.family = f.family;
            p.baseScore = f.base;
            p.generation = f.family;
            return p;
        }
    }
    // Intel N-series (N95 / N100 / N200 / N305): low-power cores
    static const std::regex intelN(R"re(Intel\(R\)\s+(?:Processor\s+)?N(95|97|100|200|305)\b)re");
    if (std::regex_search(name, m, intelN)) {
        p.recognized = true;
        p.vendor = "Intel";
        p.family = "Intel N-series";
        p.generation = "Intel N" + m[1].str();
        p.baseScore = 24;
        p.releaseYear = 2023;
        p.mobile = true;
        return p;
    }

    return p;   // not recognized
}

Assessment assess(const HardwareInfo& hw, int currentYear, UseCase requested) {
    Assessment a;

    // ----- CPU -----
    a.cpu = parseCpuName(hw.cpuName);
    a.cpuScore = cpuScoreFor(a.cpu, currentYear);

    // ----- GPU: prefer a discrete card; among equals take the strongest -----
    int bestIdx = -1;
    int bestKey = -1;
    GpuProfile best;
    for (size_t i = 0; i < hw.gpus.size(); ++i) {
        GpuProfile g = lookupGpu(hw.gpus[i].name);
        // lookupGpu already penalised names that say "Laptop"/"Max-Q"; older laptop
        // drivers (GTX 10-series, RX 6600M ...) don't, so use the device type too.
        if (hw.isLaptop && g.discrete && !hasLaptopMarker(normalizeName(hw.gpus[i].name))) {
            g.score = std::max(5, g.score - 8);
        }
        if (g.discrete) a.hasDiscreteGpu = true;
        const int key = (g.discrete ? 1000 : 0) + g.score;
        if (key > bestKey) {
            bestKey = key;
            best = g;
            bestIdx = static_cast<int>(i);
        }
    }
    if (bestIdx >= 0) {
        a.gpuScore = best.score;
        a.primaryGpu = hw.gpus[static_cast<size_t>(bestIdx)].name;
    } else {
        a.gpuScore = 10;
        a.primaryGpu = "Unknown";
    }

    // ----- other components -----
    const bool ramKnown = hw.totalRamGB > 0.0;
    const int ramGB = ramKnown ? nominalRamGB(hw.totalRamGB) : 0;
    a.ramScore = ramKnown ? ramScoreFor(ramGB) : 40;
    a.storageScore = storageScoreFor(hw.storageType);
    a.screenScore = screenScoreFor(hw.screenWidth, hw.refreshHz);
    a.screenLabel = screenLabelFor(hw.screenWidth);

    // ----- raw performance index (how powerful, regardless of what it is used for) -----
    const double perf = 0.30 * a.cpuScore + 0.40 * a.gpuScore + 0.15 * a.ramScore +
                        0.10 * a.storageScore + 0.05 * a.screenScore;
    a.performanceIndex = static_cast<int>(perf + 0.5);

    int level = 0;   // 0 basic office, 1 strong office/light gaming, 2 gaming, 3 high-end
    if (a.performanceIndex >= 80) level = 3;
    else if (a.performanceIndex >= 55) level = 2;
    else if (a.performanceIndex >= 30) level = 1;
    if (!a.hasDiscreteGpu && level > 1) level = 1;   // no dedicated GPU: capped
    static const char* const kCategories[] = {"Basic office use", "Strong office / light gaming",
                                              "Gaming", "High-end gaming / workstation"};
    a.category = kCategories[level];

    // ----- fit for each use case -----
    for (int i = 0; i < 4; ++i) {
        const UseProfile& pr = kProfiles[i];
        const double f = pr.weight[0] * fitScore(a.cpuScore, pr.cpuTarget) +
                         pr.weight[1] * fitScore(a.gpuScore, pr.gpuTarget) +
                         pr.weight[2] * fitScore(a.ramScore, pr.ramTarget) +
                         pr.weight[3] * fitScore(a.storageScore, pr.storageTarget) +
                         pr.weight[4] * fitScore(a.screenScore, pr.screenTarget);
        a.fitByUseCase[static_cast<size_t>(i)] = static_cast<int>(f + 0.5);
    }

    // Resolve "Auto": a real dedicated GPU suggests gaming, otherwise office.
    UseCase uc = requested;
    if (uc == UseCase::Auto) {
        uc = (a.hasDiscreteGpu && a.gpuScore >= 40) ? UseCase::Gaming : UseCase::Office;
    }
    a.useCase = uc;
    const int pi = profileIndex(uc);
    const UseProfile& prof = kProfiles[pi];
    a.overallScore = a.fitByUseCase[static_cast<size_t>(pi)];
    {
        std::string v = verdictFor(a.overallScore);
        v[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(v[0])));
        a.verdict = std::string("For ") + prof.phrase + ": " + v + ".";
    }
    a.cpuFit = fitScore(a.cpuScore, prof.cpuTarget);
    a.gpuFit = fitScore(a.gpuScore, prof.gpuTarget);
    a.ramFit = fitScore(a.ramScore, prof.ramTarget);
    a.storageFit = fitScore(a.storageScore, prof.storageTarget);
    a.screenFit = fitScore(a.screenScore, prof.screenTarget);

    // ----- how many comfortable years are left, and what ends them -----
    a.yearsLeft = -1;   // -1 = unknown
    {
        auto consider = [&a](int remaining, const char* what) {
            const int r = std::max(0, remaining);
            if (a.yearsLeft < 0 || r < a.yearsLeft) {
                a.yearsLeft = r;
                a.limitingComponent = what;
            }
        };
        if (a.cpu.recognized && a.cpu.releaseYear > 0) {
            consider(prof.cpuLifespanYears - std::max(0, currentYear - a.cpu.releaseYear), "CPU");
        }
        if ((uc == UseCase::Gaming || uc == UseCase::Creator) && a.hasDiscreteGpu && best.year > 0) {
            consider(5 - std::max(0, currentYear - best.year), "GPU");
        }
        if (uc == UseCase::Gaming && !a.hasDiscreteGpu) consider(0, "GPU");
        if (ramKnown && ramGB < prof.minRamGB) consider(0, "RAM");
    }

    // =======================================================================
    // Status lines
    // =======================================================================
    const std::string laptopNote =
        " (In a laptop this part is soldered, so upgrading it means a new device.)";
    const bool heavy = (uc != UseCase::Office);

    // ----- CPU -----
    if (!a.cpu.recognized) {
        addAdvice(a.advice, "CPU", Urgency::Ok,
                  "Could not identify the CPU model, so its score is only a neutral estimate.");
    } else if (a.cpu.releaseYear > 0) {
        const int lifespan = prof.cpuLifespanYears;   // rule of thumb, not a guarantee
        const int age = std::max(0, currentYear - a.cpu.releaseYear);
        const int remaining = lifespan - age;
        Urgency u = urgencyFromRemaining(remaining);
        // A chip that still meets the needs is never "NOW", however old it is.
        if (u == Urgency::Now && a.cpuFit >= 85) u = Urgency::Soon;
        std::string t = a.cpu.generation + " (~" + std::to_string(a.cpu.releaseYear) + ", about " +
                        std::to_string(age) + " years old). ";
        if (u == Urgency::Ok) {
            t += "Rule of thumb: about " + std::to_string(remaining) +
                 " more comfortable years for " + prof.phrase + ".";
        } else if (u == Urgency::Soon) {
            t += (remaining >= 0) ? "Rule of thumb: plan an upgrade within about a year."
                                  : "Past the typical lifespan, but it still meets today's needs: plan a replacement.";
        } else {
            t += "Past the typical lifespan: upgrade if you notice slowdowns.";
        }
        if (hw.isLaptop && u != Urgency::Ok) t += laptopNote;
        addAdvice(a.advice, "CPU", u, t);
    } else {
        addAdvice(a.advice, "CPU", Urgency::Ok, a.cpu.generation + ": release year unknown, scored by its class.");
    }

    if (a.cpu.win11Support == 0) {
        addAdvice(a.advice, "CPU", Urgency::Now,
                  "This CPU is not on Microsoft's Windows 11 supported list, and Windows 10 stopped getting free "
                  "security updates in October 2025: a real security risk for an internet-connected PC.");
    }

    // ----- GPU -----
    if (!a.hasDiscreteGpu) {
        std::string t = "Integrated graphics are fine for office work, video and light games. "
                        "Modern 3D games need a dedicated GPU";
        t += hw.isLaptop ? " (choose a laptop model that has one)." : " (you can add a graphics card).";
        addAdvice(a.advice, "GPU", (uc == UseCase::Gaming) ? Urgency::Soon : Urgency::Ok, t);
    } else {
        if (best.year > 0) {
            const int age = std::max(0, currentYear - best.year);
            const int remaining = 5 - age;
            const Urgency u = urgencyFromRemaining(remaining);
            std::string t = a.primaryGpu + " (~" + std::to_string(best.year) + ", about " +
                            std::to_string(age) + " years old). ";
            if (u == Urgency::Ok) {
                t += "Rule of thumb: about " + std::to_string(remaining) + " more good years for games.";
            } else if (u == Urgency::Soon) {
                t += "Rule of thumb: plan an upgrade within about a year if you play new games.";
            } else {
                t += "Past the typical lifespan for modern games.";
            }
            if (hw.isLaptop && u != Urgency::Ok) t += laptopNote;
            addAdvice(a.advice, "GPU", u, t);
        }
        const double vram = hw.gpus[static_cast<size_t>(bestIdx)].vramGB;
        if (vram > 0.0 && vram < 6.0) {
            addAdvice(a.advice, "GPU", Urgency::Soon,
                      "Only " + num1(vram) + " GB of video memory: new games at high texture settings may stutter.");
        }
    }

    // ----- RAM -----
    if (ramKnown) {
        if (ramGB <= 4) {
            addAdvice(a.advice, "RAM", Urgency::Now,
                      "Only " + std::to_string(ramGB) + " GB of RAM: upgrade to at least 8 GB (16 GB recommended).");
        } else if (ramGB <= 8) {
            if (heavy) {
                addAdvice(a.advice, "RAM", Urgency::Now,
                          "8 GB is not enough for " + std::string(prof.phrase) + ": 16 GB is the realistic minimum.");
            } else {
                std::string t = "8 GB works for light office use, but 16 GB feels much smoother with a "
                                "browser and several apps open.";
                t += hw.isLaptop ? " Check first whether your RAM is upgradable (many laptops solder it)." : "";
                addAdvice(a.advice, "RAM", Urgency::Soon, t);
            }
        } else if (ramGB <= 12) {
            addAdvice(a.advice, "RAM", (uc == UseCase::Creator || uc == UseCase::Developer) ? Urgency::Soon : Urgency::Ok,
                      "12 GB is fine for most uses; 16 GB gives more headroom.");
        } else if (ramGB < prof.idealRamGB) {
            addAdvice(a.advice, "RAM", Urgency::Ok,
                      std::to_string(ramGB) + " GB is enough; " + std::to_string(prof.idealRamGB) +
                      " GB would be ideal for " + prof.phrase + ".");
        } else {
            addAdvice(a.advice, "RAM", Urgency::Ok, std::to_string(ramGB) + " GB is plenty for " + prof.phrase + ".");
        }
        if (hw.ramModules == 1 && !hw.ramLikelySoldered && ramGB >= 8) {
            addAdvice(a.advice, "RAM", Urgency::Ok,
                      "Only one memory stick is installed: a matching second stick enables dual-channel mode "
                      "(noticeably faster for integrated graphics and games).");
        }
    }

    // ----- Storage -----
    if (hw.storageType == StorageType::HDD) {
        addAdvice(a.advice, "Storage", Urgency::Now,
                  "The system drive is a hard disk (HDD). Replacing it with an SSD is the biggest "
                  "speed upgrade you can make.");
    } else if (hw.storageType == StorageType::SSD) {
        addAdvice(a.advice, "Storage", Urgency::Ok, "The system drive is an SSD (good).");
    }
    if (hw.diskTotalGB > 0.0) {
        const double freePercent = hw.diskFreeGB / hw.diskTotalGB * 100.0;
        if (freePercent < 10.0) {
            addAdvice(a.advice, "Storage", Urgency::Now,
                      "Only " + num1(freePercent) + "% of drive " + hw.systemDrive + " is free. Free up space now.");
        } else if (freePercent < 20.0) {
            addAdvice(a.advice, "Storage", Urgency::Soon,
                      "Only " + num1(freePercent) + "% of drive " + hw.systemDrive + " is free. Try to keep at least 20% free.");
        }
    }

    // ----- Screen -----
    if (hw.screenHeight > 0 && hw.screenHeight < 1080) {
        addAdvice(a.advice, "Screen", Urgency::Ok,
                  "The resolution is below Full HD (1920x1080); text and web pages will look smaller and softer.");
    }
    if (uc == UseCase::Gaming && hw.refreshHz > 0 && hw.refreshHz <= 60) {
        addAdvice(a.advice, "Screen", Urgency::Ok,
                  "A 60 Hz screen is fine for office work; 120-165 Hz feels better in fast games.");
    }

    a.recommendations = buildRecommendations(hw, a, currentYear);
    return a;
}
