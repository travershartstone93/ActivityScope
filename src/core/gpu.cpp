#include <dlfcn.h>

#include <fstream>
#include <mutex>

#include "providers.h"

namespace as {

// The few NVML calls we need, loaded at run time so the program also builds
// and runs on machines without NVIDIA's driver.
struct Nvml {
    using Dev = void*;
    struct Util { unsigned gpu, memory; };
    struct Mem { unsigned long long total, free, used; };
    struct Proc { unsigned pid; unsigned long long used; unsigned gi, ci; };

    void* lib = nullptr;
    int (*init)() = nullptr;
    int (*shutdown)() = nullptr;
    int (*by_pci)(const char*, Dev*) = nullptr;
    int (*name)(Dev, char*, unsigned) = nullptr;
    int (*util)(Dev, Util*) = nullptr;
    int (*mem)(Dev, Mem*) = nullptr;
    int (*temp)(Dev, int, unsigned*) = nullptr;
    int (*power)(Dev, unsigned*) = nullptr;
    int (*clock)(Dev, int, unsigned*) = nullptr;
    int (*compute)(Dev, unsigned*, Proc*) = nullptr;
    int (*graphics)(Dev, unsigned*, Proc*) = nullptr;
    std::string driver;

    bool load() {
        lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        auto sym = [&](auto& fn, const char* a, const char* b = nullptr) {
            void* p = dlsym(lib, a);
            if (!p && b) p = dlsym(lib, b);
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
            return p != nullptr;
        };
        bool ok = sym(init, "nvmlInit_v2") && sym(shutdown, "nvmlShutdown") &&
                  sym(by_pci, "nvmlDeviceGetHandleByPciBusId_v2") && sym(name, "nvmlDeviceGetName") &&
                  sym(util, "nvmlDeviceGetUtilizationRates") && sym(mem, "nvmlDeviceGetMemoryInfo");
        sym(temp, "nvmlDeviceGetTemperature");
        sym(power, "nvmlDeviceGetPowerUsage");
        sym(clock, "nvmlDeviceGetClockInfo");
        sym(compute, "nvmlDeviceGetComputeRunningProcesses_v3", "nvmlDeviceGetComputeRunningProcesses_v2");
        sym(graphics, "nvmlDeviceGetGraphicsRunningProcesses_v3", "nvmlDeviceGetGraphicsRunningProcesses_v2");
        return ok;
    }
    ~Nvml() {
        if (lib) dlclose(lib);
    }
};

std::string pci_name(const std::string& vendor_hex, const std::string& device_hex) {
    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    std::lock_guard lock(mu);
    const std::string key = vendor_hex + ":" + device_hex;
    if (auto it = cache.find(key); it != cache.end()) return it->second;

    std::string vendor_name, device_name;
    for (const char* path : {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids"}) {
        std::ifstream in(path);
        if (!in) continue;
        std::string line;
        bool in_vendor = false;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line[0] != '\t') {
                if (in_vendor) break;
                if (starts_with(line, vendor_hex + "  ")) {
                    in_vendor = true;
                    vendor_name = line.substr(vendor_hex.size() + 2);
                }
            } else if (in_vendor && line.size() > 1 && line[1] != '\t' &&
                       starts_with(std::string_view(line).substr(1), device_hex + "  ")) {
                device_name = line.substr(device_hex.size() + 3);
                break;
            }
        }
        break;
    }
    // "TU106M [GeForce RTX 2060 Mobile]" reads better as the part in brackets.
    if (auto a = device_name.find('['), b = device_name.rfind(']'); a != std::string::npos && b > a)
        device_name = device_name.substr(a + 1, b - a - 1);
    std::string short_vendor = vendor_name;
    if (vendor_hex == "10de") short_vendor = "NVIDIA";
    else if (vendor_hex == "8086") short_vendor = "Intel";
    else if (vendor_hex == "1002") short_vendor = "AMD";
    std::string out = device_name.empty() ? short_vendor + " graphics" : short_vendor + " " + device_name;
    cache[key] = out;
    return out;
}

GpuProvider::GpuProvider(Roots r) : r_(std::move(r)) {
    auto n = std::make_unique<Nvml>();
    if (r_.sys == "/sys" && n->load()) nvml_ = std::move(n);
}

