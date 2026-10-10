#include <set>

#include "providers.h"

namespace as {

namespace {

std::string cache_text(const std::string& base, int level, const std::string& want_type) {
    for (const auto& idx : list_dir(base)) {
        if (!starts_with(idx, "index")) continue;
        auto lv = read_int(base + "/" + idx + "/level");
        auto ty = read_line(base + "/" + idx + "/type");
        auto sz = read_line(base + "/" + idx + "/size");
        if (lv && *lv == level && ty && sz && (want_type.empty() || *ty == want_type || *ty == "Unified"))
            return *sz;
    }
    return {};
}

std::string human_cache(const std::string& s) {
    // sysfs writes sizes like "32K" or "12288K".
    if (s.empty()) return s;
    auto n = to_int(std::string_view(s).substr(0, s.size() - 1));
    if (!n) return s;
    if (s.back() == 'K' && *n >= 1024 && *n % 1024 == 0) return std::to_string(*n / 1024) + " MB";
    if (s.back() == 'K') return std::to_string(*n) + " KB";
    return s;
}

}  // namespace

CpuProvider::CpuProvider(Roots r) : r_(std::move(r)) {
    auto& s = static_;
    if (auto info = read_file(r_.proc + "/cpuinfo")) {
        for (const auto& line : split(*info, '\n')) {
            if (s.model.empty() && starts_with(line, "model name")) s.model = trim(line.substr(line.find(':') + 1));
            if (starts_with(line, "flags")) {
                s.virtualization = line.find(" vmx") != std::string::npos || line.find(" svm") != std::string::npos;
                s.hypervisor = line.find(" hypervisor") != std::string::npos;
            }
        }
    }
    std::set<std::string> cores, sockets;
    const std::string cpus = r_.sys + "/devices/system/cpu";
    for (const auto& n : list_dir(cpus)) {
        if (n.size() < 4 || !starts_with(n, "cpu") || !to_int(std::string_view(n).substr(3))) continue;
        ++s.logical;
        auto pkg = read_line(cpus + "/" + n + "/topology/physical_package_id");
        auto sib = read_line(cpus + "/" + n + "/topology/core_cpus_list");
        if (!sib) sib = read_line(cpus + "/" + n + "/topology/thread_siblings_list");
        cores.insert(pkg.value_or("0") + ":" + sib.value_or(n));
        sockets.insert(pkg.value_or("0"));
    }
    s.physical = static_cast<int>(cores.size());
    s.sockets = static_cast<int>(sockets.size());

    const std::string c0 = cpus + "/cpu0/cpufreq";
    if (auto v = read_int(c0 + "/cpuinfo_max_freq")) s.max_mhz = Reading<double>::of(*v / 1000.0);
    else s.max_mhz = Reading<double>::none("The kernel does not report it");
    if (auto v = read_int(c0 + "/base_frequency")) s.base_mhz = Reading<double>::of(*v / 1000.0);
    else s.base_mhz = Reading<double>::none("The frequency driver does not report it");
    s.governor = read_line(c0 + "/scaling_governor").value_or("");
    s.driver = read_line(c0 + "/scaling_driver").value_or("");
    s.epp = read_line(c0 + "/energy_performance_preference").value_or("");

    const std::string cache = cpus + "/cpu0/cache";
    auto l1d = human_cache(cache_text(cache, 1, "Data"));
    auto l1i = human_cache(cache_text(cache, 1, "Instruction"));
    auto l2 = human_cache(cache_text(cache, 2, ""));
    auto l3 = human_cache(cache_text(cache, 3, ""));
    auto some = [](const std::string& v, const std::string& suffix) {
        return v.empty() ? Reading<std::string>::none("Not reported") : Reading<std::string>::of(v + suffix);
    };
    s.cache_l1 = l1d.empty() ? some("", "") : some(l1d + " data + " + l1i + " instruction", " per core");
    s.cache_l2 = some(l2, " per core");
    s.cache_l3 = some(l3, " shared");

    // Package temperature: coretemp (Intel), k10temp or zenpower (AMD).
    const std::string hw = r_.sys + "/class/hwmon";
    for (const auto& h : list_dir(hw)) {
        auto name = read_line(hw + "/" + h + "/name").value_or("");
        if (name != "coretemp" && name != "k10temp" && name != "zenpower") continue;
        for (int i = 1; i < 64 && temp_path_.empty(); ++i) {
            auto label = read_line(hw + "/" + h + "/temp" + std::to_string(i) + "_label");
            if (!label) continue;
            if (starts_with(*label, "Package") || *label == "Tctl" || *label == "Tdie")
                temp_path_ = hw + "/" + h + "/temp" + std::to_string(i) + "_input";
        }
        if (temp_path_.empty() && exists(hw + "/" + h + "/temp1_input")) temp_path_ = hw + "/" + h + "/temp1_input";
    }
}

void CpuProvider::sample(CpuSample& out) {
    out = static_;
    auto stat = read_file(r_.proc + "/stat");
    if (!stat) throw std::runtime_error("cannot read /proc/stat");

    std::vector<Times> now;
    for (const auto& line : split(*stat, '\n')) {
        if (!starts_with(line, "cpu")) break;
        auto f = split_ws(line);
        if (f.size() < 8) continue;
        uint64_t v[8] = {};
        for (int i = 0; i < 8; ++i) v[i] = to_uint(f[i + 1]).value_or(0);
        // user nice system idle iowait irq softirq steal
        Times t;
        t.total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
        t.busy = t.total - v[3] - v[4];
        t.kernel = v[2] + v[5] + v[6];
        now.push_back(t);
    }
    auto pct = [](uint64_t a, uint64_t b, uint64_t total) {
        return total ? 100.0 * double(a - b) / double(total) : 0.0;
    };
    if (prev_.size() == now.size()) {
        for (std::size_t i = 0; i < now.size(); ++i) {
            uint64_t dt = now[i].total - prev_[i].total;
            double busy = pct(now[i].busy, prev_[i].busy, dt);
            if (i == 0) {
                out.total = busy;
                out.kernel = pct(now[i].kernel, prev_[i].kernel, dt);
            } else {
                out.per_core.push_back(busy);
            }
        }
    } else {
        out.per_core.assign(now.empty() ? 0 : now.size() - 1, 0.0);
    }
    prev_ = std::move(now);

    double sum = 0;
    int n = 0;
    const std::string cpus = r_.sys + "/devices/system/cpu";
    for (int i = 0; i < out.logical; ++i) {
        if (auto v = read_int(cpus + "/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq")) {
            sum += *v / 1000.0;
            ++n;
        }
    }
    out.freq_mhz = n ? Reading<double>::of(sum / n) : Reading<double>::none("No frequency driver is loaded");

    if (temp_path_.empty()) out.temp_c = Reading<double>::none("No CPU temperature sensor found");
    else if (auto v = read_int(temp_path_)) out.temp_c = Reading<double>::of(*v / 1000.0);
    else out.temp_c = Reading<double>::none("The sensor stopped answering");

    if (auto la = read_line(r_.proc + "/loadavg")) {
        auto f = split_ws(*la);
        if (f.size() >= 3) {
            out.load1 = std::stod(std::string(f[0]));
            out.load5 = std::stod(std::string(f[1]));
            out.load15 = std::stod(std::string(f[2]));
        }
    }
}

}  // namespace as
