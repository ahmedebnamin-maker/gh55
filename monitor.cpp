#define NOMINMAX
#include <winsock2.h>   // must come BEFORE windows.h
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <dxgi.h>
#include <intrin.h>
#include <winioctl.h>
#include <powrprof.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "monitor.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "powrprof.lib")

namespace {

constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

unsigned long long toU64(const FILETIME& ft) {
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Safe difference: unsigned "now - before" would wrap to a huge number if the
// counter ever went backwards. We return 0 instead.
unsigned long long safeDelta(unsigned long long now, unsigned long long before) {
    return now >= before ? now - before : 0;
}

// Wide string (UTF-16) -> UTF-8, so names with non-English characters are safe.
std::string toUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string getCpuName() {
    int regs[4] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004u) return "Unknown";

    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + i * 16, regs, 16);
    }
    std::string name(brand);
    size_t start = name.find_first_not_of(' ');
    return start == std::string::npos ? "Unknown" : name.substr(start);
}

// Asks Windows whether the drive "seeks" (spinning HDD) or not (SSD).
// Opening the drive with access 0 only allows queries, so no admin rights.
StorageType detectStorageType(char driveLetter) {
    char path[] = "\\\\.\\X:";
    path[4] = driveLetter;

    HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return StorageType::Unknown;

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;

    DEVICE_SEEK_PENALTY_DESCRIPTOR result = {};
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                              &result, sizeof(result), &returned, nullptr);
    CloseHandle(h);   // always close the handle, success or not

    if (!ok) return StorageType::Unknown;
    return result.IncursSeekPenalty ? StorageType::HDD : StorageType::SSD;
}

// The drive Windows is installed on ("C:" almost always, but not guaranteed).
std::string getSystemDrive() {
    char buf[MAX_PATH] = {};
    const UINT n = GetSystemWindowsDirectoryA(buf, MAX_PATH);   // e.g. "C:\Windows"
    if (n >= 2 && n < MAX_PATH && buf[1] == ':') return std::string(1, buf[0]) + ":";
    return "C:";
}

// Laptop or desktop? A battery alone is a weak signal (a desktop on a USB UPS
// shows a battery; a laptop with the battery removed shows none), so first ask
// what the PC maker declares (ACPI preferred power profile), and only use the
// battery check when Windows can't tell.
bool detectLaptop() {
    switch (PowerDeterminePlatformRoleEx(POWER_PLATFORM_ROLE_V2)) {
        case PlatformRoleMobile:
        case PlatformRoleSlate:
            return true;
        case PlatformRoleDesktop:
        case PlatformRoleWorkstation:
        case PlatformRoleEnterpriseServer:
        case PlatformRoleSOHOServer:
        case PlatformRolePerformanceServer:
            return false;
        default:
            break;   // unspecified -> fall back to the battery check
    }
    // BatteryFlag 128 = "no system battery", 255 = unknown.
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        return sps.BatteryFlag != 128 && sps.BatteryFlag != 255;
    }
    return false;
}


// ---------------------------------------------------------------------------
// RAM details from the SMBIOS "Memory Device" tables (type 17). No WMI/COM and
// no admin rights needed. Everything is optional: on any problem we leave the
// fields at "unknown" and the analyzer simply skips the extra advice.
// ---------------------------------------------------------------------------
const char* smbiosMemoryType(unsigned char t) {
    switch (t) {
        case 0x12: return "DDR";    case 0x13: return "DDR2";   case 0x18: return "DDR3";
        case 0x1A: return "DDR4";   case 0x22: return "DDR5";
        case 0x1B: return "LPDDR";  case 0x1C: return "LPDDR2"; case 0x1D: return "LPDDR3";
        case 0x1E: return "LPDDR4"; case 0x23: return "LPDDR5";
        default: return "";
    }
}

