#pragma once
#include <map>
#include <string>
#include <vector>

#include "snapshot.h"

namespace as {

// One process from start to finish.
struct ProcessRecord {
    int pid = 0;
    double started = 0, ended = 0;  // ended stays 0 while it runs
    std::string name, exe, cmdline, user, unit, parent;
    int ppid = 0;
    double cpu_seconds = 0;
    uint64_t peak_rss = 0;
};

// Turns the process scans into a record of every program that ran (long
// enough to be seen by a scan: about one to three seconds).
class ProcessLog {
public:
    // Returns records that started or ended since the last call.
    std::vector<ProcessRecord> feed(const Snapshot& s);

private:
    std::map<std::pair<int, uint64_t>, ProcessRecord> live_;
};

}  // namespace as
