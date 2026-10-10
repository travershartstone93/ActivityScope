#include "guard.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

}  // namespace as
