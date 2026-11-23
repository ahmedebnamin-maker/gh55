#pragma once
// Finds which folders use the most disk space. Pure C++17 <filesystem>, no
// Windows API: runs (and is tested) on any OS. Never follows symlinks/junctions,
// skips unreadable folders, and stops at a time limit so it cannot hang.

#include <cstddef>
#include <string>
#include <vector>

struct FolderSize {
    std::string path;                 // UTF-8
    unsigned long long bytes = 0;
    unsigned long long files = 0;
};

struct ScanResult {
    std::vector<FolderSize> folders;  // largest first (direct children of the scanned root)
    unsigned long long totalBytes = 0;
    bool truncated = false;           // stopped early (time or entry limit): sizes are lower bounds
    double seconds = 0.0;
    std::string error;                // non-empty if the root could not be read
};

ScanResult scanLargestFolders(const std::string& rootUtf8, std::size_t topN = 10,
                              double timeLimitSeconds = 20.0, int maxDepth = 16);
