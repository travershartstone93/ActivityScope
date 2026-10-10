#include "conkyfeed.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace as {

std::string runtime_dir() {
    const char* r = std::getenv("XDG_RUNTIME_DIR");
    std::string base = r && *r ? r : "/tmp";
    return base + "/activityscope";
}

void write_conky_feed(const Snapshot& s, const std::string& dir) {
    ::mkdir(dir.c_str(), 0700);
    const std::string tmp = dir + "/conky.txt.tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "time %.0f\n", s.wall);
    std::fprintf(f, "cpu %.1f\n", s.cpu.total);
    for (std::size_t i = 0; i < s.cpu.per_core.size(); ++i) std::fprintf(f, "core%zu %.1f\n", i, s.cpu.per_core[i]);
    if (s.cpu.freq_mhz.ok()) std::fprintf(f, "freq_mhz %.0f\n", s.cpu.freq_mhz.get());
    if (s.cpu.temp_c.ok()) std::fprintf(f, "cpu_temp %.1f\n", s.cpu.temp_c.get());
    std::fprintf(f, "mem_used %llu\nmem_total %llu\n", (unsigned long long)s.mem.used, (unsigned long long)s.mem.total);
    std::fprintf(f, "swap_used %llu\nswap_total %llu\n", (unsigned long long)(s.mem.swap_total - std::min(s.mem.swap_total, s.mem.swap_free)),
                 (unsigned long long)s.mem.swap_total);
    std::set<std::string> written;
    for (const auto& t : s.thermals) {
        if (t.chip == "coretemp" && t.label.find("Package") == std::string::npos) continue;
        std::string key = t.chip;
        std::replace(key.begin(), key.end(), ' ', '_');
        if (!written.insert(key).second) continue;  // first sensor of each chip, like conky's hwmon lookup
        std::fprintf(f, "temp_%s %.1f\n", key.c_str(), t.celsius);
    }
    for (const auto& fan : s.power.fans) {
        std::string key = fan.label;
        std::replace(key.begin(), key.end(), ' ', '_');
        if (fan.percent.ok()) std::fprintf(f, "fan_%s %d\n", key.c_str(), fan.percent.get());
        else if (fan.rpm.ok()) std::fprintf(f, "fan_%s_rpm %d\n", key.c_str(), fan.rpm.get());
    }
    for (std::size_t i = 0; i < s.gpus.size(); ++i) {
        const auto& g = s.gpus[i];
        std::fprintf(f, "gpu%zu_asleep %d\n", i, g.asleep ? 1 : 0);
        if (g.util.ok()) std::fprintf(f, "gpu%zu_util %.0f\n", i, g.util.get());
        if (g.mem_used.ok()) std::fprintf(f, "gpu%zu_mem_used %.0f\n", i, g.mem_used.get());
        if (g.mem_total.ok()) std::fprintf(f, "gpu%zu_mem_total %.0f\n", i, g.mem_total.get());
        if (g.temp_c.ok()) std::fprintf(f, "gpu%zu_temp %.0f\n", i, g.temp_c.get());
    }
    // The discrete NVIDIA card in nvidia-smi's units, so conky can stop
    // running nvidia-smi (which also kept the card from ever sleeping).
    for (const auto& g : s.gpus) {
        if (g.vendor != "NVIDIA") continue;
        std::fprintf(f, "nvidia_asleep %d\n", g.asleep ? 1 : 0);
        if (g.util.ok()) std::fprintf(f, "nvidia_util %.0f\n", g.util.get());
        if (g.mem_used.ok()) std::fprintf(f, "nvidia_mem_used_mb %.0f\n", g.mem_used.get() / 1048576.0);
        if (g.mem_total.ok()) std::fprintf(f, "nvidia_mem_total_mb %.0f\n", g.mem_total.get() / 1048576.0);
        if (g.temp_c.ok()) std::fprintf(f, "nvidia_temp %.0f\n", g.temp_c.get());
        break;
    }
    double rd = 0, wr = 0, rx = 0, tx = 0;
    for (const auto& d : s.disks) rd += d.read_bps, wr += d.write_bps;
    for (const auto& n : s.nets)
        if (n.kind != "Loopback" && n.kind != "Virtual") rx += n.rx_bps, tx += n.tx_bps;
    std::fprintf(f, "disk_read %.0f\ndisk_write %.0f\nnet_rx %.0f\nnet_tx %.0f\n", rd, wr, rx, tx);

    std::vector<const ProcessInfo*> top;
    for (const auto& p : s.procs)
        if (!p.kernel_thread) top.push_back(&p);
    std::partial_sort(top.begin(), top.begin() + std::min<std::size_t>(8, top.size()), top.end(),
                      [](auto a, auto b) { return a->cpu > b->cpu; });
    for (std::size_t i = 0; i < std::min<std::size_t>(8, top.size()); ++i) {
        double mem = s.mem.total ? 100.0 * double(top[i]->rss) / double(s.mem.total) : 0;
        std::fprintf(f, "top%zu %.1f %.1f %s\n", i + 1, top[i]->cpu, mem, top[i]->name.c_str());
    }
    std::fclose(f);
    std::rename(tmp.c_str(), (dir + "/conky.txt").c_str());
}

}  // namespace as