GpuProvider::~GpuProvider() {
    if (nvml_ && nvml_open_) nvml_->shutdown();
}

void GpuProvider::sample(std::vector<GpuSample>& out, std::unordered_map<int, double>& by_pid, double now) {
    const std::string drm = r_.sys + "/class/drm";
    // NVML calls are expensive, so cards are measured every 3 s (conky's own
    // rate) and the last reading is reused in between. Sleep state is fresh.
    if (now - full_t_ < 3 && !last_.empty()) {
        for (auto [pci, g] : last_) {
            const std::string dev = r_.sys + "/bus/pci/devices/" + pci;
            bool asleep = read_line(dev + "/power/runtime_status").value_or("") == "suspended";
            if (asleep != g.asleep) {
                full_t_ = -1e9;  // state changed: measure again right away
                break;
            }
            out.push_back(g);
        }
        if (full_t_ > 0) {
            for (const auto& [pid, bytes] : last_by_pid_) by_pid[pid] += bytes;
            return;
        }
        out.clear();
    }
    full_t_ = now;
    last_.clear();
    for (const auto& card : list_dir(drm)) {
        if (!starts_with(card, "card") || card.find('-') != std::string::npos) continue;
        const std::string dev = drm + "/" + card + "/device";
        auto vendor = read_line(dev + "/vendor");
        auto device = read_line(dev + "/device");
        if (!vendor || !device) continue;
        GpuSample g;
        std::string vhex = vendor->substr(2), dhex = device->substr(2);
        g.vendor = vhex == "10de" ? "NVIDIA" : vhex == "8086" ? "Intel" : vhex == "1002" ? "AMD" : "Other";
        g.name = pci_name(vhex, dhex);
        auto drv = read_link(dev + "/driver");
        g.driver = drv.substr(drv.rfind('/') + 1);
        auto real = read_link(drm + "/" + card + "/device");
        g.pci = real.substr(real.rfind('/') + 1);
        g.asleep = read_line(dev + "/power/runtime_status").value_or("") == "suspended";

        const std::string asleep = "The card is asleep to save power; reading it would wake it";
        auto none = [&](const std::string& why) {
            g.util = g.mem_used = g.mem_total = g.temp_c = g.power_w = g.clock_mhz = Reading<double>::none(why);
        };

        if (g.vendor == "NVIDIA") {
            if (g.asleep) busy_until_ = 0;
            if (!g.asleep && rest_until_ > 0 && now >= rest_until_) {
                // We left it alone for a minute and it stayed awake, so another
                // program is holding it: measure for 5 minutes, then let go
                // again so it can sleep once that program is done.
                busy_until_ = now + 300;
                rest_until_ = 0;
            }
            if (g.asleep && nvml_open_) {
                nvml_->shutdown();
                nvml_open_ = false;
            }
            if (g.asleep) none(asleep);
            else if (!nvml_) none("NVIDIA's monitoring library (libnvidia-ml) is not installed");
            else if (now < rest_until_) none("Idle; left alone for a minute so it can power down");
            else if (!nvml_open_ && nvml_->init() != 0) none("NVIDIA's monitoring library would not start");
            else {
                nvml_open_ = true;
                Nvml::Dev h = nullptr;
                if (nvml_->by_pci(g.pci.c_str(), &h) == 0) {
                    if (nvml_name_.empty()) {
                        char nm[96] = {};
                        if (nvml_->name(h, nm, sizeof nm) == 0) nvml_name_ = std::string("NVIDIA ") + (starts_with(nm, "NVIDIA ") ? nm + 7 : nm);
                    }
                    if (!nvml_name_.empty()) g.name = nvml_name_;
                    Nvml::Util u{};
                    g.util = nvml_->util(h, &u) == 0 ? Reading<double>::of(u.gpu) : Reading<double>::none("Not reported");
                    Nvml::Mem m{};
                    if (nvml_->mem(h, &m) == 0) {
                        g.mem_used = Reading<double>::of(double(m.used));
                        g.mem_total = Reading<double>::of(double(m.total));
                    } else {
                        g.mem_used = g.mem_total = Reading<double>::none("Not reported");
                    }
                    unsigned v = 0;
                    g.temp_c = nvml_->temp && nvml_->temp(h, 0, &v) == 0 ? Reading<double>::of(v) : Reading<double>::none("Not reported");
                    // Power and clock are the slow calls (several ms each); every 15 s is plenty.
                    if (now - slow_t_ >= 15) {
                        slow_power_ = nvml_->power && nvml_->power(h, &v) == 0 ? Reading<double>::of(v / 1000.0) : Reading<double>::none("Not reported");
                        slow_clock_ = nvml_->clock && nvml_->clock(h, 0, &v) == 0 ? Reading<double>::of(v) : Reading<double>::none("Not reported");
                        slow_t_ = now;
                    }
                    g.power_w = slow_power_;
                    g.clock_mhz = slow_clock_;

                    // The per-process list is the slow call; every 5 s is enough.
                    int users = last_users_;
                    if (now - users_t_ >= 5) {
                    users = 0;
                    users_t_ = now;
                    last_by_pid_.clear();
                    for (auto fn : {nvml_->compute, nvml_->graphics}) {
                        if (!fn) continue;
                        Nvml::Proc procs[64];
                        unsigned count = 64;
                        if (fn(h, &count, procs) != 0) continue;
                        for (unsigned i = 0; i < count; ++i) {
                            ++users;
                            // usedGpuMemory is ~0ULL when the driver cannot attribute it.
                            if (procs[i].used != ~0ULL) last_by_pid_[int(procs[i].pid)] += double(procs[i].used);
                        }
                    }
                    last_users_ = users;
                    }
                    for (const auto& [pid, bytes] : last_by_pid_) by_pid[pid] += bytes;
                    // Asking every second would keep an otherwise idle laptop GPU
                    // awake, so after ten idle samples with no users, back off.
                    idle_streak_ = (users == 0 && g.util.or_else(0) < 1) ? idle_streak_ + 1 : 0;
                    if (idle_streak_ >= 10 && now >= busy_until_) {
                        rest_until_ = now + 60;
                        idle_streak_ = 0;
                        nvml_->shutdown();  // an open handle would keep the card awake
                        nvml_open_ = false;
                    }
                } else {
                    none("NVIDIA's library does not know this card");
                }
            }
        } else if (g.vendor == "AMD") {
            auto pct = read_int(dev + "/gpu_busy_percent");
            g.util = pct ? Reading<double>::of(double(*pct)) : Reading<double>::none(g.asleep ? asleep : "Not reported");
            auto used = read_int(dev + "/mem_info_vram_used");
            auto total = read_int(dev + "/mem_info_vram_total");
            g.mem_used = used ? Reading<double>::of(double(*used)) : Reading<double>::none("Not reported");
            g.mem_total = total ? Reading<double>::of(double(*total)) : Reading<double>::none("Not reported");
            g.temp_c = g.power_w = Reading<double>::none("Not reported");
            for (const auto& h : list_dir(dev + "/hwmon")) {
                if (auto t = read_int(dev + "/hwmon/" + h + "/temp1_input")) g.temp_c = Reading<double>::of(*t / 1000.0);
                auto p = read_int(dev + "/hwmon/" + h + "/power1_average");
                if (!p) p = read_int(dev + "/hwmon/" + h + "/power1_input");
                if (p) g.power_w = Reading<double>::of(*p / 1e6);
            }
            g.clock_mhz = Reading<double>::none("Not reported");
        } else {
            // Intel's load counters need performance-monitoring access, which a
            // normal user does not have; the clock is public.
            g.util = Reading<double>::none("Intel GPU load needs performance-counter access");
            g.mem_used = g.mem_total = Reading<double>::none("Shares system memory");
            g.temp_c = g.power_w = Reading<double>::none("Not reported");
            auto f = read_int(drm + "/" + card + "/gt_act_freq_mhz");
            if (!f) f = read_int(drm + "/" + card + "/gt_cur_freq_mhz");
            g.clock_mhz = f ? Reading<double>::of(double(*f)) : Reading<double>::none("Not reported");
        }
        last_[g.pci] = g;
        out.push_back(std::move(g));
    }
}

}  // namespace as