void readRamModules(HardwareInfo& hw) {
    const DWORD sig = 'RSMB';
    const UINT size = GetSystemFirmwareTable(sig, 0, nullptr, 0);
    if (size < 8 || size > (16u << 20)) return;   // sanity: a table is never huge
    std::vector<unsigned char> buf(size);
    if (GetSystemFirmwareTable(sig, 0, buf.data(), size) != size) return;

    // RawSMBIOSData: 4 bytes of version info, DWORD length, then the structures.
    DWORD tableLen = 0;
    std::memcpy(&tableLen, buf.data() + 4, sizeof(tableLen));
    const unsigned char* p = buf.data() + 8;
    const unsigned char* end = p + std::min<size_t>(tableLen, size - 8);

    int slots = 0, filled = 0, speed = 0;
    std::string type;
    bool soldered = false;
    while (p + 4 <= end) {
        const unsigned char stype = p[0];
        const unsigned char slen = p[1];
        if (slen < 4 || p + slen > end) break;                    // malformed: stop safely
        if (stype == 127) break;                                  // end-of-table marker
        if (stype == 17 && slen >= 0x13) {                        // Memory Device
            ++slots;
            unsigned size16 = 0;
            std::memcpy(&size16, p + 0x0C, 2);
            unsigned long long sizeMB = size16;
            if (size16 == 0x7FFF && slen >= 0x20) {               // extended size field
                unsigned int ext = 0;
                std::memcpy(&ext, p + 0x1C, 4);
                sizeMB = ext & 0x7FFFFFFFu;
            } else if (size16 & 0x8000u) {
                sizeMB = (size16 & 0x7FFFu) / 1024;               // value was in KB
            }
            if (size16 != 0 && size16 != 0xFFFF && sizeMB > 0) {  // populated slot
                ++filled;
                const unsigned char formFactor = p[0x0E];
                const char* t = smbiosMemoryType(p[0x12]);
                if (type.empty() && t[0]) type = t;
                if (std::strncmp(t, "LPDDR", 5) == 0 || formFactor == 0x05 || formFactor == 0x0B || formFactor == 0x10) {
                    soldered = true;                              // chip / row of chips / die: on-board memory
                }
                unsigned short cfg = 0, mx = 0;
                if (slen >= 0x17) std::memcpy(&mx, p + 0x15, 2);
                if (slen >= 0x22) std::memcpy(&cfg, p + 0x20, 2);
                const int s = cfg ? cfg : mx;
                if (s > 0 && (speed == 0 || s < speed)) speed = s;   // report the slowest stick (it sets the pace)
            }
        }
        // skip the formatted area, then the string-set (ends with a double NUL)
        const unsigned char* q = p + slen;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) ++q;
        p = q + 2;
    }
    hw.ramSlotsTotal = slots;
    hw.ramModules = filled;
    hw.ramSpeedMHz = speed;
    hw.ramType = type;
    hw.ramLikelySoldered = soldered;
}

}  // namespace

// ---------------------------------------------------------------------------
// Static hardware information

// ---------------------------------------------------------------------------
HardwareInfo SystemMonitor::readHardwareInfo() {
    HardwareInfo hw;

    hw.cpuName = getCpuName();

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    hw.logicalProcessors = static_cast<int>(si.dwNumberOfProcessors);

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        hw.totalRamGB = static_cast<double>(mem.ullTotalPhys) / kGB;
    }

    hw.systemDrive = getSystemDrive();
    std::error_code ec;
    auto info = std::filesystem::space(hw.systemDrive + "\\", ec);
    if (!ec) {
        hw.diskTotalGB = static_cast<double>(info.capacity) / kGB;
        hw.diskFreeGB = static_cast<double>(info.available) / kGB;
    }
    hw.storageType = detectStorageType(hw.systemDrive[0]);

    DEVMODEA dm = {};
    dm.dmSize = static_cast<WORD>(sizeof(dm));
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        hw.screenWidth = static_cast<int>(dm.dmPelsWidth);
        hw.screenHeight = static_cast<int>(dm.dmPelsHeight);
        // 0 and 1 both mean "hardware default" (not a real refresh rate).
        hw.refreshHz = dm.dmDisplayFrequency > 1 ? static_cast<int>(dm.dmDisplayFrequency) : 0;
    }

    hw.isLaptop = detectLaptop();
    readRamModules(hw);

    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                GpuInfo g;
                g.name = toUtf8(desc.Description);
                g.vramGB = static_cast<double>(desc.DedicatedVideoMemory) / kGB;
                hw.gpus.push_back(g);
            }
            adapter->Release();
        }
        factory->Release();
    }
    return hw;
}

