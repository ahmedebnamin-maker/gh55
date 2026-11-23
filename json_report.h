#pragma once
// JSON output for a back end / web API. Pure C++, no dependencies.
// Every function returns a complete JSON value (object or array).

#include <string>
#include <vector>

#include "analyzer.h"
#include "diagnosis.h"
#include "disk_scan.h"
#include "hardware_types.h"
#include "process_types.h"

std::string jsonEscape(const std::string& s);   // returns the escaped text WITHOUT surrounding quotes

std::string toJson(const HardwareInfo& hw);
std::string toJson(const Assessment& a);
std::string toJson(const LiveStats& s);
std::string toJson(const LiveDiagnosis& d);
std::string toJson(const std::vector<ProcessGroup>& groups);
std::string toJson(const ScanResult& r);
