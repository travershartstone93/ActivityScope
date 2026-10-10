#include "events.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>
#include <unordered_map>

namespace as {

namespace {

template <class... A>
std::string fmt(const char* f, A... a) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a...);
    return buf;
}

double fan_max(const Snapshot& s) {
    double m = -1;
    for (const auto& f : s.power.fans)
        if (f.percent.ok()) m = std::max(m, double(f.percent.get()));
    return m;
}

double gpu_max(const Snapshot& s) {
    double m = -1;
    for (const auto& g : s.gpus)
        if (g.util.ok()) m = std::max(m, g.util.get());
    return m;
}

double disk_total(const Snapshot& s) {
    double t = 0;
    for (const auto& d : s.disks) t += d.read_bps + d.write_bps;
    return t;
}

}  // namespace

std::string clock_time(double epoch) {
    time_t t = static_cast<time_t>(epoch);
    tm lt{};
    localtime_r(&t, &lt);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &lt);
    return buf;
}

std::string human_bytes(double b) {
    if (b < 0) b = 0;
    if (b >= 1e12) return fmt("%.1f TB", b / 1e12);
    if (b >= 1e9) return fmt("%.1f GB", b / 1e9);
    if (b >= 1e6) return fmt("%.0f MB", b / 1e6);
    if (b >= 1e3) return fmt("%.0f KB", b / 1e3);
    return fmt("%.0f B", b);
}

std::string human_duration(double s) {
    if (s < 60) return fmt("%.0f s", s);
    if (s < 3600) return fmt("%d min %d s", int(s) / 60, int(s) % 60);
    return fmt("%d h %d min", int(s) / 3600, (int(s) % 3600) / 60);
}

std::string kind_names(unsigned k) {
    std::vector<std::string> parts;
    if (k & KindCpu) parts.push_back("CPU");
    if (k & KindMemory) parts.push_back("Memory");
    if (k & KindDisk) parts.push_back("Disk");
    if (k & KindGpu) parts.push_back("GPU");
    if (k & KindHeat) parts.push_back("Heat");
    if (k & KindShutdown) parts.push_back("Shutdown");
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    return out;
}

const Snapshot* EventEngine::ago(double seconds) const {
    if (ring_.empty()) return nullptr;
    double target = ring_.back()->wall - seconds;
    const Snapshot* best = nullptr;
    for (const auto& s : ring_) {
        if (s->wall <= target + 0.5) best = s.get();
        else break;
    }
    return best;
}

EventSample EventEngine::point(const Snapshot& s) const {
    EventSample p;
    p.t = s.wall;
    p.cpu = s.cpu.total;
    p.mem_pct = s.mem.percent();
    p.pressure = s.mem.pressure_some10.or_else(0);
    p.disk_bps = disk_total(s);
    p.gpu = gpu_max(s);
    p.temp = s.cpu.temp_c.or_else(-1);
    p.fan = fan_max(s);
    p.guard = s.guard.temp.or_else(-1);
    return p;
}

