#include "disk_scan.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

std::string toUtf8(const fs::path& p) {
    const auto u = p.u8string();   // std::string (C++17) or std::u8string (C++20)
    return std::string(u.begin(), u.end());
}

constexpr unsigned long long kMaxEntries = 5000000ULL;   // safety cap

struct Budget {
    std::chrono::steady_clock::time_point deadline;
    unsigned long long entries = 0;
    bool stop = false;

    bool tick() {
        if (stop) return false;
        if ((++entries & 0x1FF) == 0 && std::chrono::steady_clock::now() > deadline) stop = true;
        if (entries > kMaxEntries) stop = true;
        return !stop;
    }
};

// Adds up the sizes of all regular files below `dir`.
void sumDirectory(const fs::path& dir, int maxDepth, Budget& b, FolderSize& out) {
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    if (ec) return;
    while (it != end) {
        if (!b.tick()) return;
        std::error_code e2;
        const fs::file_status st = it->symlink_status(e2);
        if (!e2) {
            if (fs::is_regular_file(st)) {
                const auto sz = it->file_size(e2);
                if (!e2) {
                    out.bytes += sz;
                    out.files += 1;
                }
            } else if (fs::is_directory(st)) {
                // Real directories only: symlinks and junctions report another file type.
                if (it.depth() >= maxDepth) it.disable_recursion_pending();
            } else {
                it.disable_recursion_pending();
            }
        }
        std::error_code e3;
        it.increment(e3);
        if (e3) break;
    }
}

}  // namespace

ScanResult scanLargestFolders(const std::string& rootUtf8, std::size_t topN, double timeLimitSeconds, int maxDepth) {
    ScanResult res;
    const auto start = std::chrono::steady_clock::now();
    Budget b;
    b.deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(timeLimitSeconds));

    std::error_code ec;
    const fs::path root = fs::path(std::u8string_view(
        reinterpret_cast<const char8_t*>(rootUtf8.data()),
        rootUtf8.size()
    ));
    if (!fs::is_directory(root, ec) || ec) {
        res.error = "Not a readable folder: " + rootUtf8;
        return res;
    }

    fs::directory_iterator top(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        res.error = "Cannot read folder: " + rootUtf8;
        return res;
    }

    FolderSize looseFiles;
    looseFiles.path = toUtf8(root) + " (files directly inside)";
    for (const fs::directory_iterator end; top != end;) {
        if (!b.tick()) break;
        std::error_code e2;
        const fs::file_status st = top->symlink_status(e2);
        if (!e2) {
            if (fs::is_directory(st)) {
                FolderSize f;
                f.path = toUtf8(top->path());
                sumDirectory(top->path(), maxDepth, b, f);
                res.totalBytes += f.bytes;
                res.folders.push_back(std::move(f));
            } else if (fs::is_regular_file(st)) {
                const auto sz = top->file_size(e2);
                if (!e2) {
                    looseFiles.bytes += sz;
                    looseFiles.files += 1;
                    res.totalBytes += sz;
                }
            }
        }
        std::error_code e3;
        top.increment(e3);
        if (e3) break;
    }
    if (looseFiles.files > 0) res.folders.push_back(std::move(looseFiles));

    std::sort(res.folders.begin(), res.folders.end(),
              [](const FolderSize& a, const FolderSize& c) { return a.bytes > c.bytes; });
    if (topN > 0 && res.folders.size() > topN) res.folders.resize(topN);

    res.truncated = b.stop;
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return res;
}
