#include "providers.h"

namespace as {

namespace {

bool is_physical(const std::string& n) {
    for (const char* skip : {"loop", "ram", "zram", "dm-", "sr", "fd", "nbd"})
        if (starts_with(n, skip)) return false;
    return true;
}

}  // namespace

void DiskProvider::sample(std::vector<DiskSample>& out, double now) {
    auto stats = read_file(r_.proc + "/diskstats");
    if (!stats) throw std::runtime_error("cannot read /proc/diskstats");
    double dt = prev_t_ > 0 ? now - prev_t_ : 0;
    std::map<std::string, Prev> next;

    for (const auto& line : split(*stats, '\n')) {
        auto f = split_ws(line);
        if (f.size() < 14) continue;
        std::string name(f[2]);
        const std::string blk = r_.sys + "/block/" + name;
        if (!is_physical(name) || !exists(blk)) continue;  // partitions are not in /sys/block

        Prev cur{to_uint(f[5]).value_or(0) * 512, to_uint(f[9]).value_or(0) * 512, to_uint(f[12]).value_or(0)};
        DiskSample d;
        d.name = name;
        d.model = read_line(blk + "/device/model").value_or("");
        d.size = static_cast<uint64_t>(read_int(blk + "/size").value_or(0)) * 512;
        if (starts_with(name, "nvme")) d.kind = "NVMe SSD";
        else if (read_int(blk + "/removable").value_or(0)) d.kind = "Removable";
        else if (read_int(blk + "/queue/rotational").value_or(0)) d.kind = "Hard drive";
        else d.kind = "SSD";

        auto it = prev_.find(name);
        if (it != prev_.end() && dt > 0) {
            d.read_bps = double(cur.rd - it->second.rd) / dt;
            d.write_bps = double(cur.wr - it->second.wr) / dt;
            d.busy = std::min(100.0, double(cur.ticks - it->second.ticks) / (dt * 10.0));
        }
        // Asking a drive for its temperature is slow, so every 10 s is enough.
        if (now - temps_t_ >= 10 || !temps_.count(name)) {
            Reading<double> t = Reading<double>::none("No drive temperature sensor");
            for (const auto& h : list_dir(blk + "/device")) {
                if (!starts_with(h, "hwmon")) continue;
                if (auto v = read_int(blk + "/device/" + h + "/temp1_input")) t = Reading<double>::of(*v / 1000.0);
            }
            temps_[name] = t;
        }
        d.temp_c = temps_[name];
        next[name] = cur;
        if (d.size > 0) out.push_back(std::move(d));
    }
    if (now - temps_t_ >= 10) temps_t_ = now;
    prev_ = std::move(next);
    prev_t_ = now;
}

}  // namespace as
