#pragma once
// Pure C++ logic (no Windows code): parses the CPU name, looks up the GPU,
// scores the device for a chosen use case, and produces upgrade advice with
// approximate prices. Because it has no OS calls it can be unit-tested on any
// machine.

#include <array>
#include <string>
#include <vector>

#include "hardware_types.h"

// What the owner mainly does with the device. Scores are judged against what
// THAT use needs, so an office PC is no longer punished for having no gaming GPU.
enum class UseCase { Auto, Office, Gaming, Creator, Developer };

const char* useCaseLabel(UseCase u);
// Accepts: auto, office, gaming, creator, dev, developer (case-insensitive).
bool parseUseCase(const std::string& text, UseCase& out);

struct CpuProfile {
    bool recognized = false;
    std::string vendor = "Unknown";   // Intel / AMD / Unknown
    std::string family;               // "Core i3", "Ryzen 5", "Core Ultra 7"
    std::string generation;           // "13th Gen", "Ryzen 5000 series"
    int tier = 0;                     // 3 / 5 / 7 / 9 (0 = unknown)
    int releaseYear = 0;              // approximate, 0 = unknown
    std::string suffix;               // "U", "H", "HX", "K", "G7" ...
    bool mobile = false;              // laptop chip (U/H/P/Y/HX...) vs desktop chip
    std::string socket;               // desktop only: "LGA1200", "AM4", "AM5" ... (empty = unknown)
    int baseScore = 0;                // used instead of tier for families without tiers (Celeron...)
    int win11Support = -1;            // 1 = on Microsoft's Windows 11 CPU list, 0 = not, -1 = unknown
};

struct GpuProfile {
    bool known = false;               // found in our lookup table
    bool discrete = false;            // dedicated card (not integrated)
    int score = 0;                    // 0-100, rough relative performance
    int year = 0;                     // approximate launch year (0 = unknown)
};

enum class Urgency { Ok, Soon, Now };

struct Advice {
    std::string component;            // "CPU", "GPU", "RAM", "Storage", "Screen"
    Urgency urgency = Urgency::Ok;
    std::string text;
};

// A concrete purchase / action suggestion. Prices are APPROXIMATE street prices
// in US dollars (see priceDisclaimer()); 0/0 means "free" or "no price".
struct Recommendation {
    std::string component;            // "RAM", "Storage", "CPU", "GPU", "Device", "Overall"
    Urgency priority = Urgency::Ok;
    std::string title;                // short headline
    std::string detail;               // why / how
    std::string examples;             // example parts or models
    int priceMinUSD = 0;
    int priceMaxUSD = 0;
    int extraYears = 0;               // roughly how many more years it should buy
};

enum UseCaseIndex { kOffice = 0, kGaming = 1, kCreator = 2, kDeveloper = 3 };

struct Assessment {
    CpuProfile cpu;

    // Raw component scores (0-100, relative to today's best hardware).
    int cpuScore = 0;
    int gpuScore = 0;
    int ramScore = 0;
    int storageScore = 0;
    int screenScore = 0;

    // "Raw power" of the machine and its class, independent of use case.
    int performanceIndex = 0;
    std::string category;             // "Basic office use", "Gaming", ...

    // Fitness for the selected use case: 85 = meets the needs, 100 = plenty of headroom.
    UseCase useCase = UseCase::Office;   // the resolved use case (never Auto)
    int overallScore = 0;                // fit for `useCase` (0-100)
    std::string verdict;                 // plain-language meaning of overallScore
    std::array<int, 4> fitByUseCase{};   // fit for Office / Gaming / Creator / Developer
    int cpuFit = 0, gpuFit = 0, ramFit = 0, storageFit = 0, screenFit = 0;

    // How long the device should stay comfortable for `useCase`.
    int yearsLeft = -1;                  // -1 = unknown
    std::string limitingComponent;       // which part ends its life first

    std::string primaryGpu;
    bool hasDiscreteGpu = false;
    std::string screenLabel;             // "Full HD", "QHD", "4K" ...
    std::vector<Advice> advice;          // short status lines per component
    std::vector<Recommendation> recommendations;   // what to buy / do, most urgent first
};

CpuProfile parseCpuName(const std::string& cpuName);
GpuProfile lookupGpu(const std::string& gpuName);
int nominalRamGB(double reportedGB);   // 7.7 -> 8, 15.8 -> 16 ...
Assessment assess(const HardwareInfo& hw, int currentYear, UseCase useCase = UseCase::Auto);
const char* urgencyLabel(Urgency u);

// Implemented in recommend.cpp. Called by assess(); exposed for tests.
std::vector<Recommendation> buildRecommendations(const HardwareInfo& hw, const Assessment& a,
                                                 int currentYear);
// Text to show next to any price: they are estimates and the market is volatile.
const char* priceDisclaimer();
