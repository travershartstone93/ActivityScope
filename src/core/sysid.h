#pragma once
#include <cstdint>
#include <string>

#include "fs.h"

namespace as {

// What this machine is: the facts that do not change between samples.
// Read once per question from DMI, os-release, uname and the hostname;
// the moving parts (CPU model, RAM size, disks) come from a Snapshot.
struct SystemId {
    std::string vendor;       // e.g. "LENOVO"
    std::string product;      // e.g. "ThinkPad X1"
    std::string os_pretty;    // e.g. "CachyOS Linux"
    std::string kernel;       // e.g. "7.2.9-2-cachyos"
    std::string arch;         // e.g. "x86_64"
    std::string hostname;
    int64_t uptime_seconds = 0;
};

SystemId system_identity(const Roots& r = {});

}  // namespace as
