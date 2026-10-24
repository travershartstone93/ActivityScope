#include "guard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

#include "store.h"

namespace as {

void ThermalGuard::find_zone(double now) {
    // Trip points can change (firmware updates, user tweaks); re-read each minute.
    if (now - zone_t_ < 60 && !zone_path_.empty()) return;
    zone_t_ = now;
    const std::string base = r_.sys + "/class/thermal";
    double best = 1e9;
    for (const auto& z : list_dir(base)) {
        if (!starts_with(z, "thermal_zone")) continue;
        const std::string dir = base + "/" + z;
        if (read_line(dir + "/mode").value_or("enabled") == "disabled") continue;
        for (int i = 0; i < 32; ++i) {
            auto type = read_line(dir + "/trip_point_" + std::to_string(i) + "_type");
            if (!type) break;
            if (*type != "critical") continue;
            auto t = read_int(dir + "/trip_point_" + std::to_string(i) + "_temp");
            if (!t || *t <= 0) continue;
            // With several critical zones, the one with the least headroom matters.
            auto now_t = read_int(dir + "/temp");
            double headroom = *t / 1000.0 - (now_t ? *now_t / 1000.0 : 0);
            if (headroom < best) {
                best = headroom;
                zone_path_ = dir + "/temp";
                zone_name_ = read_line(dir + "/type").value_or(z);
                limit_ = Reading<double>::of(*t / 1000.0);
            }
        }
    }
}

void ThermalGuard::update(Snapshot& s) {
    GuardSample& g = s.guard;
    find_zone(s.mono);
    g.zone = zone_name_;
    g.limit = limit_;
    auto none = [&](const std::string& why) { g.temp = g.raw = g.margin = g.rate = g.seconds_left = Reading<double>::none(why); };
    if (zone_path_.empty()) {
        none("This system reports no critical thermal zone");
    } else if (auto t = read_int(zone_path_)) {
        g.raw = Reading<double>::of(*t / 1000.0);
        recent_.push_back(*t / 1000.0);
        if (recent_.size() > 3) recent_.pop_front();
        std::vector<double> sorted(recent_.begin(), recent_.end());
        std::sort(sorted.begin(), sorted.end());
        double c = sorted[sorted.size() / 2];
        g.temp = Reading<double>::of(c);
        g.margin = Reading<double>::of(limit_.get() - c);
        history_.emplace_back(s.wall, c);
        while (!history_.empty() && history_.front().first < s.wall - 15) history_.pop_front();
        // Least-squares slope over the last 15 s: steadier than two points.
        if (history_.size() >= 8) {
            double n = double(history_.size()), sx = 0, sy = 0, sxx = 0, sxy = 0;
            double t0 = history_.front().first;
            for (const auto& [ts, v] : history_) {
                double x = ts - t0;
                sx += x, sy += v, sxx += x * x, sxy += x * v;
            }
            double den = n * sxx - sx * sx;
            double slope = den > 1e-9 ? (n * sxy - sx * sy) / den : 0;
            g.rate = Reading<double>::of(slope);
            // This sensor is noisy (it can swing 10 °C between seconds), so a
            // countdown only means something close to the limit.
            if (g.margin.get() > 20) g.seconds_left = Reading<double>::none("Far from the limit");
            else if (slope > 0.05) g.seconds_left = Reading<double>::of(std::max(0.0, g.margin.get()) / slope);
            else g.seconds_left = Reading<double>::none("Not climbing");
        } else {
            g.rate = g.seconds_left = Reading<double>::none("Measuring");
        }
    } else {
        none("The shutdown sensor stopped answering");
    }

    // A fan that reads zero while another fan works hard and the CPU is hot
    // is not just idling.
    double busiest = 0;
    for (const auto& f : s.power.fans) {
        if (f.percent.ok()) busiest = std::max(busiest, double(f.percent.get()));
        else if (f.rpm.ok() && f.rpm.get() > 2500) busiest = std::max(busiest, 61.0);
    }
    double hot = std::max(s.cpu.temp_c.or_else(0), g.temp.or_else(0));
    for (const auto& f : s.power.fans) {
        bool still = (f.percent.ok() && f.percent.get() == 0) || (f.rpm.ok() && f.rpm.get() == 0);
        bool spinning = (f.percent.ok() && f.percent.get() > 0) || (f.rpm.ok() && f.rpm.get() > 0);
        int& secs = still_[f.label];
        if (spinning) secs = 0;  // it moved, so it works
        else if (still && busiest >= 60 && hot >= 75) secs = std::min(secs + 1, 1 << 20);
        if (secs >= kDeadFanSeconds) g.dead_fans.push_back(f.label);
    }
}

BootEndInfo classify_boot_end(const std::string& last_lines) {
    BootEndInfo info;
    std::string last_sleep, last_wake;
    for (const auto& line : split(last_lines, '\n')) {
        if (line.find("critical temperature reached") != std::string::npos ||
            line.find("HARDWARE PROTECTION shutdown") != std::string::npos ||
            line.find("Hardware protection timed-out") != std::string::npos) {
            info.kind = BootEnd::Thermal;
            info.line = trim(line);
            return info;
        }
    }
    for (const auto& line : split(last_lines, '\n')) {
        for (const char* clean : {"System is powering down", "System is rebooting", "Journal stopped", "systemd-shutdown",
                                  "Reached target System Power Off", "Reached target System Reboot", "Reached target System Shutdown",
                                  "Power key pressed", "Unmounted /home", "Unmounted /boot", "Stopped target Graphical Interface",
                                  "Stopped target Multi-User System"}) {
            if (line.find(clean) != std::string::npos) {
                info.kind = BootEnd::Clean;
                info.line = trim(line);
            }
        }
        if (line.find("PM: suspend entry") != std::string::npos) last_sleep = line;
        if (line.find("PM: suspend exit") != std::string::npos) last_wake = line;
    }
    if (info.kind == BootEnd::Clean) return info;
    // The log ends inside a suspend that never woke up.
    if (!last_sleep.empty() && last_lines.rfind(last_sleep) > (last_wake.empty() ? 0 : last_lines.rfind(last_wake))) {
        info.kind = BootEnd::Asleep;
        info.line = trim(last_sleep);
        return info;
    }
    info.kind = BootEnd::Sudden;
    return info;
}

namespace {

std::string clock_date(double t) {
    time_t tt = time_t(t);
    tm lt{};
    localtime_r(&tt, &lt);
    char buf[64];
    std::strftime(buf, sizeof buf, "%a %b %e at %H:%M:%S", &lt);
    return buf;
}

}  // namespace

Event shutdown_event(const Store& store, const BootEndInfo& info, double end, double next_start) {
    // The recorder's own last reading of that boot: its last one before the next boot began. The log can stop
    // writing a minute before the recorder does, so the stop is the later of the two.
    StoredSample last = store.last_sample_before(next_start);
    const bool read = last.t > 0 && end - last.t < 120;
    const double stop = read ? std::max(end, last.t) : end;
    Event e;
    e.start = e.end = e.last_active = stop;
    e.kinds = KindShutdown | (info.kind == BootEnd::Thermal ? KindHeat : 0u);
    e.primary = KindShutdown;
    e.open = false;
    if (!info.line.empty()) e.triggers.push_back(info.line.substr(info.line.find(' ') + 1));
    std::string when = clock_date(stop);
    std::string text;
    if (info.kind == BootEnd::Thermal)
        text = "The system shut itself down for heat on " + when + ": the kernel's shutdown sensor reached its critical temperature.";
    else if (info.kind == BootEnd::Asleep)
        text = "The machine went to sleep on " + when + " and never woke up; the battery may have run out.";
    else
        text = "The machine stopped without warning on " + when +
               ". Nothing was written to the system log first, which is what happens when the laptop's own firmware cuts the power, "
               "the battery dies, or the system freezes.";

    if (read) {
        char buf[256];
        const std::string ago = last.t >= end ? "The recorder read on past the log's last entry (" + clock_time(end) + "); its last reading"
                                              : "The last reading, " + std::to_string(int64_t(std::lround(end - last.t))) + " s before,";
        if (last.guard >= 0 && last.guard_limit > 0) {
            std::snprintf(buf, sizeof buf, " was %.0f °C on the shutdown sensor (%.0f °C below its %.0f °C limit), with the CPU %.0f%% busy.",
                          last.guard, last.guard_limit - last.guard, last.guard_limit, last.cpu);
            e.min_margin = std::max(0.0, last.guard_limit - last.guard);
            e.guard_limit = last.guard_limit;
        } else {
            std::snprintf(buf, sizeof buf, " had the CPU at %.0f °C and %.0f%% busy.", last.temp, last.cpu);
        }
        text += " " + ago + buf;
        double hottest = -1;  // the shutdown sensor at its closest to the limit
        for (const auto& x : store.samples(stop - 120, stop + 1)) {
            EventSample p;
            p.t = x.t;
            p.cpu = x.cpu;
            p.mem_pct = x.mem_pct;
            p.temp = x.temp;
            p.fan = x.fan;
            p.guard = x.guard;
            e.samples.push_back(p);
            e.peak_cpu = std::max(e.peak_cpu, x.cpu);
            e.peak_temp = std::max(e.peak_temp, x.temp);
            e.peak_fan = std::max(e.peak_fan, x.fan);
            if (x.guard >= 0 && x.guard_limit > 0) {
                double m = std::max(0.0, x.guard_limit - x.guard);
                if (e.min_margin < 0 || m < e.min_margin) e.min_margin = m, hottest = x.guard;
            }
        }
        // The last reading alone can be calm after a hot minute: the two minutes' worst is said too.
        if (hottest > last.guard)
            std::snprintf(buf, sizeof buf, " In its last two minutes the shutdown sensor reached %.0f °C (%.0f °C below its limit) and the CPU %.0f%% busy.",
                          hottest, e.min_margin, e.peak_cpu);
        else if (last.guard < 0 && e.peak_temp > last.temp)
            std::snprintf(buf, sizeof buf, " In its last two minutes the CPU reached %.0f °C and %.0f%% busy.", e.peak_temp, e.peak_cpu);
        else
            buf[0] = 0;
        text += buf;
        auto top = store.minute_top(stop - 120, stop + 1);
        double total = 0;
        for (const auto& c : top) total += c.cpu_seconds;
        for (auto& c : top) c.share = total > 0 ? c.cpu_seconds / total : 0;
        if (top.size() > 8) top.resize(8);
        if (!top.empty() && top.front().share >= 0.2) {
            std::snprintf(buf, sizeof buf, " The busiest program in its last two minutes was %s (%.0f%% of the CPU time).", top.front().name.c_str(),
                          top.front().share * 100);
            text += buf;
        }
        e.culprits = std::move(top);
    } else {
        text += " ActivityScope was not recording then, so there are no readings from just before.";
    }
    e.summary = std::string(clock_time(stop)) + " to " + clock_time(stop) + " (1 s). " + text;
    return e;
}

}  // namespace as
