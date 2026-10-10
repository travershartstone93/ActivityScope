#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "reading.h"

namespace as {

struct CpuSample {
    std::string model;
    int logical = 0;
    int physical = 0;
    int sockets = 0;
    double total = 0;                 // percent of the whole machine
    double kernel = 0;                // part of total spent in the kernel
    std::vector<double> per_core;     // percent per logical processor
    Reading<double> freq_mhz;         // average current clock
    Reading<double> max_mhz;
    Reading<double> base_mhz;
    Reading<double> temp_c;           // package temperature
    std::string governor;
    std::string driver;
    std::string epp;                  // energy-performance preference
    Reading<std::string> cache_l1, cache_l2, cache_l3;
    bool virtualization = false;
    bool hypervisor = false;
    double load1 = 0, load5 = 0, load15 = 0;
};

struct MemorySample {
    uint64_t total = 0, available = 0, used = 0;
    uint64_t cached = 0, buffers = 0, shared = 0, dirty = 0;
    uint64_t swap_total = 0, swap_free = 0;
    uint64_t committed = 0, commit_limit = 0;
    uint64_t zram_orig = 0, zram_compr = 0;  // compressed swap in RAM
    Reading<double> pressure_some10;         // % of time some task waited on memory
    Reading<double> pressure_full10;
    uint64_t page_ins = 0, page_outs = 0;
    double percent() const { return total ? 100.0 * double(used) / double(total) : 0; }
};

struct DiskSample {
    std::string name, model, kind;  // kind: NVMe SSD, SSD, HDD, removable
    uint64_t size = 0;
    double read_bps = 0, write_bps = 0;
    double busy = 0;                 // percent of time the device had I/O in flight
    Reading<double> temp_c;
};

struct NetSample {
    std::string name, kind;          // kind: Wi-Fi, Ethernet, Virtual, Loopback
    bool up = false;
    double rx_bps = 0, tx_bps = 0;
    uint64_t rx_total = 0, tx_total = 0;
    Reading<int> speed_mbps;
    std::string ipv4, ipv6, mac;
    Reading<std::string> ssid;
};

struct GpuSample {
    std::string name, vendor, driver, pci;
    bool asleep = false;             // runtime-suspended; not woken to measure
    Reading<double> util;
    Reading<double> mem_used, mem_total;  // bytes
    Reading<double> temp_c;
    Reading<double> power_w;
    Reading<double> clock_mhz;
};

struct Thermal {
    std::string chip, label;
    double celsius = 0;
    Reading<double> critical;
};

struct FanSample {
    std::string label;
    Reading<int> rpm;
    Reading<int> percent;
};

// The sensor the kernel uses to force a thermal shutdown, and how close it is.
struct GuardSample {
    std::string zone;                 // e.g. "acpitz"
    Reading<double> temp;             // median of its last 3 readings, so a one-second blip is ignored
    Reading<double> raw;              // the latest reading as-is
    Reading<double> limit;            // its critical trip point
    Reading<double> margin;           // limit - temp
    Reading<double> rate;             // degrees per second over the last 15 s
    Reading<double> seconds_left;     // at the current climb rate
    std::vector<std::string> dead_fans;  // fans that stay still while the machine is hot
};

struct PowerSample {
    Reading<bool> on_ac;
    Reading<double> battery_pct;
    Reading<double> battery_w;       // positive while discharging
    std::string battery_state;
    Reading<double> battery_health;  // full charge vs design, percent
    Reading<double> cpu_package_w;
    std::string profile;             // platform power profile
    std::vector<FanSample> fans;
};

// A process is identified by its pid AND its start time, so a recycled pid
// can never be mistaken for the process that used to own it.
struct ProcessKey {
    int pid = 0;
    uint64_t start_ticks = 0;
    bool operator==(const ProcessKey&) const = default;
};

struct ProcessInfo {
    ProcessKey key;
    int ppid = 0;
    int uid = -1;
    std::string name, comm, exe, cmdline, user, state, unit;
    double cpu = 0;                  // percent of the whole machine
    uint64_t rss = 0;                // resident bytes
    uint64_t swap = 0;
    Reading<double> read_bps, write_bps;
    int threads = 0;
    int nice = 0;
    double cpu_seconds = 0;
    uint64_t major_faults = 0;
    double started_epoch = 0;        // seconds since 1970
    Reading<double> gpu_mem;         // bytes, NVIDIA only
    bool kernel_thread = false;
    bool self = false;
    bool is_new = false;             // first seen this sample
    double child_cpu = 0;            // percent of the machine used by children reaped this interval
    std::string cgroup;              // full cgroup v2 path
};

// A systemd unit or app scope, measured from cgroup v2 accounting. Processes
// that already exited stay counted here, which is how short-lived work is seen.
struct UnitSample {
    std::string path, name;
    double cpu = 0;                  // percent of the whole machine
};

struct Snapshot {
    uint64_t generation = 0;
    double mono = 0;                 // seconds, monotonic
    double wall = 0;                 // seconds since 1970
    double uptime = 0;
    CpuSample cpu;
    MemorySample mem;
    std::vector<DiskSample> disks;
    std::vector<NetSample> nets;
    std::vector<GpuSample> gpus;
    std::vector<Thermal> thermals;
    PowerSample power;
    GuardSample guard;
    std::vector<ProcessInfo> procs;
    std::vector<ProcessInfo> exited;  // gone since the last sample, with final totals
    std::vector<UnitSample> units;
    int new_procs = 0;
    bool procs_reused = false;        // process list carried over from the previous tick
    int threads_total = 0;
    std::vector<std::string> problems;  // providers that failed this round
    std::vector<std::pair<std::string, double>> timings;  // milliseconds per provider
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

}  // namespace as
