#include "providers.h"

namespace as {

void MemoryProvider::sample(MemorySample& m) {
    auto info = read_file(r_.proc + "/meminfo");
    if (!info) throw std::runtime_error("cannot read /proc/meminfo");
    for (const auto& line : split(*info, '\n')) {
        auto f = split_ws(line);
        if (f.size() < 2) continue;
        uint64_t kb = to_uint(f[1]).value_or(0) * 1024;
        auto k = f[0];
        if (k == "MemTotal:") m.total = kb;
        else if (k == "MemAvailable:") m.available = kb;
        else if (k == "Cached:") m.cached = kb;
        else if (k == "Buffers:") m.buffers = kb;
        else if (k == "Shmem:") m.shared = kb;
        else if (k == "Dirty:") m.dirty = kb;
        else if (k == "SwapTotal:") m.swap_total = kb;
        else if (k == "SwapFree:") m.swap_free = kb;
        else if (k == "Committed_AS:") m.committed = kb;
        else if (k == "CommitLimit:") m.commit_limit = kb;
    }
    m.used = m.total > m.available ? m.total - m.available : 0;

    if (auto p = read_file(r_.proc + "/pressure/memory")) {
        for (const auto& line : split(*p, '\n')) {
            auto f = split_ws(line);
            if (f.size() < 2 || !starts_with(f[1], "avg10=")) continue;
            double v = std::stod(std::string(f[1].substr(6)));
            if (f[0] == "some") m.pressure_some10 = Reading<double>::of(v);
            if (f[0] == "full") m.pressure_full10 = Reading<double>::of(v);
        }
    } else {
        m.pressure_some10 = Reading<double>::none("The kernel was built without pressure stall information");
        m.pressure_full10 = m.pressure_some10;
    }

    if (auto vm = read_file(r_.proc + "/vmstat")) {
        for (const auto& line : split(*vm, '\n')) {
            auto f = split_ws(line);
            if (f.size() < 2) continue;
            if (f[0] == "pswpin") m.page_ins = to_uint(f[1]).value_or(0);
            if (f[0] == "pswpout") m.page_outs = to_uint(f[1]).value_or(0);
        }
    }

    // "/dev/zram0  partition  8089084  8004652  100": the size column is in kB.
    if (auto sw = read_file(r_.proc + "/swaps")) {
        for (const auto& line : split(*sw, '\n')) {
            auto f = split_ws(line);
            if (f.size() < 3) continue;
            const auto slash = f[0].rfind('/');
            if (starts_with(slash == std::string::npos ? f[0] : f[0].substr(slash + 1), "zram"))
                m.zram_swap += to_uint(f[2]).value_or(0) * 1024;
        }
    }

    for (const auto& b : list_dir(r_.sys + "/block")) {
        if (!starts_with(b, "zram")) continue;
        if (auto mm = read_line(r_.sys + "/block/" + b + "/mm_stat")) {
            auto f = split_ws(*mm);
            if (f.size() >= 2) {
                m.zram_orig += to_uint(f[0]).value_or(0);
                m.zram_compr += to_uint(f[1]).value_or(0);
            }
        }
    }
}

}  // namespace as
