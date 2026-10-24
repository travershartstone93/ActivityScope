#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "fs.h"
#include "snapshot.h"

namespace as {

class CpuProvider {
public:
    explicit CpuProvider(Roots r);
    void sample(CpuSample& out);

private:
    struct Times { uint64_t busy = 0, kernel = 0, total = 0; };
    Roots r_;
    CpuSample static_;
    std::vector<Times> prev_;  // index 0 is the aggregate line
    std::string temp_path_;
};

class MemoryProvider {
public:
    explicit MemoryProvider(Roots r) : r_(std::move(r)) {}
    void sample(MemorySample& out);

private:
    Roots r_;
};

class DiskProvider {
public:
    explicit DiskProvider(Roots r) : r_(std::move(r)) {}
    void sample(std::vector<DiskSample>& out, double now);

private:
    struct Prev { uint64_t rd = 0, wr = 0, ticks = 0; };
    Roots r_;
    std::map<std::string, Prev> prev_;
    std::map<std::string, Reading<double>> temps_;
    double temps_t_ = -1e9;
    double prev_t_ = 0;
};

class NetProvider {
public:
    explicit NetProvider(Roots r) : r_(std::move(r)) {}
    void sample(std::vector<NetSample>& out, double now);

private:
    struct Prev { uint64_t rx = 0, tx = 0; };
    Roots r_;
    std::map<std::string, Prev> prev_;
    std::map<std::string, std::pair<std::string, std::string>> addrs_;
    double addrs_t_ = -1e9;
    double prev_t_ = 0;
};

struct Nvml;

class GpuProvider {
public:
    explicit GpuProvider(Roots r);
    ~GpuProvider();
    // Fills GPU samples, plus video memory per pid where the driver says.
    void sample(std::vector<GpuSample>& out, std::unordered_map<int, double>& gpu_mem_by_pid,
                double now);

private:
    Roots r_;
    std::unique_ptr<Nvml> nvml_;
    bool nvml_open_ = false;
    std::map<std::string, GpuSample> last_;
    double full_t_ = -1e9;
    int last_users_ = 0;
    double users_t_ = -1e9;
    std::unordered_map<int, double> last_by_pid_;
    int idle_streak_ = 0;
    double busy_until_ = 0;  // another program kept it awake through a rest: measure until then
    std::string nvml_name_;
    double slow_t_ = -1e9;
    Reading<double> slow_power_, slow_clock_;  // another program keeps the card awake, so resting gains nothing
    double rest_until_ = 0;
};

class SensorProvider {
public:
    explicit SensorProvider(Roots r) : r_(std::move(r)) {}
    void sample(std::vector<Thermal>& thermals, PowerSample& power, double now);

private:
    Roots r_;
    uint64_t prev_energy_uj_ = 0;
    double prev_energy_t_ = 0;
    // Some sensors are slow to read (the battery asks the laptop's firmware),
    // so they are refreshed less often and reused in between.
    std::vector<Thermal> slow_thermals_;
    double slow_thermals_t_ = -1e9;
    PowerSample battery_;
    double battery_t_ = -1e9;
    std::map<std::string, Reading<double>> crit_;
    struct Chip { std::string base, name; std::vector<int> temps, fans; };
    std::vector<Chip> chips_;
    std::map<std::string, std::string> labels_;
    double chips_t_ = -1e9;
};

class ProcessProvider {
public:
    explicit ProcessProvider(Roots r);
    // read_io: also read per-process disk counters (only worth it while disks are busy)
    void sample(Snapshot& snap, int logical_cpus, const std::unordered_map<int, double>& gpu_mem_by_pid, bool read_io = true);

private:
    struct Known {
        uint64_t ticks = 0, child_ticks = 0;
        uint64_t rd = 0, wr = 0;
        bool io_ok = false;
        ProcessInfo last;
    };
    Roots r_;
    std::unordered_map<int, Known> known_;
    std::unordered_map<int, uint64_t> carry_;  // ended children's ticks a parent has not shown yet
    std::unordered_map<int, std::string> users_;
    double prev_t_ = 0;
    double boot_epoch_ = 0;
    long hz_ = 100;
    long page_ = 4096;
    int self_ = 0;
    const std::string& user_name(int uid);
};

class CgroupProvider {
public:
    explicit CgroupProvider(Roots r) : r_(std::move(r)) {}
    void sample(std::vector<UnitSample>& out, double now, int logical_cpus);

private:
    struct Prev { uint64_t usage = 0; };
    void walk(const std::string& path, int depth, std::vector<std::string>& leaves);
    Roots r_;
    std::vector<std::string> leaves_;
    double leaves_t_ = -1e9;
    std::map<std::string, Prev> prev_;
    double prev_t_ = 0;
};

std::string pci_name(const std::string& vendor_hex, const std::string& device_hex);

}  // namespace as