std::vector<EventEngine::Active> EventEngine::evaluate(const Snapshot& s) {
    std::vector<Active> out;
    std::set<std::string> touched;
    auto streak = [&](const std::string& id, bool on) {
        touched.insert(id);
        if (!on) {
            streaks_[id] = 0;
            return 0;
        }
        return ++streaks_[id];
    };

    if (streak("cpu", s.cpu.total > t_.cpu_total) >= t_.cpu_total_secs)
        out.push_back({KindCpu, "cpu", fmt("CPU above %.0f%% (reached %.0f%%)", t_.cpu_total, s.cpu.total)});
    for (std::size_t i = 0; i < s.cpu.per_core.size(); ++i) {
        std::string id = "core" + std::to_string(i);
        if (streak(id, s.cpu.per_core[i] >= t_.core) >= t_.core_secs)
            out.push_back({KindCpu, id, fmt("Core %zu pinned at %.0f%%", i, s.cpu.per_core[i])});
    }
    for (const auto& p : s.procs) {
        std::string id = fmt("p%d:%llu", p.key.pid, (unsigned long long)p.key.start_ticks);
        double total = p.cpu + p.child_cpu;
        if (streak(id, total > t_.proc_cpu) >= t_.proc_secs)
            out.push_back({KindCpu, id, fmt("%s used %.0f%% of the whole machine", p.name.c_str(), total)});
    }
    int born = 0;
    for (const auto& r : ring_)
        if (r->wall > s.wall - t_.new_procs_window) born += r->new_procs;
    if (born >= t_.new_procs) out.push_back({KindCpu, "births", fmt("%d new processes in %d s", born, t_.new_procs_window)});

    if (s.mem.pressure_some10.ok() && s.mem.pressure_some10.get() > t_.pressure)
        out.push_back({KindMemory, "pressure", fmt("Programs waited on memory %.0f%% of the time", s.mem.pressure_some10.get())});
    if (ring_.size() >= 2) {
        const Snapshot& prev = *ring_[ring_.size() - 2];
        double dt = s.mono - prev.mono;
        if (dt > 0 && s.mem.page_ins >= prev.mem.page_ins) {
            double kbps = double(s.mem.page_ins - prev.mem.page_ins) * 4.0 / dt;
            if (streak("swapin", kbps > t_.swapin_kbps) >= t_.swapin_secs) out.push_back({KindMemory, "swapin", fmt("Swapping back in at %s/s", human_bytes(kbps * 1024).c_str())});
        }
    }
    if (const Snapshot* a = ago(t_.growth_window); a && a != &s) {
        if (a->mem.available > s.mem.available && double(a->mem.available - s.mem.available) >= t_.avail_drop_mb * 1e6)
            out.push_back({KindMemory, "availdrop", fmt("Free memory fell by %s in %d s", human_bytes(double(a->mem.available - s.mem.available)).c_str(), t_.growth_window)});
        std::unordered_map<int, uint64_t> then;
        for (const auto& p : a->procs) then[p.key.pid] = p.rss;
        for (const auto& p : s.procs) {
            auto it = then.find(p.key.pid);
            if (it != then.end() && p.rss > it->second && double(p.rss - it->second) >= t_.proc_growth_mb * 1e6)
                out.push_back({KindMemory, fmt("grow%d", p.key.pid), fmt("%s grew by %s in %d s", p.name.c_str(), human_bytes(double(p.rss - it->second)).c_str(), t_.growth_window)});
        }
    }

    for (const auto& d : s.disks) {
        if (streak("busy" + d.name, d.busy > t_.disk_busy) >= t_.disk_secs)
            out.push_back({KindDisk, "busy" + d.name, fmt("Disk %s busy %.0f%% of the time", d.name.c_str(), d.busy)});
        double bps = d.read_bps + d.write_bps;
        if (streak("mbps" + d.name, bps > t_.disk_mbps * 1e6) >= t_.disk_secs)
            out.push_back({KindDisk, "mbps" + d.name, fmt("Disk %s moving %s/s", d.name.c_str(), human_bytes(bps).c_str())});
    }

    const Snapshot* ten = ago(10);
    for (std::size_t i = 0; i < s.gpus.size(); ++i) {
        const auto& g = s.gpus[i];
        std::string id = "gpu" + std::to_string(i);
        if (streak(id, g.util.ok() && g.util.get() > t_.gpu) >= t_.gpu_secs)
            out.push_back({KindGpu, id, fmt("%s at %.0f%%", g.name.c_str(), g.util.get())});
        if (ten && i < ten->gpus.size() && g.mem_used.ok() && ten->gpus[i].mem_used.ok() &&
            g.mem_used.get() - ten->gpus[i].mem_used.get() >= t_.vram_jump_mb * 1e6)
            out.push_back({KindGpu, id + "mem", fmt("Video memory jumped by %s", human_bytes(g.mem_used.get() - ten->gpus[i].mem_used.get()).c_str())});
    }

    if (s.cpu.temp_c.ok()) {
        double c = s.cpu.temp_c.get();
        if (c >= t_.temp) out.push_back({KindHeat, "temp", fmt("CPU at %.0f °C", c)});
        if (const Snapshot* a = ago(t_.temp_window); a && a->cpu.temp_c.ok() && c - a->cpu.temp_c.get() >= t_.temp_rise)
            out.push_back({KindHeat, "rise", fmt("CPU temperature rose %.0f °C in %d s", c - a->cpu.temp_c.get(), t_.temp_window)});
    }
    if (s.guard.margin.ok() && s.guard.margin.get() <= t_.shutdown_margin)
        out.push_back({KindHeat, "shutdown", fmt("%.0f °C from thermal shutdown (%s at %.0f of %.0f °C)", s.guard.margin.get(),
                                                  s.guard.zone.c_str(), s.guard.temp.get(), s.guard.limit.get())});
    double fan = fan_max(s);
    if (fan >= 0) {
        if (fan > t_.fan) out.push_back({KindHeat, "fan", fmt("Fan at %.0f%%", fan)});
        if (const Snapshot* a = ago(t_.temp_window); a && fan_max(*a) >= 0 && fan - fan_max(*a) >= t_.fan_step)
            out.push_back({KindHeat, "fanstep", fmt("Fan stepped up from %.0f%% to %.0f%%", fan_max(*a), fan)});
    }

    for (auto it = streaks_.begin(); it != streaks_.end();)
        it = touched.count(it->first) ? std::next(it) : streaks_.erase(it);
    return out;
}

