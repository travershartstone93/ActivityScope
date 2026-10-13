#pragma once
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "snapshot.h"

namespace as {

// Thresholds for "this is worth remembering". Deliberately sensitive.
struct Triggers {
    double cpu_total = 50;      int cpu_total_secs = 2;
    double core = 95;           int core_secs = 3;
    double proc_cpu = 25;       int proc_secs = 3;
    int new_procs = 50;         int new_procs_window = 5;
    double pressure = 2;
    double swapin_kbps = 1024;  int swapin_secs = 2;
    double avail_drop_mb = 500;
    double proc_growth_mb = 300;
    int growth_window = 10;
    double disk_busy = 60;      double disk_mbps = 50;   int disk_secs = 3;
    double gpu = 50;            int gpu_secs = 3;        double vram_jump_mb = 500;
    double temp = 85;           double temp_rise = 25;   int temp_window = 30;
    double fan = 85;            double fan_step = 20;
    double shutdown_margin = 10;   // degrees below the kernel's shutdown point
    int preroll = 60;
    int calm = 15;
    int merge = 30;
};

enum Kind : unsigned { KindCpu = 1, KindMemory = 2, KindDisk = 4, KindGpu = 8, KindHeat = 16, KindShutdown = 32 };

struct EventSample {
    double t = 0, cpu = 0, mem_pct = 0, pressure = 0, disk_bps = 0, gpu = -1, temp = -1, fan = -1;
    double guard = -1;  // the shutdown sensor
};

struct Culprit {
    std::string kind = "process";  // process, group, unseen, unit
    std::string name, cmdline, parent, unit;
    int pid = 0, ppid = 0, count = 1;
    uint64_t start_ticks = 0;
    bool exited = false;
    double cpu_seconds = 0, child_cpu_seconds = 0, share = 0;
    uint64_t peak_rss = 0, first_rss = 0;
    int64_t rss_growth = 0;
    double io_bytes = 0, gpu_mem = 0;
    uint64_t first_faults = 0, faults = 0;  // major page faults: waits for swap or disk
    double first_seen = 0, last_seen = 0, started = 0;
};

struct Event {
    int64_t id = 0;
    double start = 0, end = 0, last_active = 0;
    unsigned kinds = 0;
    unsigned primary = 0;
    std::vector<std::string> triggers;
    double peak_cpu = 0, peak_mem_pct = 0, peak_pressure = 0, peak_disk_bps = 0;
    double peak_gpu = -1, peak_temp = -1, peak_fan = -1;
    double total_cpu_seconds = 0;
    // Closest approach to the kernel's thermal shutdown (-1: not measured).
    double min_margin = -1, min_seconds = -1, guard_limit = -1;
    std::string guard_zone;
    std::vector<std::string> dead_fans;
    std::vector<EventSample> samples;
    std::vector<Culprit> culprits;
    std::string summary;
    bool open = true;
};

struct MinuteTop {
    int64_t minute = 0;  // seconds since 1970, start of the minute
    std::vector<Culprit> top;
};

class EventEngine {
public:
    explicit EventEngine(Triggers t = {}) : t_(t) {}
    void set_triggers(const Triggers& t) { t_ = t; }
    const Triggers& triggers() const { return t_; }

    // Feeds one snapshot. Returns events that closed on this tick.
    std::vector<Event> feed(const SnapshotPtr& s);
    // The event in progress, with culprits and summary refreshed.
    std::optional<Event> open_event();
    std::optional<MinuteTop> take_minute();
    bool has_open() const { return ev_.has_value(); }
    const std::vector<std::string>& active_now() const { return active_now_; }

private:
    struct Acc {
        Culprit c;
        bool seen_new = false;
    };
    struct Active { unsigned kind; std::string id, text; };

    std::vector<Active> evaluate(const Snapshot& s);
    void accumulate(const Snapshot& s, double dt, double proc_dt);
    void begin(const SnapshotPtr& s, const std::vector<Active>& active);
    void note_guard(const Snapshot& s);
    Event finish(bool final_close);
    const Snapshot* ago(double seconds) const;
    EventSample point(const Snapshot& s) const;

    Triggers t_;
    std::deque<SnapshotPtr> ring_;
    std::map<std::string, int> streaks_;
    std::optional<Event> ev_;
    std::map<std::pair<int, uint64_t>, Acc> acc_;
    std::map<std::string, double> unit_cpu_;
    std::map<std::string, std::string> trigger_text_;
    std::vector<std::string> active_now_;
    double prev_mono_ = 0;
    double prev_scan_mono_ = 0;

    int64_t minute_ = 0;
    std::map<std::pair<int, uint64_t>, Culprit> minute_acc_;
    std::optional<MinuteTop> minute_ready_;
};

std::string describe(const Event& e);
// False for a blip: under 5 s, no program stood out, nowhere near shutdown.
bool worth_keeping(const Event& e);
std::string kind_names(unsigned kinds);
std::string clock_time(double epoch);
std::string human_bytes(double bytes);
std::string human_duration(double seconds);

}  // namespace as
