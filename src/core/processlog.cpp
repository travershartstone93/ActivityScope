#include "processlog.h"

#include <algorithm>
#include <unordered_map>

namespace as {

std::vector<ProcessRecord> ProcessLog::feed(const Snapshot& s) {
    std::vector<ProcessRecord> out;
    if (s.procs_reused) return out;
    std::unordered_map<int, std::string> names;
    for (const auto& p : s.procs) names[p.key.pid] = p.name;
    for (const auto& p : s.procs) {
        if (p.kernel_thread) continue;
        auto key = std::make_pair(p.key.pid, p.key.start_ticks);
        auto it = live_.find(key);
        if (it == live_.end()) {
            ProcessRecord r;
            r.pid = p.key.pid;
            r.started = p.started_epoch;
            r.name = p.name;
            r.exe = p.exe;
            r.cmdline = p.cmdline;
            r.user = p.user;
            r.unit = p.unit;
            r.ppid = p.ppid;
            auto pn = names.find(p.ppid);
            r.parent = pn == names.end() ? "" : pn->second;
            r.cpu_seconds = p.cpu_seconds;
            r.peak_rss = p.rss;
            live_.emplace(key, r);
            out.push_back(r);
        } else {
            it->second.cpu_seconds = p.cpu_seconds;
            it->second.peak_rss = std::max(it->second.peak_rss, p.rss);
        }
    }
    for (const auto& p : s.exited) {
        auto it = live_.find({p.key.pid, p.key.start_ticks});
        if (it == live_.end()) continue;
        ProcessRecord r = it->second;
        // The exited entry carries the final totals, which can be ahead of
        // the last scan for a process that burned CPU right before exiting.
        r.cpu_seconds = std::max(r.cpu_seconds, p.cpu_seconds);
        r.peak_rss = std::max(r.peak_rss, p.rss);
        r.ended = s.wall;
        out.push_back(r);
        live_.erase(it);
    }
    return out;
}

}  // namespace as