void EventEngine::accumulate(const Snapshot& s, double dt, double proc_dt) {
    if (dt <= 0 || !ev_) return;
    const double cpus = std::max(1, s.cpu.logical);
    ev_->total_cpu_seconds += s.cpu.total * cpus * dt / 100.0;
    // A carried-over process list was already counted; real scans cover the
    // whole gap since the previous real scan.
    if (s.procs_reused || proc_dt <= 0) return;
    dt = proc_dt;
    for (const auto& p : s.procs) {
        auto [it, fresh] = acc_.try_emplace({p.key.pid, p.key.start_ticks});
        Culprit& c = it->second.c;
        if (fresh) {
            c.name = p.name;
            c.cmdline = p.cmdline;
            c.unit = p.unit;
            c.pid = p.key.pid;
            c.ppid = p.ppid;
            c.start_ticks = p.key.start_ticks;
            c.first_rss = p.rss;
            c.first_faults = p.major_faults;
            c.first_seen = s.wall;
            c.started = p.started_epoch;
        }
        c.faults = p.major_faults - std::min(p.major_faults, c.first_faults);
        c.cpu_seconds += p.cpu * cpus * dt / 100.0;
        c.child_cpu_seconds += p.child_cpu * cpus * dt / 100.0;
        c.peak_rss = std::max(c.peak_rss, p.rss);
        c.rss_growth = int64_t(p.rss) - int64_t(c.first_rss);
        if (p.read_bps.ok()) c.io_bytes += (p.read_bps.get() + p.write_bps.or_else(0)) * dt;
        if (p.gpu_mem.ok()) c.gpu_mem = std::max(c.gpu_mem, p.gpu_mem.get());
        c.last_seen = s.wall;
    }
    for (const auto& p : s.exited)
        if (auto it = acc_.find({p.key.pid, p.key.start_ticks}); it != acc_.end()) it->second.c.exited = true;
    for (const auto& u : s.units) unit_cpu_[u.name] += u.cpu * cpus * dt / 100.0;
}

void EventEngine::begin(const SnapshotPtr& s, const std::vector<Active>& active) {
    ev_.emplace();
    ev_->start = s->wall - std::max(t_.cpu_total_secs, t_.disk_secs);
    ev_->last_active = s->wall;
    ev_->primary = active.front().kind;
    for (const auto& a : active)
        if (a.kind == KindCpu) ev_->primary = KindCpu;
    // Graph the lead-in; count culprits from shortly before the trigger.
    double prev = 0, prev_scan = 0;
    for (const auto& r : ring_) {
        if (r->wall >= s->wall - t_.preroll) ev_->samples.push_back(point(*r));
        if (r->wall >= ev_->start - 10 && prev > 0) accumulate(*r, r->mono - prev, prev_scan > 0 ? r->mono - prev_scan : 0);
        prev = r->mono;
        if (!r->procs_reused) prev_scan = r->mono;
    }
    for (const auto& a : active) {
        ev_->kinds |= a.kind;
        if (trigger_text_.emplace(a.id, a.text).second && ev_->triggers.size() < 24) ev_->triggers.push_back(a.text);
    }
    for (const auto& r : ring_)
        if (r->wall >= ev_->start - 10) note_guard(*r);
}

