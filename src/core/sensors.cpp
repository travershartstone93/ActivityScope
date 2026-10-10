#include "providers.h"

namespace as {

void SensorProvider::sample(std::vector<Thermal>& thermals, PowerSample& p, double now) {
    const std::string hw = r_.sys + "/class/hwmon";
    // CPU sensors are cheap register reads; drives, Wi-Fi and ACPI zones go
    // through firmware, so they refresh every 5 s.
    const bool slow_due = now - slow_thermals_t_ >= 5;
    if (slow_due) slow_thermals_.clear();
    if (now - chips_t_ >= 30) {
        chips_.clear();
        for (const auto& h : list_dir(hw)) {
            Chip c;
            c.base = hw + "/" + h;
            c.name = read_line(c.base + "/name").value_or(h);
            for (int i = 1; i < 64; ++i) {
                if (exists(c.base + "/temp" + std::to_string(i) + "_input")) c.temps.push_back(i);
                if (i < 16 && exists(c.base + "/fan" + std::to_string(i) + "_input")) c.fans.push_back(i);
            }
            chips_.push_back(std::move(c));
        }
        chips_t_ = now;
    }
    for (const auto& ch : chips_) {
        const std::string& base = ch.base;
        const std::string& chip = ch.name;
        bool fast = chip == "coretemp" || chip == "k10temp" || chip == "zenpower" || starts_with(chip, "pch_");
        if (!fast && !slow_due) continue;
        for (int i : ch.temps) {
            const std::string t = base + "/temp" + std::to_string(i);
            auto v = read_int(t + "_input");
            if (!v) continue;
            Thermal th;
            th.chip = chip;
            auto label = labels_.find(t);
            if (label == labels_.end()) label = labels_.emplace(t, read_line(t + "_label").value_or("Sensor " + std::to_string(i))).first;
            th.label = label->second;
            th.celsius = *v / 1000.0;
            auto cached = crit_.find(t);
            if (cached == crit_.end()) {
                auto c = read_int(t + "_crit");
                cached = crit_.emplace(t, c && *c > 0 ? Reading<double>::of(*c / 1000.0) : Reading<double>::none("No limit reported")).first;
            }
            th.critical = cached->second;
            (fast ? thermals : slow_thermals_).push_back(std::move(th));
        }
        for (int i : ch.fans) {
            const std::string f = base + "/fan" + std::to_string(i);
            auto rpm = read_int(f + "_input");
            if (!rpm) continue;
            FanSample fan;
            fan.label = read_line(f + "_label").value_or(chip + " fan " + std::to_string(i));
            fan.rpm = Reading<int>::of(int(*rpm));
            fan.percent = Reading<int>::none("Not reported");
            p.fans.push_back(std::move(fan));
        }
    }

    // MSI laptops: the msi-ec driver reports fan speed as a percentage.
    const std::string msi = r_.sys + "/devices/platform/msi-ec";
    for (const char* which : {"cpu", "gpu"}) {
        if (auto pct = read_int(msi + "/" + which + "/realtime_fan_speed")) {
            FanSample fan;
            fan.label = std::string(which) == "cpu" ? "CPU fan" : "GPU fan";
            fan.rpm = Reading<int>::none("This driver reports percent, not RPM");
            fan.percent = Reading<int>::of(int(*pct));
            p.fans.push_back(std::move(fan));
        }
    }

    if (slow_due) slow_thermals_t_ = now;
    thermals.insert(thermals.end(), slow_thermals_.begin(), slow_thermals_.end());

    // The battery is read through ACPI firmware calls that take ~100 ms, and
    // it changes slowly, so every 10 s is plenty.
    if (now - battery_t_ < 10) {
        p.on_ac = battery_.on_ac;
        p.battery_pct = battery_.battery_pct;
        p.battery_w = battery_.battery_w;
        p.battery_state = battery_.battery_state;
        p.battery_health = battery_.battery_health;
    } else {
    const std::string ps = r_.sys + "/class/power_supply";
    p.on_ac = Reading<bool>::none("No mains adapter found");
    p.battery_pct = p.battery_w = p.battery_health = Reading<double>::none("No battery found");
    for (const auto& s : list_dir(ps)) {
        const std::string base = ps + "/" + s;
        auto type = read_line(base + "/type").value_or("");
        if (type == "Mains") {
            p.on_ac = Reading<bool>::of(read_int(base + "/online").value_or(0) == 1);
        } else if (type == "Battery" && read_line(base + "/scope").value_or("") != "Device") {
            if (auto c = read_int(base + "/capacity")) p.battery_pct = Reading<double>::of(double(*c));
            p.battery_state = read_line(base + "/status").value_or("");
            double watts = -1;
            if (auto pw = read_int(base + "/power_now")) watts = *pw / 1e6;
            else if (auto cur = read_int(base + "/current_now"), volt = read_int(base + "/voltage_now"); cur && volt)
                watts = (double(*cur) / 1e6) * (double(*volt) / 1e6);
            if (watts >= 0) p.battery_w = Reading<double>::of(p.battery_state == "Charging" ? -watts : watts);
            else p.battery_w = Reading<double>::none("The battery does not report its draw");
            auto full = read_int(base + "/energy_full");
            auto design = read_int(base + "/energy_full_design");
            if (!full || !design) {
                full = read_int(base + "/charge_full");
                design = read_int(base + "/charge_full_design");
            }
            if (full && design && *design > 0) p.battery_health = Reading<double>::of(100.0 * double(*full) / double(*design));
            else p.battery_health = Reading<double>::none("Not reported");
        }
    }
    battery_ = p;
    battery_t_ = now;
    }

    // CPU package power: root-only since Linux 5.10, so it is often unavailable.
    const std::string rapl = r_.sys + "/class/powercap/intel-rapl:0/energy_uj";
    if (!exists(rapl)) {
        p.cpu_package_w = Reading<double>::none("This processor has no power counter");
    } else if (auto e = read_int(rapl)) {
        if (prev_energy_t_ > 0 && uint64_t(*e) >= prev_energy_uj_)
            p.cpu_package_w = Reading<double>::of(double(uint64_t(*e) - prev_energy_uj_) / 1e6 / (now - prev_energy_t_));
        else
            p.cpu_package_w = Reading<double>::none("Measuring");
        prev_energy_uj_ = uint64_t(*e);
        prev_energy_t_ = now;
    } else {
        p.cpu_package_w = Reading<double>::none("Power access restricted: the kernel lets only root read it");
    }

    if (auto prof = read_line(r_.sys + "/firmware/acpi/platform_profile")) p.profile = *prof;
    else if (auto shift = read_line(msi + "/shift_mode")) p.profile = *shift;
}

}  // namespace as
