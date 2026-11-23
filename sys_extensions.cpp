#include "sys_extensions.h"

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <sstream>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr double kMB = 1024.0 * 1024.0;

std::string fmt1(double v) {
    if (!std::isfinite(v) || v < 0.0) v = 0.0;
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << v;
    return o.str();
}

std::string formatMB(double mb) { return fmt1(mb) + " MB"; }

uint64_t fileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
}

uint64_t safeSub(uint64_t now, uint64_t before) { return now >= before ? (now - before) : 0ULL; }

std::string wideToUtf8(const wchar_t* w) {
    if (!w || w[0] == L'\0') return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], needed, nullptr, nullptr);
    if (written <= 0) return {};
    return out;
}

// Least-privilege memory query: PROCESS_QUERY_LIMITED_INFORMATION is enough on
// current Windows. If the OS still refuses, retry once with VM_READ added.
bool queryWorkingSetBytes(DWORD pid, HANDLE limitedHandle, std::size_t& bytesOut, DWORD& errOut) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (limitedHandle &&
        GetProcessMemoryInfo(limitedHandle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        bytesOut = pmc.WorkingSetSize;
        return true;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        errOut = GetLastError();
        return false;
    }
    pmc = {};
    pmc.cb = sizeof(pmc);
    const BOOL ok = GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    if (ok) bytesOut = pmc.WorkingSetSize; else errOut = GetLastError();
    CloseHandle(h);
    return ok != FALSE;
}

// Quotes one argument for a Windows command line (CommandLineToArgvW rules).
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

}  // namespace

// ============================================================================
// ProcessMonitor
// ============================================================================
ProcessMonitor::ProcessMonitor() : lastSampleTime_(std::chrono::steady_clock::now()) {}

bool ProcessMonitor::readSystemTotal100ns(uint64_t& totalOut) {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return false;
    totalOut = fileTimeToU64(kernel) + fileTimeToU64(user);   // across ALL logical CPUs
    return true;
}