void EventEngine::note_guard(const Snapshot& s) {
    const auto& g = s.guard;
    if (g.margin.ok()) {
        ev_->guard_zone = g.zone;
        ev_->guard_limit = g.limit.get();
        if (ev_->min_margin < 0 || g.margin.get() < ev_->min_margin) ev_->min_margin = std::max(0.0, g.margin.get());
    }
    if (g.seconds_left.ok() && (ev_->min_seconds < 0 || g.seconds_left.get() < ev_->min_seconds)) ev_->min_seconds = g.seconds_left.get();
    for (const auto& f : g.dead_fans)
        if (std::find(ev_->dead_fans.begin(), ev_->dead_fans.end(), f) == ev_->dead_fans.end()) ev_->dead_fans.push_back(f);
}

std::vector<Event> EventEngine::feed(const SnapshotPtr& s) {
    ring_.push_back(s);
    const double keep = std::max({t_.preroll, t_.temp_window, t_.growth_window}) + 5;
    while (!ring_.empty() && ring_.front()->wall < s->wall - keep) ring_.pop_front();
    const double dt = prev_mono_ > 0 ? s->mono - prev_mono_ : 0;
    prev_mono_ = s->mono;
    double proc_dt = 0;
    if (!s->procs_reused) {
        proc_dt = prev_scan_mono_ > 0 ? s->mono - prev_scan_mono_ : 0;
        prev_scan_mono_ = s->mono;
    }
    const double cpus = std::max(1, s->cpu.logical);

    int64_t minute = int64_t(s->wall) / 60 * 60;
    if (minute != minute_) {
        if (minute_ != 0 && !minute_acc_.empty()) {
            std::vector<Culprit> all;
            for (auto& [k, c] : minute_acc_) all.push_back(c);
            std::set<std::pair<int, uint64_t>> pick;
            auto take = [&](auto better, std::size_t n) {
                std::sort(all.begin(), all.end(), better);
                for (std::size_t i = 0; i < std::min(n, all.size()); ++i) pick.insert({all[i].pid, all[i].start_ticks});
            };
            take([](const Culprit& a, const Culprit& b) { return a.cpu_seconds > b.cpu_seconds; }, 15);
            take([](const Culprit& a, const Culprit& b) { return a.peak_rss > b.peak_rss; }, 5);
            take([](const Culprit& a, const Culprit& b) { return a.io_bytes > b.io_bytes; }, 5);
            MinuteTop mt;
            mt.minute = minute_;
            for (auto& c : all)
                if (pick.count({c.pid, c.start_ticks})) mt.top.push_back(c);
            std::sort(mt.top.begin(), mt.top.end(), [](const Culprit& a, const Culprit& b) { return a.cpu_seconds > b.cpu_seconds; });
            minute_ready_ = std::move(mt);
        }
        minute_acc_.clear();
        minute_ = minute;
    }
    if (proc_dt > 0) {
        for (const auto& p : s->procs) {
            Culprit& c = minute_acc_[{p.key.pid, p.key.start_ticks}];
            if (c.pid == 0) {
                c.name = p.name;
                c.cmdline = p.cmdline;
                c.unit = p.unit;
                c.pid = p.key.pid;
                c.ppid = p.ppid;
                c.start_ticks = p.key.start_ticks;
            }
            c.cpu_seconds += (p.cpu + p.child_cpu) * cpus * proc_dt / 100.0;
            c.peak_rss = std::max(c.peak_rss, p.rss);
            if (p.read_bps.ok()) c.io_bytes += (p.read_bps.get() + p.write_bps.or_else(0)) * proc_dt;
        }
        for (const auto& p : s->exited)
            if (auto it = minute_acc_.find({p.key.pid, p.key.start_ticks}); it != minute_acc_.end()) it->second.exited = true;
    }

    auto active = evaluate(*s);
    active_now_.clear();
    for (const auto& a : active) active_now_.push_back(a.text);
    std::vector<Event> closed;
    if (ev_) {
        ev_->samples.push_back(point(*s));
        accumulate(*s, dt, proc_dt);
        note_guard(*s);
        if (!active.empty()) {
            ev_->last_active = s->wall;
            for (const auto& a : active) {
                if (a.kind == KindCpu) ev_->primary = KindCpu;
                ev_->kinds |= a.kind;
                if (trigger_text_.emplace(a.id, a.text).second && ev_->triggers.size() < 24) ev_->triggers.push_back(a.text);
            }
        }
        bool calm_long_enough = s->wall - ev_->last_active >= t_.merge;
        bool too_long = s->wall - ev_->start > 1800;
        if (calm_long_enough || too_long) {
            closed.push_back(finish(true));
            ev_.reset();
            acc_.clear();
            unit_cpu_.clear();
            trigger_text_.clear();
            if (too_long && !active.empty()) begin(s, active);
        }
    } else if (!active.empty()) {
        begin(s, active);
    }
    return closed;
}

