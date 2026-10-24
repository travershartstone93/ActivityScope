#include "sampler.h"

#include <sys/statvfs.h>

#include <ctime>
#include <exception>

namespace as {

double monotonic_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

double thread_cpu_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

double wall_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

Sampler::Sampler(Roots r)
    : r_(r), cpu_(r), mem_(r), disks_(r), nets_(r), gpus_(r), sensors_(r), procs_(r), cgroups_(r), guard_(r) {}

SnapshotPtr Sampler::sample(bool processes) {
    auto s = std::make_shared<Snapshot>();
    s->generation = ++generation_;
    s->mono = monotonic_seconds();
    s->wall = wall_seconds();
    if (auto up = read_line(r_.proc + "/uptime")) s->uptime = std::stod(up->substr(0, up->find(' ')));

    auto guard = [&](const char* what, auto&& fn) {
        double t0 = thread_cpu_seconds();
        try {
            fn();
        } catch (const std::exception& e) {
            s->problems.push_back(std::string(what) + ": " + e.what());
        }
        s->timings.emplace_back(what, (thread_cpu_seconds() - t0) * 1000);
    };
    guard("CPU", [&] { cpu_.sample(s->cpu); });
    guard("Memory", [&] { mem_.sample(s->mem); });
    guard("Disks", [&] { disks_.sample(s->disks, s->mono); });
    guard("Network", [&] { nets_.sample(s->nets, s->mono); });
    std::unordered_map<int, double> gpu_mem;
    guard("GPU", [&] { gpus_.sample(s->gpus, gpu_mem, s->mono); });
    guard("Sensors", [&] { sensors_.sample(s->thermals, s->power, s->mono); });
    guard("Shutdown watch", [&] { guard_.update(*s); });
    // Root filesystem free space, for the fill-rate history. statvfs is cheap
    // but a minute's staleness costs nothing, so it is cached.
    if (s->mono - last_statvfs_ >= 60) {
        struct statvfs vfs{};
        root_free_ = statvfs("/", &vfs) == 0 && vfs.f_blocks > 0
                         ? Reading<double>::of(100.0 * double(vfs.f_bavail) / double(vfs.f_blocks))
                         : Reading<double>::none("statvfs failed");
        last_statvfs_ = s->mono;
    }
    s->root_free_pct = root_free_;
    int cpus = std::max(1, s->cpu.logical);
    // Scanning every process is the biggest cost. While the machine is calm a
    // 3 s scan loses nothing (CPU is averaged over the gap, and reaped
    // children still show in their parent); once it gets busy, every second.
    // A machine with a lot in swap shows a little memory pressure all the
    // time, so only real pressure counts as busy.
    bool busy = s->cpu.total > 25 || s->mem.pressure_some10.or_else(0) > 5 || !last_;
    if (busy || processes || s->mono - last_proc_scan_ >= 2.9) {
        double disk_bps = 0;
        for (const auto& d : s->disks) disk_bps += d.read_bps + d.write_bps;
        guard("Processes", [&] { procs_.sample(*s, cpus, gpu_mem, disk_bps > 512e3); });
        last_proc_scan_ = s->mono;
    } else {
        s->procs = last_->procs;
        s->threads_total = last_->threads_total;
        s->procs_reused = true;
    }
    // Unit counters are cumulative, so reading them with the process scan
    // (every 3 s when calm) loses nothing.
    if (!last_ || busy || s->mono - last_units_ >= 2.9) {
        guard("Units", [&] { cgroups_.sample(s->units, s->mono, cpus); });
        last_units_ = s->mono;
    } else {
        s->units = last_->units;
    }
    last_ = s;
    return s;
}

}  // namespace as