// ---------------------------------------------------------------------------
// Live readings
// ---------------------------------------------------------------------------
bool SystemMonitor::readCpuTimes(CpuTimes& out) {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return false;
    out.idle = toU64(idle);
    out.total = toU64(kernel) + toU64(user);   // kernel time already includes idle
    return true;
}

double SystemMonitor::cpuUsagePercent(const CpuTimes& before, const CpuTimes& now) {
    unsigned long long idleDelta = safeDelta(now.idle, before.idle);
    unsigned long long totalDelta = safeDelta(now.total, before.total);
    if (totalDelta == 0) return 0.0;   // avoid division by zero
    double usage = (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta)) * 100.0;
    return std::clamp(usage, 0.0, 100.0);
}

bool SystemMonitor::readNetSnapshot(NetSnapshot& out) {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR) return false;

    out.clear();
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.OperStatus != IfOperStatusUp) continue;                    // adapter is up
        if (!row.InterfaceAndOperStatusFlags.HardwareInterface) continue;  // real adapters only
        NetCounters c;
        c.rx = row.InOctets;
        c.tx = row.OutOctets;
        out[row.InterfaceLuid.Value] = c;   // keep each adapter separate
    }
    FreeMibTable(table);   // Windows allocated it, we must free it
    return true;
}

void SystemMonitor::hardenProcess() {
    // Only load DLLs from System32 (blocks "DLL planting" from the current/exe folder).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // Terminate immediately on heap corruption instead of continuing in an unsafe state.
    HeapSetInformation(GetProcessHeap(), HeapEnableTerminationOnCorruption, nullptr, 0);
}

void SystemMonitor::enableUtf8Console() {
    SetConsoleOutputCP(CP_UTF8);   // names are converted to UTF-8 (toUtf8), so print them as UTF-8
}

SystemMonitor::SystemMonitor() {
    sysDrive_ = getSystemDrive();
    cpuBaselineOk_ = readCpuTimes(prevCpu_);
    netBaselineOk_ = readNetSnapshot(prevNet_);
    prevTime_ = std::chrono::steady_clock::now();
}

LiveStats SystemMonitor::readStats() {
    LiveStats s;

    // RAM and disk are instant readings (no baseline needed).
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        s.ramPercent = static_cast<double>(mem.dwMemoryLoad);
        s.ramUsedGB = static_cast<double>(mem.ullTotalPhys - mem.ullAvailPhys) / kGB;
    }

    std::error_code ec;
    auto info = std::filesystem::space(sysDrive_ + "\\", ec);
    if (!ec && info.capacity > 0) {
        s.diskUsedPercent =
            (1.0 - static_cast<double>(info.available) / static_cast<double>(info.capacity)) * 100.0;
    }

    // CPU needs two readings: compare with the previous one.
    CpuTimes cpu;
    if (readCpuTimes(cpu)) {
        if (cpuBaselineOk_) s.cpuPercent = cpuUsagePercent(prevCpu_, cpu);
        prevCpu_ = cpu;
        cpuBaselineOk_ = true;
    }

    // Network speed = bytes difference / REAL elapsed seconds (not a fixed 1.0).
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - prevTime_).count();

    NetSnapshot net;
    if (readNetSnapshot(net)) {
        if (!netBaselineOk_) {
            prevNet_ = std::move(net);
            prevTime_ = now;
            netBaselineOk_ = true;
        } else if (seconds >= 0.05) {
            unsigned long long rx = 0, tx = 0;
            for (const auto& entry : net) {
                const auto before = prevNet_.find(entry.first);
                if (before == prevNet_.end()) continue;   // adapter just appeared: no baseline yet,
                                                          // skip it for this reading (no fake spike)
                rx += safeDelta(entry.second.rx, before->second.rx);
                tx += safeDelta(entry.second.tx, before->second.tx);
            }
            s.downKBps = static_cast<double>(rx) / 1024.0 / seconds;
            s.upKBps = static_cast<double>(tx) / 1024.0 / seconds;
            prevNet_ = std::move(net);
            prevTime_ = now;
        }
    }
    return s;
}