std::optional<Event> EventEngine::open_event() {
    if (!ev_) return std::nullopt;
    return finish(false);
}

std::optional<MinuteTop> EventEngine::take_minute() {
    auto out = std::move(minute_ready_);
    minute_ready_.reset();
    return out;
}

Event EventEngine::finish(bool final_close) {
    Event e = *ev_;
    e.open = !final_close;
    e.end = final_close ? e.last_active : (ring_.empty() ? e.last_active : ring_.back()->wall);
    if (final_close) {
        double cut = e.last_active + t_.calm;
        e.samples.erase(std::remove_if(e.samples.begin(), e.samples.end(), [&](const EventSample& p) { return p.t > cut; }),
                        e.samples.end());
    }
    for (const auto& p : e.samples) {
        if (p.t < e.start) continue;
        e.peak_cpu = std::max(e.peak_cpu, p.cpu);
        e.peak_mem_pct = std::max(e.peak_mem_pct, p.mem_pct);
        e.peak_pressure = std::max(e.peak_pressure, p.pressure);
        e.peak_disk_bps = std::max(e.peak_disk_bps, p.disk_bps);
        e.peak_gpu = std::max(e.peak_gpu, p.gpu);
        e.peak_temp = std::max(e.peak_temp, p.temp);
        e.peak_fan = std::max(e.peak_fan, p.fan);
    }

    std::unordered_map<int, std::string> names;
    if (!ring_.empty())
        for (const auto& p : ring_.back()->procs) names[p.key.pid] = p.name;
    for (const auto& [k, a] : acc_) names.try_emplace(a.c.pid, a.c.name);
    auto parent_of = [&](int ppid) {
        auto it = names.find(ppid);
        return it == names.end() ? std::string("PID ") + std::to_string(ppid) : it->second;
    };

    std::vector<Culprit> list;
    std::map<std::pair<std::string, std::string>, std::vector<Culprit>> groups;
    std::unordered_map<int, double> seen_child_cpu;
    std::vector<Culprit> unseen_list;
    std::map<std::pair<int, uint64_t>, double> extra;
    {
        // A process's reaped-children time includes everything its exited
        // descendants used. Walking deepest-first, each process reports the
        // part of its children's time we never saw by name, then hands its
        // own total up to its parent, so nothing is counted twice.
        std::unordered_map<int, const Culprit*> latest;
        for (const auto& [k, a] : acc_) {
            auto it = latest.find(a.c.pid);
            if (it == latest.end() || a.c.started > it->second->started) latest[a.c.pid] = &a.c;
        }
        auto depth = [&](const Culprit& c) {
            int d = 0;
            for (int pp = c.ppid; d < 64; ++d) {
                auto it = latest.find(pp);
                if (it == latest.end()) break;
                pp = it->second->ppid;
            }
            return d;
        };
        std::vector<const Culprit*> order;
        for (const auto& [k, a] : acc_) order.push_back(&a.c);
        std::stable_sort(order.begin(), order.end(), [&](auto x, auto y) { return depth(*x) > depth(*y); });
        using Key = std::pair<int, uint64_t>;
        struct Contribution { double amount; Key owner; double owner_cpu; };
        std::unordered_map<int, std::vector<Contribution>> contrib;
        for (const Culprit* c : order) {
            double seen = 0;
            const Contribution* best = nullptr;
            for (const auto& x : contrib[c->pid]) {
                seen += x.amount;
                if (!best || x.owner_cpu > best->owner_cpu) best = &x;
            }
            seen_child_cpu[c->pid] = seen;
            double unseen = c->child_cpu_seconds - seen;
            if (unseen >= 0.5 && best && seen >= 1 && unseen <= 0.35 * seen) {
                // The last moments of busy children that exited between samples.
                extra[best->owner] += unseen;
            } else if (unseen >= 0.5) {
                Culprit u;
                u.kind = "unseen";
                u.name = c->name;
                u.parent = c->name;
                u.pid = c->pid;
                u.ppid = c->ppid;
                u.start_ticks = c->start_ticks;
                u.unit = c->unit;
                u.exited = c->exited;
                u.cpu_seconds = unseen;
                u.count = 0;
                unseen_list.push_back(std::move(u));
            }
            if (c->exited) {
                Contribution mine{c->cpu_seconds + std::max(c->child_cpu_seconds, seen), {c->pid, c->start_ticks}, c->cpu_seconds};
                if (best && best->owner_cpu > c->cpu_seconds) {
                    mine.owner = best->owner;
                    mine.owner_cpu = best->owner_cpu;
                }
                contrib[c->ppid].push_back(mine);
            }
        }
    }
    for (const auto& [k, a] : acc_) {
        Culprit c = a.c;
        if (auto x = extra.find(k); x != extra.end()) c.cpu_seconds += x->second;
        c.parent = parent_of(c.ppid);
        bool short_lived = c.exited && c.started >= e.start - 11 && c.last_seen - c.started < 10;
        if (short_lived) groups[{c.name, c.parent}].push_back(std::move(c));
        else list.push_back(std::move(c));
    }
    for (auto& [key, members] : groups) {
        if (members.size() < 3) {
            for (auto& m : members) list.push_back(std::move(m));
            continue;
        }
        Culprit g = members.front();
        g.kind = "group";
        g.count = int(members.size());
        g.cpu_seconds = g.io_bytes = 0;
        g.peak_rss = 0;
        g.rss_growth = 0;
        g.faults = 0;
        for (const auto& m : members) {
            g.cpu_seconds += m.cpu_seconds;
            g.io_bytes += m.io_bytes;
            g.faults += m.faults;
            g.peak_rss = std::max(g.peak_rss, m.peak_rss);
        }
        list.push_back(std::move(g));
    }
    for (auto& u : unseen_list) list.push_back(std::move(u));
    std::map<std::string, double> by_unit;
    for (const auto& c : list)
        if (!c.unit.empty()) by_unit[c.unit] += c.cpu_seconds;
    for (const auto& [unit, used] : unit_cpu_) {
        double rest = used - by_unit[unit];
        if (rest >= std::max(1.0, 0.05 * e.total_cpu_seconds)) {
            Culprit u;
            u.kind = "unit";
            u.name = unit;
            u.unit = unit;
            u.cpu_seconds = rest;
            list.push_back(std::move(u));
        }
    }
    double total = std::max(e.total_cpu_seconds, 1e-9);
    for (auto& c : list) c.share = std::min(1.0, c.cpu_seconds / total);

    auto by_cpu = [](const Culprit& a, const Culprit& b) { return a.cpu_seconds > b.cpu_seconds; };
    switch (e.primary) {
    case KindMemory:
        std::sort(list.begin(), list.end(), [](const Culprit& a, const Culprit& b) {
            auto score = [](const Culprit& c) { return std::max(double(c.rss_growth), double(c.faults) * 4096 * 20) + double(c.peak_rss) * 0.01; };
            return score(a) > score(b);
        });
        break;
    case KindDisk:
        std::sort(list.begin(), list.end(), [](const Culprit& a, const Culprit& b) { return a.io_bytes > b.io_bytes; });
        break;
    case KindGpu:
        std::sort(list.begin(), list.end(), [](const Culprit& a, const Culprit& b) {
            return a.gpu_mem != b.gpu_mem ? a.gpu_mem > b.gpu_mem : a.cpu_seconds > b.cpu_seconds;
        });
        break;
    default:
        std::sort(list.begin(), list.end(), by_cpu);
    }
    // Keep the top 12 for the main resource, plus the top 5 for every other
    // resource involved, so each part of the summary has its evidence.
    std::vector<Culprit> kept(list.begin(), list.begin() + std::min<std::size_t>(12, list.size()));
    auto add_top = [&](auto key) {
        std::vector<const Culprit*> v;
        for (const auto& c : list) v.push_back(&c);
        std::sort(v.begin(), v.end(), [&](auto a, auto b) { return key(*a) > key(*b); });
        for (std::size_t i = 0; i < std::min<std::size_t>(5, v.size()); ++i) {
            bool have = std::any_of(kept.begin(), kept.end(), [&](const Culprit& k) {
                return k.kind == v[i]->kind && k.pid == v[i]->pid && k.start_ticks == v[i]->start_ticks && k.name == v[i]->name;
            });
            if (!have && key(*v[i]) > 0) kept.push_back(*v[i]);
        }
    };
    if (e.kinds & KindCpu) add_top([](const Culprit& c) { return c.cpu_seconds; });
    if (e.kinds & KindMemory) {
        add_top([](const Culprit& c) { return std::max(double(c.rss_growth) * 10, double(c.peak_rss)); });
        add_top([](const Culprit& c) { return double(c.faults); });
    }
    if (e.kinds & KindDisk) add_top([](const Culprit& c) { return c.io_bytes; });
    if (e.kinds & KindGpu) add_top([](const Culprit& c) { return c.gpu_mem; });
    e.culprits = std::move(kept);
    e.summary = describe(e);
    return e;
}