std::vector<ProcessSnapshot> ProcessMonitor::listProcesses(bool sortByCpu, std::size_t limit) {
    std::vector<ProcessSnapshot> result;

    uint64_t sysTotalNow = 0;
    const bool sysOk = readSystemTotal100ns(sysTotalNow);

    const auto nowTp = std::chrono::steady_clock::now();
    const double elapsedSec =
        hasBaseline_ ? std::chrono::duration<double>(nowTp - lastSampleTime_).count() : 0.0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    std::unordered_map<uint32_t, CpuSample> nextSamples;
    nextSamples.reserve(256);

    if (Process32FirstW(snap, &pe)) {
        do {
            const uint32_t pid = static_cast<uint32_t>(pe.th32ProcessID);
            if (pid == 0) continue;

            ProcessSnapshot item;
            item.pid = pid;
            item.name = sanitizeProcessName(wideToUtf8(pe.szExeFile));
            if (item.name.empty()) item.name = "<unknown>";

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
            if (hProc) {
                std::size_t wsBytes = 0;
                DWORD memErr = 0;
                if (queryWorkingSetBytes(static_cast<DWORD>(pid), hProc, wsBytes, memErr)) {
                    item.ramMB = static_cast<double>(wsBytes) / kMB;
                } else if (memErr == ERROR_ACCESS_DENIED) {
                    item.accessRestricted = true;
                }

                FILETIME createFt{}, exitFt{}, kernelFt{}, userFt{};
                if (sysOk && GetProcessTimes(hProc, &createFt, &exitFt, &kernelFt, &userFt)) {
                    CpuSample cur;
                    cur.procKernel100ns = fileTimeToU64(kernelFt);
                    cur.procUser100ns = fileTimeToU64(userFt);
                    cur.sysTotal100ns = sysTotalNow;

                    auto prevIt = prevSamples_.find(pid);
                    if (hasBaseline_ && prevIt != prevSamples_.end() && elapsedSec > 0.01) {
                        const uint64_t dProc = safeSub(cur.procKernel100ns, prevIt->second.procKernel100ns) +
                                               safeSub(cur.procUser100ns, prevIt->second.procUser100ns);
                        const uint64_t dSys = safeSub(cur.sysTotal100ns, prevIt->second.sysTotal100ns);
                        if (dSys > 0) {
                            // dSys already sums all logical CPUs, so this is "% of the whole machine".
                            const double rawPct = static_cast<double>(dProc) / static_cast<double>(dSys) * 100.0;
                            item.cpuPercent = std::clamp(rawPct, 0.0, 100.0);
                        }
                    }
                    nextSamples[pid] = cur;
                }
                CloseHandle(hProc);
            } else {
                if (GetLastError() == ERROR_ACCESS_DENIED) item.accessRestricted = true;
            }

            result.push_back(std::move(item));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (sysOk) {
        prevSamples_ = std::move(nextSamples);
        lastSampleTime_ = nowTp;
        hasBaseline_ = true;
    }

    if (sortByCpu) {
        std::sort(result.begin(), result.end(), [](const ProcessSnapshot& a, const ProcessSnapshot& b) {
            if (a.cpuPercent != b.cpuPercent) return a.cpuPercent > b.cpuPercent;
            return a.ramMB > b.ramMB;
        });
    } else {
        std::sort(result.begin(), result.end(), [](const ProcessSnapshot& a, const ProcessSnapshot& b) {
            if (a.ramMB != b.ramMB) return a.ramMB > b.ramMB;
            return a.cpuPercent > b.cpuPercent;
        });
    }
    if (limit > 0 && result.size() > limit) result.resize(limit);
    return result;
}

TrimResult ProcessMonitor::trimWorkingSet(uint32_t pid) {
    TrimResult res;

    if (pid == 0 || pid == 4) {
        res.errorCode = ERROR_ACCESS_DENIED;
        res.message = "Refusing to trim protected system process (PID " + std::to_string(pid) + ").";
        return res;
    }

    HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!hProc) {
        const DWORD err = GetLastError();
        res.errorCode = static_cast<uint32_t>(err);
        if (err == ERROR_ACCESS_DENIED) {
            res.needsElevation = true;
            res.message = "Access denied while opening the process for a memory trim (Administrator rights may be required).";
        } else if (err == ERROR_INVALID_PARAMETER) {
            res.message = "Process does not exist or has already exited.";
        } else {
            res.message = "Failed to open target process (Win32 error " + std::to_string(err) + ").";
        }
        return res;
    }

    std::size_t beforeBytes = 0;
    DWORD ignored = 0;
    if (queryWorkingSetBytes(static_cast<DWORD>(pid), hProc, beforeBytes, ignored)) {
        res.beforeMB = static_cast<double>(beforeBytes) / kMB;
    }

    const BOOL trimmed = EmptyWorkingSet(hProc);
    const DWORD trimErr = trimmed ? ERROR_SUCCESS : GetLastError();

    std::size_t afterBytes = 0;
    const bool haveAfter = trimmed && queryWorkingSetBytes(static_cast<DWORD>(pid), hProc, afterBytes, ignored);
    CloseHandle(hProc);

    if (!trimmed) {
        res.errorCode = static_cast<uint32_t>(trimErr);
        if (trimErr == ERROR_ACCESS_DENIED) {
            res.needsElevation = true;
            res.message = "EmptyWorkingSet denied (Administrator rights may be required).";
        } else {
            res.message = "EmptyWorkingSet failed (Win32 error " + std::to_string(trimErr) + ").";
        }
        return res;
    }

    if (haveAfter) {
        res.afterMB = static_cast<double>(afterBytes) / kMB;
        res.freedMB = std::max(0.0, res.beforeMB - res.afterMB);
    }
    res.ok = true;
    res.errorCode = 0;
    res.message = "Trimmed working set: before " + formatMB(res.beforeMB) + " -> after " + formatMB(res.afterMB) +
                  " (freed ~" + formatMB(res.freedMB) + "). Windows may load the pages back as the app runs.";
    return res;
}

bool ProcessMonitor::isElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION elev{};
    DWORD retLen = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &retLen)) {
            elevated = (elev.TokenIsElevated != 0);
        }
        CloseHandle(token);
    }
    return elevated;
}

bool ProcessMonitor::relaunchAsAdmin(const char* extraArg, bool closeCurrentProcessOnSuccess, uint32_t* errorOut) {
    if (errorOut) *errorOut = 0;
    if (isElevated()) return true;

    wchar_t exePath[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (len == 0 || len >= std::size(exePath)) {
        if (errorOut) *errorOut = GetLastError();
        return false;
    }

    // Rebuild the original arguments (skip argv[0]) so --json, --usecase ... survive the restart.
    std::wstring params;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (!params.empty()) params += L' ';
            params += quoteArg(argv[i]);
        }
        LocalFree(argv);
    }
    if (extraArg && extraArg[0]) {
        if (!params.empty()) params += L' ';
        params += quoteArg(std::wstring(extraArg, extraArg + std::char_traits<char>::length(extraArg)));
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";          // shows the UAC prompt
    sei.lpFile = exePath;
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        if (errorOut) *errorOut = GetLastError();   // ERROR_CANCELLED (1223) = user pressed "No"
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);

    if (closeCurrentProcessOnSuccess) std::exit(0);
    return true;
}
