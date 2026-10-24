#pragma once
#include <string>
#include <vector>

#include "events.h"
#include "logbook.h"
#include "netlog.h"
#include "processlog.h"
#include "snapshot.h"

struct sqlite3;

namespace as {

struct StoredSample {
    double t = 0, cpu = 0, kernel = 0, mem_pct = 0, swap_used = 0, pressure = 0;
    double disk_r = 0, disk_w = 0, net_rx = 0, net_tx = 0, gpu = -1, gpu_mem = -1;
    double temp = -1, fan = -1, power = -1, freq = -1, guard = -1, guard_limit = -1;
    double battery_pct = -1, on_ac = -1, disk_free_pct = -1;
    double swap_total = -1;  // bytes of swap, 0 for none; -1 not recorded (rows from before it was kept)
    int procs = 0;
    std::string cores;  // comma-separated per-core percentages
};

// One metric's hour, kept long after the raw seconds are pruned. The judge
// handlers compare like with like: window medians against window medians.
struct HourlyRow {
    int64_t hour_ts = 0;  // start of the hour, unix seconds
    std::string metric;
    int64_t n = 0;        // valid samples behind the stats
    double med = 0, p05 = 0, p95 = 0, max = 0;
    double min = -1;               // -1: rolled up before the lowest value was kept
    int64_t dropouts = 0;          // sentinel readings filtered out, counted
    double on_battery_frac = -1;   // share of the hour spent on battery; -1 unknown
};

struct EventRow {
    int64_t id = 0;
    double start = 0, end = 0;
    unsigned kinds = 0, primary = 0;
    std::string triggers, summary;
    double peak_cpu = 0, peak_mem = 0, peak_disk = 0, peak_gpu = -1, peak_temp = -1, peak_fan = -1;
    double min_margin = -1;
    bool notable = true;
};

class Store {
public:
    explicit Store(std::string path);
    ~Store();
    bool ok() const { return db_ != nullptr; }
    const std::string& error() const { return error_; }

    void add_sample(const Snapshot& s);
    void add_minute(const MinuteTop& m);
    int64_t add_event(const Event& e);
    void flush();   // writes queued samples in one transaction
    void prune(double now, double detail_hours = 24, double event_days = 30);
    // Folds completed hours of samples into samples_hourly; call before prune.
    void rollup_hours(double now);

    std::vector<StoredSample> samples(double from, double to) const;
    std::vector<HourlyRow> hourly(const std::string& metric, double from, double to) const;
    std::vector<EventRow> events(double from, double to) const;
    Event event(int64_t id) const;
    std::vector<Culprit> minute_top(double from, double to) const;
    StoredSample last_sample_before(double t) const;

    // The investigation log.
    void add_log(const std::vector<LogEntry>& entries);
    std::vector<LogEntry> log(double from, double to, const std::string& search = {}, int limit = 5000) const;
    std::string meta(const std::string& key) const;
    void set_meta(const std::string& key, const std::string& value);
    bool exe_seen(const std::string& exe) const;
    void mark_exe(const std::string& exe);
    void prune_log(double now, double days);

    // Every program that ran, and who connected where.
    void add_procs(const std::vector<ProcessRecord>& records);
    double last_sample_time() const;
    int close_stale_procs(const std::vector<ProcessRecord>& live, double fallback_end);
    // Returns the records that were never seen before (by program, address, port).
    std::vector<ConnectionRecord> add_connections(const std::vector<ConnectionRecord>& records);
    bool boot_seen(const std::string& id) const;
    void mark_boot(const std::string& id);

private:
    void exec(const char* sql);
    sqlite3* db_ = nullptr;
    std::string error_;
    std::vector<StoredSample> pending_;
};

std::string default_db_path();

}  // namespace as