namespace {

std::string culprit_phrase(const Culprit& c, unsigned kind) {
    std::string who;
    if (c.kind == "group")
        who = fmt("%d short-lived %s processes started by %s", c.count, c.name.c_str(), c.parent.c_str());
    else if (c.kind == "unseen")
        who = "programs too brief to catch by name, started by " + c.name + (c.exited ? " (since exited)" : "");
    else if (c.kind == "unit")
        who = "other work inside " + c.name;
    else
        who = c.name + fmt(" (PID %d)", c.pid);

    std::string how;
    if (kind == KindMemory && c.rss_growth <= 50e6 && c.faults >= 100)
        how = fmt("waited on %llu swapped-out pages", (unsigned long long)c.faults);
    else if (kind == KindMemory) how = c.rss_growth > 50e6 ? "grew by " + human_bytes(double(c.rss_growth)) : "held " + human_bytes(double(c.peak_rss));
    else if (kind == KindDisk) how = "moved " + human_bytes(c.io_bytes);
    else if (kind == KindGpu) how = c.gpu_mem > 0 ? human_bytes(c.gpu_mem) + " of video memory" : fmt("%.0f%% of the CPU time", c.share * 100);
    else how = fmt("%.0f%% of the CPU time", c.share * 100);

    std::string status = c.kind == "process" && c.exited ? ", since exited" : "";
    return who + " (" + how + status + ")";
}

// The culprits that matter for one resource, best first, at most three.
std::vector<const Culprit*> leaders(const std::vector<Culprit>& all, unsigned kind) {
    std::vector<const Culprit*> v;
    for (const auto& c : all) {
        bool matters = kind == KindMemory ? (c.rss_growth > 50e6 || c.peak_rss > 1e9 || c.faults >= 100)
                     : kind == KindDisk   ? c.io_bytes > 10e6
                     : kind == KindGpu    ? c.gpu_mem > 100e6
                                          : c.share >= 0.05;
        if (matters) v.push_back(&c);
    }
    auto key = [kind](const Culprit* c) {
        if (kind == KindMemory)
            return std::max(c->rss_growth > 50e6 ? double(c->rss_growth) * 10 : double(c->peak_rss), double(c->faults) * 4096 * 20);
        if (kind == KindDisk) return c->io_bytes;
        if (kind == KindGpu) return c->gpu_mem;
        return c->cpu_seconds;
    };
    std::stable_sort(v.begin(), v.end(), [&](auto a, auto b) { return key(a) > key(b); });
    if (v.size() > 3) v.resize(3);
    return v;
}

std::string list_of(const std::vector<const Culprit*>& v, unsigned kind) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += i + 1 == v.size() ? " and " : ", ";
        s += culprit_phrase(*v[i], kind);
    }
    return s;
}

}  // namespace

