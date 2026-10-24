#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_set>

#include "providers.h"

namespace as {

namespace {

std::string base_name(const std::string& path) {
    auto slash = path.rfind('/');
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    // The kernel appends " (deleted)" to the link of a replaced executable.
    if (auto d = b.find(" (deleted)"); d != std::string::npos) b.resize(d);
    return b;
}

std::string unit_of(const std::string& cgroup) {
    auto slash = cgroup.rfind('/');
    std::string last = slash == std::string::npos ? cgroup : cgroup.substr(slash + 1);
    for (const char* sfx : {".service", ".scope"})
        if (last.size() > std::string(sfx).size() && last.ends_with(sfx)) return last;
    return {};
}

}  // namespace

ProcessProvider::ProcessProvider(Roots r) : r_(std::move(r)) {
    hz_ = sysconf(_SC_CLK_TCK);
    page_ = sysconf(_SC_PAGESIZE);
    if (r_.proc == "/proc") self_ = getpid();
    if (auto st = read_file(r_.proc + "/stat")) {
        for (const auto& line : split(*st, '\n'))
            if (starts_with(line, "btime ")) boot_epoch_ = double(to_int(std::string_view(line).substr(6)).value_or(0));
    }
}

const std::string& ProcessProvider::user_name(int uid) {
    auto it = users_.find(uid);
    if (it != users_.end()) return it->second;
    std::string name = std::to_string(uid);
    passwd pw{};
    passwd* res = nullptr;
    char buf[4096];
    if (getpwuid_r(static_cast<uid_t>(uid), &pw, buf, sizeof buf, &res) == 0 && res) name = res->pw_name;
    return users_.emplace(uid, name).first->second;
}

void ProcessProvider::sample(Snapshot& snap, int cpus, const std::unordered_map<int, double>& gpu_mem, bool read_io) {
    const double now = snap.mono;
    const double dt = prev_t_ > 0 ? now - prev_t_ : 0;
    const double machine_ticks = dt * double(hz_) * double(std::max(1, cpus));
    const int my_uid = static_cast<int>(getuid());
    std::unordered_set<int> seen;
    // Reaped-children ticks gained since the last sample, and the processes
    // that ended since then with their last seen totals.
    std::unordered_map<int, uint64_t> reaped;
    struct Ended { int pid, ppid; uint64_t total; };
    std::vector<Ended> ended;
    snap.threads_total = 0;

    for (const auto& entry : list_dir(r_.proc)) {
        auto pid = to_int(entry);
        if (!pid) continue;
        const std::string dir = r_.proc + "/" + entry;
        auto stat = read_file(dir + "/stat", 4096);
        if (!stat) continue;  // exited while we were looking
        auto open = stat->find('(');
        auto close = stat->rfind(')');
        if (open == std::string::npos || close == std::string::npos || close + 2 >= stat->size()) continue;
        auto f = split_ws(std::string_view(*stat).substr(close + 2));
        if (f.size() < 22) continue;
        // Field n of /proc/pid/stat is f[n - 3] here.
        ProcessInfo p;
        p.key.pid = int(*pid);
        p.key.start_ticks = to_uint(f[19]).value_or(0);
        p.comm = stat->substr(open + 1, close - open - 1);
        p.state = std::string(f[0]);
        p.ppid = int(to_int(f[1]).value_or(0));
        p.major_faults = to_uint(f[9]).value_or(0);
        uint64_t ticks = to_uint(f[11]).value_or(0) + to_uint(f[12]).value_or(0);
        uint64_t child_ticks = to_uint(f[13]).value_or(0) + to_uint(f[14]).value_or(0);
        p.nice = int(to_int(f[16]).value_or(0));
        p.threads = int(to_int(f[17]).value_or(1));
        p.rss = to_uint(f[21]).value_or(0) * uint64_t(page_);
        p.cpu_seconds = double(ticks) / double(hz_);
        p.started_epoch = boot_epoch_ + double(p.key.start_ticks) / double(hz_);
        p.kernel_thread = p.ppid == 2 || p.key.pid == 2;
        p.self = p.key.pid == self_;
        snap.threads_total += p.threads;

        auto it = known_.find(p.key.pid);
        if (it != known_.end() && it->second.last.key.start_ticks != p.key.start_ticks) {
            if (!it->second.last.kernel_thread)
                ended.push_back({it->first, it->second.last.ppid, it->second.ticks + it->second.child_ticks});
            snap.exited.push_back(it->second.last);  // the pid was recycled
            known_.erase(it);
            it = known_.end();
        }
        if (it == known_.end()) {
            Known k;
            if (!p.kernel_thread) {
                k.last.cmdline = read_file(dir + "/cmdline", 8192).value_or("");
                for (auto& c : k.last.cmdline)
                    if (c == '\0') c = ' ';
                k.last.cmdline = trim(k.last.cmdline);
                k.last.exe = read_link(dir + "/exe");
                if (auto cg = read_line(dir + "/cgroup"); cg && starts_with(*cg, "0::")) k.last.cgroup = cg->substr(3);
            }
            struct stat st {};
            if (::stat(dir.c_str(), &st) == 0) k.last.uid = int(st.st_uid);  // an owner never changes
            p.is_new = true;
            it = known_.emplace(p.key.pid, std::move(k)).first;
            if (prev_t_ > 0 && p.started_epoch >= snap.wall - dt - 1) ++snap.new_procs;
        }
        Known& k = it->second;
        p.uid = k.last.uid;
        p.user = user_name(p.uid);
        p.cmdline = k.last.cmdline;
        p.exe = k.last.exe;
        p.cgroup = k.last.cgroup;
        p.unit = unit_of(p.cgroup);

        // Prefer the executable's name when the 15-character comm is a cut-off prefix of it.
        std::string exe_base = base_name(p.exe);
        p.name = (p.comm.size() >= 15 && starts_with(exe_base, p.comm)) ? exe_base : p.comm;

        if (machine_ticks > 0) {
            if (p.is_new) {
                // Started since the last sample: all of its CPU time is recent.
                if (p.started_epoch >= snap.wall - dt - 1) p.cpu = 100.0 * double(ticks) / machine_ticks;
            } else {
                p.cpu = 100.0 * double(ticks - std::min(ticks, k.ticks)) / machine_ticks;
                reaped[p.key.pid] = child_ticks - std::min(child_ticks, k.child_ticks);
            }
        }

        if (!read_io) {
            k.io_ok = false;  // the next reading starts a fresh measurement
            p.read_bps = p.write_bps = Reading<double>::of(0);
        } else if (p.uid == my_uid && !p.kernel_thread) {
            if (auto io = read_file(dir + "/io", 1024)) {
                uint64_t rd = 0, wr = 0;
                for (const auto& line : split(*io, '\n')) {
                    if (starts_with(line, "read_bytes: ")) rd = to_uint(std::string_view(line).substr(12)).value_or(0);
                    if (starts_with(line, "write_bytes: ")) wr = to_uint(std::string_view(line).substr(13)).value_or(0);
                }
                if (k.io_ok && dt > 0) {
                    p.read_bps = Reading<double>::of(double(rd - std::min(rd, k.rd)) / dt);
                    p.write_bps = Reading<double>::of(double(wr - std::min(wr, k.wr)) / dt);
                } else {
                    p.read_bps = p.write_bps = Reading<double>::none("Measuring");
                }
                k.rd = rd;
                k.wr = wr;
                k.io_ok = true;
            }
        }
        if (!p.read_bps.ok() && p.read_bps.why.empty())
            p.read_bps = p.write_bps = Reading<double>::none("Only readable for your own processes");

        if (auto g = gpu_mem.find(p.key.pid); g != gpu_mem.end()) p.gpu_mem = Reading<double>::of(g->second);
        else p.gpu_mem = Reading<double>::none("No video memory in use");

        k.ticks = ticks;
        k.child_ticks = child_ticks;
        k.last = p;
        seen.insert(p.key.pid);
        snap.procs.push_back(std::move(p));
    }

    for (auto it = known_.begin(); it != known_.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        ProcessInfo gone = it->second.last;
        if (!gone.kernel_thread) ended.push_back({it->first, gone.ppid, it->second.ticks + it->second.child_ticks});
        gone.cpu = 0;
        gone.state = "exited";
        snap.exited.push_back(std::move(gone));
        it = known_.erase(it);
    }

    // When a child ends, its whole lifetime lands at once in the reaped-
    // children time of the process that reaps it. What was seen while it ran
    // was counted then, so its last seen total is taken off again; what is
    // left is children that started and ended between two samples, and the
    // last moments of those that ended. The reaper is the last seen parent,
    // or, when that parent ended too, an ancestor further up the same line:
    // the parent reaped it before ending, or the orphan was adopted by an
    // older ancestor (systemd --user takes in orphans).
    std::unordered_map<int, int> up;  // pid -> parent, built only when a parent ended too
    auto take = [&](int pid, uint64_t& credit) {
        auto r = reaped.find(pid);
        if (r == reaped.end()) return;
        uint64_t n = std::min(credit, r->second);
        r->second -= n;
        credit -= n;
    };
    // A parent read just before it reaped a child shows the jump one sample late.
    for (auto [pid, credit] : carry_) take(pid, credit);
    carry_.clear();
    std::sort(ended.begin(), ended.end(), [](const Ended& a, const Ended& b) { return a.pid < b.pid; });
    for (auto e : ended) {
        if (seen.count(e.ppid)) {
            take(e.ppid, e.total);
            if (e.total) carry_[e.ppid] += e.total;
            continue;
        }
        if (up.empty()) {
            for (const auto& x : ended) up[x.pid] = x.ppid;
            for (const auto& p : snap.procs) up[p.key.pid] = p.ppid;
        }
        for (int at = e.ppid, depth = 0; at > 0 && e.total && depth < 64; ++depth) {
            if (seen.count(at)) take(at, e.total);
            auto u = up.find(at);
            if (u == up.end()) break;
            at = u->second;
        }
    }
    for (auto& p : snap.procs)
        if (auto r = reaped.find(p.key.pid); r != reaped.end()) p.child_cpu = 100.0 * double(r->second) / machine_ticks;
    prev_t_ = now;
}

void CgroupProvider::sample(std::vector<UnitSample>& out, double now, int cpus) {
    const std::string root = r_.sys + "/fs/cgroup";
    // Units come and go rarely; the tree is re-listed every 10 s.
    if (now - leaves_t_ >= 10) {
        leaves_.clear();
        walk(root, 0, leaves_);
        leaves_t_ = now;
    }
    std::map<std::string, Prev> next;
    double dt = prev_t_ > 0 ? now - prev_t_ : 0;
    for (const auto& path : leaves_) {
        Prev cur;
        auto cs = read_file(path + "/cpu.stat", 2048);
        if (!cs) continue;  // the unit stopped
        for (const auto& line : split(*cs, '\n'))
            if (starts_with(line, "usage_usec ")) cur.usage = to_uint(std::string_view(line).substr(11)).value_or(0);
        // Only CPU is read every tick: it is what the culprit math needs, and
        // each extra file per unit costs about as much again.
        UnitSample u;
        u.path = path.substr(root.size());
        u.name = path.substr(path.rfind('/') + 1);
        if (auto it = prev_.find(path); it != prev_.end() && dt > 0) {
            u.cpu = 100.0 * double(cur.usage - std::min(cur.usage, it->second.usage)) / (dt * 1e6 * std::max(1, cpus));
        }
        next[path] = cur;
        out.push_back(std::move(u));
    }
    prev_ = std::move(next);
    prev_t_ = now;
}

void CgroupProvider::walk(const std::string& path, int depth, std::vector<std::string>& leaves) {
    if (depth > 8) return;
    bool has_child_unit = false;
    for (const auto& name : list_dir(path)) {
        bool slice = name.ends_with(".slice");
        bool unit = name.ends_with(".service") || name.ends_with(".scope");
        if (!slice && !unit) continue;
        std::size_t before = leaves.size();
        walk(path + "/" + name, depth + 1, leaves);
        if (unit || leaves.size() > before) has_child_unit = true;
    }
    std::string base = path.substr(path.rfind('/') + 1);
    // Only leaf units are kept, so nothing is counted twice.
    if ((base.ends_with(".service") || base.ends_with(".scope")) && !has_child_unit) leaves.push_back(path);
}

}  // namespace as