std::string describe(const Event& e) {
    std::string s = clock_time(e.start) + " to " + clock_time(e.end) + " (" + human_duration(std::max(1.0, e.end - e.start)) + ").";
    bool any = false;
    if (e.kinds & KindCpu || e.kinds & KindHeat) {
        s += fmt(" CPU peaked at %.0f%%", e.peak_cpu);
        if (e.kinds & KindHeat && e.peak_temp >= 0) s += fmt(", the CPU reached %.0f °C", e.peak_temp);
        if (e.kinds & KindHeat && e.peak_fan >= 0) s += fmt(" with the fan at %.0f%%", e.peak_fan);
        auto v = leaders(e.culprits, KindCpu);
        s += v.empty() ? ", spread across many programs." : ", mostly " + list_of(v, KindCpu) + ".";
        any = true;
    }
    if (e.min_margin >= 0 && e.min_margin <= 15 && e.guard_limit > 0) {
        s += fmt(" It came within %.0f °C of the %.0f °C point where the system shuts itself down", e.min_margin, e.guard_limit);
        if (e.min_seconds >= 0 && e.min_seconds < 120)
            s += fmt("; at its fastest climb that was about %.0f seconds away", e.min_seconds);
        s += ".";
        any = true;
    }
    if (!e.dead_fans.empty()) {
        std::string fans;
        for (std::size_t i = 0; i < e.dead_fans.size(); ++i) fans += (i ? " and the " : "") + e.dead_fans[i];
        s += " The " + fans + (e.dead_fans.size() > 1 ? " were" : " was") + " not spinning while the machine was hot.";
    }
    if (e.kinds & KindMemory) {
        s += fmt(" Memory use reached %.0f%%", e.peak_mem_pct);
        if (e.peak_pressure >= 1) s += fmt(" and programs waited on memory up to %.0f%% of the time", e.peak_pressure);
        auto v = leaders(e.culprits, KindMemory);
        if (!v.empty()) {
            s += "; the biggest were " + list_of(v, KindMemory) + ".";
        } else {
            // Nothing stood out by name: say what tripped it.
            for (const auto& t : e.triggers)
                if (t.find("emory") != std::string::npos || t.find("wap") != std::string::npos) {
                    s += "; " + std::string(1, char(std::tolower(t[0]))) + t.substr(1);
                    break;
                }
            s += ", with no single program standing out.";
        }
        any = true;
    }
    if (e.kinds & KindDisk) {
        s += " Disk traffic peaked at " + human_bytes(e.peak_disk_bps) + "/s";
        auto v = leaders(e.culprits, KindDisk);
        s += v.empty() ? " (from programs whose disk use only the system can see)." : ", from " + list_of(v, KindDisk) + ".";
        any = true;
    }
    if (e.kinds & KindGpu) {
        s += fmt(" GPU load peaked at %.0f%%", e.peak_gpu);
        auto v = leaders(e.culprits, KindGpu);
        s += v.empty() ? "." : ", used by " + list_of(v, KindGpu) + ".";
        any = true;
    }
    if (!any) s += " No single program stood out.";
    return s;
}

}  // namespace as
