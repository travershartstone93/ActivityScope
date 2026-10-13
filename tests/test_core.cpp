// Core tests: run the measuring code against small fake /proc and /sys trees.
#include <sys/stat.h>
#include <sys/wait.h>

#include <csignal>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <functional>
#include <string>
#include <vector>

#include "actions.h"
#include "dirscan.h"
#include "logbook.h"
#include "netlog.h"
#include "peerwatch.h"
#include "persist.h"
#include "processlog.h"
#include "updates.h"
#include "upgradecheck.h"
#include <sqlite3.h>
#include "events.h"
#include "guard.h"
#include "providers.h"
#include "store.h"

namespace {

int failures = 0, checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

bool near(double a, double b, double tol = 0.01) { return std::abs(a - b) <= tol; }

// A throwaway folder tree that is removed when the test ends.
struct Tree {
    std::string root;
    Tree() {
        char tmpl[] = "/tmp/ascore-test-XXXXXX";
        root = mkdtemp(tmpl);
        mkdir((root + "/proc").c_str(), 0700);
        mkdir((root + "/sys").c_str(), 0700);
    }
    ~Tree() { std::system(("rm -rf '" + root + "'").c_str()); }
    void write(const std::string& rel, const std::string& text) {
        std::string path = root + "/" + rel;
        for (std::size_t i = root.size() + 1; i < path.size(); ++i)
            if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0700);
        std::ofstream(path) << text;
    }
    as::Roots roots() const { return {root + "/proc", root + "/sys", root + "/etc"}; }
};

std::string stat_line(int pid, const std::string& comm, char state, int ppid, uint64_t utime, uint64_t stime,
                      uint64_t start, uint64_t rss_pages, uint64_t cutime = 0) {
    // Fields 1..24 of /proc/pid/stat; only the ones the reader uses matter.
    char buf[512];
    std::snprintf(buf, sizeof buf, "%d (%s) %c %d 1 1 0 -1 4194560 100 0 0 0 %llu %llu %llu 0 20 0 1 0 %llu 1000000 %llu\n",
                  pid, comm.c_str(), state, ppid, (unsigned long long)utime, (unsigned long long)stime, (unsigned long long)cutime,
                  (unsigned long long)start, (unsigned long long)rss_pages);
    return buf;
}

void test_cpu() {
    std::puts("cpu");
    Tree t;
    t.write("proc/cpuinfo", "model name\t: Test CPU 9000\nflags\t\t: fpu vmx\n");
    t.write("sys/devices/system/cpu/cpu0/topology/core_cpus_list", "0-1\n");
    t.write("sys/devices/system/cpu/cpu1/topology/core_cpus_list", "0-1\n");
    t.write("proc/stat", "cpu 100 0 100 800 0 0 0 0\ncpu0 50 0 50 400 0 0 0 0\ncpu1 50 0 50 400 0 0 0 0\nbtime 1000\n");
    as::CpuProvider cpu(t.roots());
    as::CpuSample s;
    cpu.sample(s);
    CHECK(s.model == "Test CPU 9000");
    CHECK(s.logical == 2);
    CHECK(s.physical == 1);
    CHECK(s.virtualization);
    CHECK(!s.temp_c.ok());
    CHECK(!s.temp_c.why.empty());  // missing sensor says why, never a fake zero
    // 100 more busy ticks out of 200 total = 50%; core 1 fully busy.
    t.write("proc/stat", "cpu 150 0 150 900 0 0 0 0\ncpu0 50 0 50 500 0 0 0 0\ncpu1 100 0 100 400 0 0 0 0\nbtime 1000\n");
    cpu.sample(s);
    CHECK(near(s.total, 50));
    CHECK(near(s.kernel, 25));
    CHECK(s.per_core.size() == 2);
    CHECK(near(s.per_core[0], 0));
    CHECK(near(s.per_core[1], 100));
}

void test_memory() {
    std::puts("memory");
    Tree t;
    t.write("proc/meminfo", "MemTotal: 1000 kB\nMemAvailable: 250 kB\nCached: 100 kB\nSwapTotal: 400 kB\nSwapFree: 100 kB\n");
    t.write("proc/pressure/memory", "some avg10=3.50 avg60=1.00 avg300=0.10 total=1\nfull avg10=1.25 avg60=0 avg300=0 total=1\n");
    as::MemoryProvider m(t.roots());
    as::MemorySample s;
    m.sample(s);
    CHECK(s.total == 1000 * 1024);
    CHECK(s.used == 750 * 1024);
    CHECK(near(s.percent(), 75));
    CHECK(s.pressure_some10.ok() && near(s.pressure_some10.get(), 3.5));
    CHECK(near(s.pressure_full10.get(), 1.25));
}

void test_disks() {
    std::puts("disks");
    Tree t;
    t.write("sys/block/nvme0n1/size", "2000\n");
    t.write("sys/block/nvme0n1/device/model", "Fast Drive\n");
    t.write("sys/block/loop0/size", "100\n");
    auto stats = [](int rd, int wr, int ticks) {
        char buf[256];
        std::snprintf(buf, sizeof buf, " 259 0 nvme0n1 1 0 %d 0 1 0 %d 0 0 %d 0\n 259 1 nvme0n1p1 1 0 5 0 1 0 5 0 0 5 0\n 7 0 loop0 1 0 9 0 1 0 9 0 0 9 0\n", rd, wr, ticks);
        return std::string(buf);
    };
    t.write("proc/diskstats", stats(0, 0, 0));
    as::DiskProvider d(t.roots());
    std::vector<as::DiskSample> out;
    d.sample(out, 10.0);
    CHECK(out.size() == 1);  // loop devices and partitions are skipped
    CHECK(out[0].kind == "NVMe SSD");
    CHECK(out[0].size == 2000 * 512);
    t.write("proc/diskstats", stats(2048, 4096, 500));
    out.clear();
    d.sample(out, 11.0);
    CHECK(near(out[0].read_bps, 2048 * 512));
    CHECK(near(out[0].write_bps, 4096 * 512));
    CHECK(near(out[0].busy, 50));
}

void test_processes() {
    std::puts("processes");
    Tree t;
    t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
    t.write("proc/10/stat", stat_line(10, "worker", 'R', 1, 0, 0, 500, 100));
    t.write("proc/10/cmdline", std::string("worker\0--fast\0", 14));
    t.write("proc/10/cgroup", "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-worker.scope\n");
    t.write("proc/11/stat", stat_line(11, "a-very-long-name", 'S', 10, 0, 0, 600, 10));
    as::ProcessProvider pp(t.roots());
    as::Snapshot s1;
    s1.mono = 100;
    s1.wall = 2000;
    pp.sample(s1, 2, {});
    CHECK(s1.procs.size() == 2);
    const as::ProcessInfo* w = nullptr;
    for (auto& p : s1.procs)
        if (p.key.pid == 10) w = &p;
    CHECK(w && w->cmdline == "worker --fast");
    CHECK(w && w->unit == "app-worker.scope");
    CHECK(w && w->key.start_ticks == 500);

    // One second later: worker used 100 ticks (one full core of two = 50%).
    long hz = sysconf(_SC_CLK_TCK);
    t.write("proc/10/stat", stat_line(10, "worker", 'R', 1, uint64_t(hz), 0, 500, 100, uint64_t(hz) / 2));
    // pid 11 exited and its number was reused by a new program.
    t.write("proc/11/stat", stat_line(11, "newcomer", 'R', 1, 0, 0, 900, 10));
    as::Snapshot s2;
    s2.mono = 101;
    s2.wall = 2001;
    pp.sample(s2, 2, {});
    const as::ProcessInfo* w2 = nullptr;
    const as::ProcessInfo* n2 = nullptr;
    for (auto& p : s2.procs) {
        if (p.key.pid == 10) w2 = &p;
        if (p.key.pid == 11) n2 = &p;
    }
    CHECK(w2 && near(w2->cpu, 50, 0.5));
    CHECK(w2 && near(w2->child_cpu, 25, 0.5));  // reaped children's time
    CHECK(n2 && n2->name == "newcomer" && n2->is_new);
    bool sawExit = false;
    for (auto& e : s2.exited) sawExit |= e.key.pid == 11 && e.key.start_ticks == 600;
    CHECK(sawExit);  // the recycled pid's old owner is reported as exited
}

void test_cgroups() {
    std::puts("cgroups");
    Tree t;
    const std::string base = "sys/fs/cgroup/user.slice/user-1000.slice/user@1000.service/app.slice/";
    t.write(base + "app-build.scope/cpu.stat", "usage_usec 0\n");
    t.write("sys/fs/cgroup/system.slice/demo.service/cpu.stat", "usage_usec 0\n");
    as::CgroupProvider cg(t.roots());
    std::vector<as::UnitSample> out;
    cg.sample(out, 10, 2);
    t.write(base + "app-build.scope/cpu.stat", "usage_usec 1000000\n");
    out.clear();
    cg.sample(out, 11, 2);
    bool found = false;
    for (auto& u : out) {
        if (u.name == "user@1000.service") CHECK(false);  // parents with child units are skipped
        if (u.name == "app-build.scope") {
            found = true;
            CHECK(near(u.cpu, 50));
        }
    }
    CHECK(found);
}

as::SnapshotPtr snap(double t, double cpu, std::vector<as::ProcessInfo> procs = {}, std::vector<as::ProcessInfo> exited = {}) {
    auto s = std::make_shared<as::Snapshot>();
    s->mono = t;
    s->wall = 1'700'000'000 + t;
    s->cpu.logical = 4;
    s->cpu.total = cpu;
    s->cpu.per_core.assign(4, cpu);
    s->mem.total = 8e9;
    s->mem.available = 4e9;
    s->mem.used = 4e9;
    s->procs = std::move(procs);
    s->exited = std::move(exited);
    return s;
}

as::ProcessInfo proc(int pid, const std::string& name, double cpu, int ppid = 1, double started = 0) {
    as::ProcessInfo p;
    p.key = {pid, uint64_t(pid) * 10};
    p.name = name;
    p.ppid = ppid;
    p.cpu = cpu;
    p.started_epoch = started;
    return p;
}

void test_events() {
    std::puts("events");
    as::Triggers trig;
    as::EventEngine e(trig);
    double t = 0;
    // Calm: nothing opens.
    for (int i = 0; i < 5; ++i) CHECK(e.feed(snap(t++, 10, {proc(5, "editor", 2)})).empty());
    CHECK(!e.has_open());
    // A hog takes 80% for 6 seconds.
    for (int i = 0; i < 6; ++i) e.feed(snap(t++, 80, {proc(5, "editor", 2), proc(7, "renderer", 75)}));
    CHECK(e.has_open());
    auto open = e.open_event();
    CHECK(open && (open->kinds & as::KindCpu));
    CHECK(open && !open->culprits.empty() && open->culprits.front().name == "renderer");
    // A brief dip shorter than the merge window keeps one event.
    for (int i = 0; i < 10; ++i) CHECK(e.feed(snap(t++, 10, {proc(5, "editor", 2)})).empty());
    for (int i = 0; i < 3; ++i) e.feed(snap(t++, 80, {proc(5, "editor", 2), proc(7, "renderer", 75)}));
    // Then a long calm closes it.
    std::vector<as::Event> closed;
    for (int i = 0; i < 40 && closed.empty(); ++i) closed = e.feed(snap(t++, 5, {proc(5, "editor", 2)}));
    CHECK(closed.size() == 1);
    CHECK(!e.has_open());
    if (!closed.empty()) {
        CHECK(closed[0].culprits.front().name == "renderer");
        CHECK(closed[0].summary.find("renderer") != std::string::npos);
        CHECK(closed[0].end - closed[0].start >= 18);  // both bursts are in one event
    }
}

void test_blips_dropped() {
    std::puts("short blips dropped");
    as::EventEngine e;
    double t = 0;
    for (int i = 0; i < 3; ++i) e.feed(snap(t++, 5));
    // Two seconds of 60% CPU with nobody standing out, then calm.
    std::vector<as::Event> closed;
    for (int i = 0; i < 2; ++i) e.feed(snap(t++, 60, {proc(9, "a", 4), proc(10, "b", 4)}));
    for (int i = 0; i < 45 && closed.empty(); ++i) closed = e.feed(snap(t++, 5));
    CHECK(closed.empty());
    CHECK(!e.has_open());
}

void test_short_lived_group() {
    std::puts("short-lived grouping");
    as::EventEngine e;
    double t = 0;
    e.feed(snap(t++, 5, {proc(2, "make", 1)}));
    std::vector<as::Event> closed;
    // Each second a new compiler appears, burns a core and is gone next second.
    for (int i = 0; i < 8; ++i) {
        int pid = 100 + i;
        double now = 1'700'000'000 + t;
        auto p = proc(pid, "cc1plus", 25, 2, now - 0.5);
        std::vector<as::ProcessInfo> exited;
        if (i > 0) {
            auto prev = proc(pid - 1, "cc1plus", 0, 2, now - 1.5);
            prev.state = "exited";
            exited.push_back(prev);
        }
        e.feed(snap(t++, 70, {proc(2, "make", 1), p}, exited));
    }
    for (int i = 0; i < 40 && closed.empty(); ++i) {
        std::vector<as::ProcessInfo> exited;
        if (i == 0) exited.push_back(proc(107, "cc1plus", 0, 2, 1'700'000'000 + t - 1.5));
        closed = e.feed(snap(t++, 3, {proc(2, "make", 1)}, exited));
    }
    CHECK(closed.size() == 1);
    if (!closed.empty()) {
        const auto& top = closed[0].culprits.front();
        CHECK(top.kind == "group");
        CHECK(top.name == "cc1plus");
        CHECK(top.parent == "make");
        CHECK(top.count >= 3);
    }
}

void test_store() {
    std::puts("store");
    Tree t;
    std::string db = t.root + "/h.db";
    {
        as::Store st(db);
        CHECK(st.ok());
        auto s = snap(0, 42);
        st.add_sample(*s);
        st.flush();
        as::Event ev;
        ev.start = 1'700'000'000;
        ev.end = ev.start + 20;
        ev.kinds = as::KindCpu;
        ev.primary = as::KindCpu;
        ev.summary = "test summary";
        as::Culprit c;
        c.name = "renderer";
        c.cpu_seconds = 12;
        ev.culprits.push_back(c);
        ev.samples.push_back({ev.start, 80});
        int64_t id = st.add_event(ev);
        CHECK(id > 0);
        auto back = st.event(id);
        CHECK(back.summary == "test summary");
        CHECK(back.culprits.size() == 1 && back.culprits[0].name == "renderer");
        CHECK(back.samples.size() == 1);
        CHECK(st.samples(s->wall - 1, s->wall + 1).size() == 1);
        // Pruning keeps 24 h of samples and 30 days of events.
        st.prune(s->wall + 25 * 3600, 24, 30);
        CHECK(st.samples(0, 1e12).empty());
        CHECK(st.events(0, 1e12).size() == 1);
        st.prune(ev.start + 31 * 86400, 24, 30);
        CHECK(st.events(0, 1e12).empty());
    }
}

void test_end_process() {
    std::puts("end process");
    pid_t child = fork();
    if (child == 0) {
        execlp("sleep", "sleep", "30", nullptr);
        _exit(1);
    }
    usleep(100000);
    auto stat = as::read_file("/proc/" + std::to_string(child) + "/stat");
    CHECK(stat.has_value());
    uint64_t start = 0;
    if (stat) {
        auto f = as::split_ws(std::string_view(*stat).substr(stat->rfind(')') + 2));
        start = as::to_uint(f[19]).value_or(0);
    }
    // A key whose start time does not match is a different (recycled) process.
    std::string refused = as::signal_process({int(child), start + 1}, SIGTERM);
    CHECK(!refused.empty());
    CHECK(kill(child, 0) == 0);  // still alive
    std::string ok = as::signal_process({int(child), start}, SIGTERM);
    CHECK(ok.empty());
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
}

void test_pause_resume() {
    std::puts("pause and resume");
    pid_t child = fork();
    if (child == 0) {
        execlp("sleep", "sleep", "30", nullptr);
        _exit(1);
    }
    usleep(100000);
    auto stat = as::read_file("/proc/" + std::to_string(child) + "/stat");
    auto f = as::split_ws(std::string_view(*stat).substr(stat->rfind(')') + 2));
    as::ProcessKey key{int(child), as::to_uint(f[19]).value_or(0)};
    auto state = [&] {
        auto st = as::read_file("/proc/" + std::to_string(child) + "/stat");
        return st ? (*st)[st->rfind(')') + 2] : '?';
    };
    CHECK(as::signal_process(key, SIGSTOP).empty());
    usleep(100000);
    CHECK(state() == 'T');  // paused
    CHECK(as::signal_process(key, SIGCONT).empty());
    usleep(100000);
    CHECK(state() == 'S');  // running (sleeping) again
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
}

void test_guard() {
    std::puts("shutdown watch");
    Tree t;
    t.write("sys/class/thermal/thermal_zone0/type", "acpitz\n");
    t.write("sys/class/thermal/thermal_zone0/trip_point_0_type", "critical\n");
    t.write("sys/class/thermal/thermal_zone0/trip_point_0_temp", "100000\n");
    t.write("sys/class/thermal/thermal_zone1/type", "INT3400 Thermal\n");
    t.write("sys/class/thermal/thermal_zone1/mode", "disabled\n");
    as::ThermalGuard g(t.roots());
    // Climbing 1 °C per second from 80 °C.
    as::Snapshot s;
    for (int i = 0; i <= 10; ++i) {
        t.write("sys/class/thermal/thermal_zone0/temp", std::to_string((80 + i) * 1000) + "\n");  // within 20 °C of 100
        s = as::Snapshot{};
        s.mono = s.wall = 1000 + i;
        g.update(s);
    }
    CHECK(s.guard.zone == "acpitz");
    CHECK(s.guard.limit.ok() && near(s.guard.limit.get(), 100));
    // The median of the last three readings trails a steady climb by one.
    CHECK(s.guard.margin.ok() && near(s.guard.margin.get(), 11));
    CHECK(near(s.guard.raw.get(), 90));
    CHECK(s.guard.rate.ok() && near(s.guard.rate.get(), 1.0, 0.1));
    CHECK(s.guard.seconds_left.ok() && near(s.guard.seconds_left.get(), 11, 1.5));
    // Steady: no countdown.
    for (int i = 0; i < 16; ++i) {
        s = as::Snapshot{};
        s.mono = s.wall = 1011 + i;
        g.update(s);
    }
    CHECK(!s.guard.seconds_left.ok());
    // A one-second blip (59 -> 87 -> 63, as seen on this machine) is ignored.
    as::ThermalGuard fresh(t.roots());
    double when = 5000;
    for (double v : {52.0, 64.0, 59.0, 87.0, 63.0, 53.0}) {
        t.write("sys/class/thermal/thermal_zone0/temp", std::to_string(int(v * 1000)) + "\n");
        s = as::Snapshot{};
        s.mono = s.wall = when++;
        fresh.update(s);
        CHECK(s.guard.temp.get() < 70);
        CHECK(!s.guard.seconds_left.ok() || s.guard.seconds_left.get() > 60);
    }
    CHECK(near(s.guard.raw.get(), 53));
}

void test_dead_fan() {
    std::puts("dead fan");
    Tree t;
    as::ThermalGuard g(t.roots());
    auto snapWith = [](double cpuTemp, int fanA, int fanB, double when) {
        as::Snapshot s;
        s.mono = s.wall = when;
        s.cpu.temp_c = as::Reading<double>::of(cpuTemp);
        as::FanSample a, b;
        a.label = "CPU fan";
        a.percent = as::Reading<int>::of(fanA);
        b.label = "GPU fan";
        b.percent = as::Reading<int>::of(fanB);
        s.power.fans = {a, b};
        return s;
    };
    // Cool machine with a fan at rest: not a fault.
    as::Snapshot s;
    for (int i = 0; i < 60; ++i) {
        s = snapWith(55, 50, 0, i);
        g.update(s);
    }
    CHECK(s.guard.dead_fans.empty());
    // Hot, one fan working hard, the other still for 30 s: flagged.
    for (int i = 0; i < 31; ++i) {
        s = snapWith(90, 85, 0, 100 + i);
        g.update(s);
    }
    CHECK(s.guard.dead_fans.size() == 1 && s.guard.dead_fans[0] == "GPU fan");
    // It spins once: cleared.
    s = snapWith(90, 85, 30, 200);
    g.update(s);
    CHECK(s.guard.dead_fans.empty());
}

void test_boot_end() {
    std::puts("boot endings");
    // Lines as they appear in this machine's own log.
    auto thermal = as::classify_boot_end(
        "2026-10-06T10:08:36-04:00 whisper kernel: PM: suspend exit\n"
        "2026-10-06T10:08:41-04:00 whisper kernel: thermal thermal_zone0: acpitz: critical temperature reached\n"
        "2026-10-06T10:08:41-04:00 whisper kernel: reboot: HARDWARE PROTECTION shutdown (Temperature too high)\n");
    CHECK(thermal.kind == as::BootEnd::Thermal);
    auto clean = as::classify_boot_end("x systemd-logind[1]: System is powering down.\nx systemd[1]: Unmounted /srv.\n");
    CHECK(clean.kind == as::BootEnd::Clean);
    auto asleep = as::classify_boot_end("x kernel: PM: suspend exit\nx systemd-sleep[9]: Performing sleep operation 'suspend'...\n"
                                        "x kernel: PM: suspend entry (deep)\n");
    CHECK(asleep.kind == as::BootEnd::Asleep);
    auto woke = as::classify_boot_end("x kernel: PM: suspend entry (deep)\nx kernel: PM: suspend exit\nx tailscaled[1]: timeout\n");
    CHECK(woke.kind == as::BootEnd::Sudden);
    auto forced = as::classify_boot_end("10:08:41 whisper systemd[1]: Stopped Authorization Manager.\n"
                                        "10:08:41 whisper kernel: reboot: Hardware protection timed-out. Trying forced shutdown\n");
    CHECK(forced.kind == as::BootEnd::Thermal);
    auto unmounted = as::classify_boot_end("18:11:32 whisper systemd[1]: Unmounted /home.\n18:11:32 whisper systemd[1]: Unmounted /srv.\n");
    CHECK(unmounted.kind == as::BootEnd::Clean);
    auto sudden = as::classify_boot_end("x tailscaled[753]: all connection attempts failed\n");
    CHECK(sudden.kind == as::BootEnd::Sudden);
}

void test_event_margin_text() {
    std::puts("event closest approach");
    as::EventEngine e;
    double t = 0;
    auto hot = [&](double guard, double cpu) {
        auto s = snap(t++, cpu, {proc(7, "renderer", cpu - 5)});
        auto* m = const_cast<as::Snapshot*>(s.get());
        m->guard.zone = "acpitz";
        m->guard.limit = as::Reading<double>::of(100);
        m->guard.temp = as::Reading<double>::of(guard);
        m->guard.margin = as::Reading<double>::of(100 - guard);
        m->guard.seconds_left = as::Reading<double>::of((100 - guard) / 0.5);
        return s;
    };
    for (int i = 0; i < 8; ++i) e.feed(hot(90 + i, 80));
    std::vector<as::Event> closed;
    for (int i = 0; i < 45 && closed.empty(); ++i) closed = e.feed(hot(70, 5));
    CHECK(closed.size() == 1);
    if (!closed.empty()) {
        CHECK(near(closed[0].min_margin, 3));
        CHECK(closed[0].summary.find("within 3 °C of the 100 °C point") != std::string::npos);
        CHECK(closed[0].summary.find("seconds away") != std::string::npos);
    }
}

void test_dirscan() {
    std::puts("folder scan");
    Tree t;
    t.write("big/a.bin", std::string(200000, 'x'));
    t.write("big/inner/b.bin", std::string(100000, 'y'));
    t.write("small/c.txt", "hello");
    symlink((t.root + "/big").c_str(), (t.root + "/link").c_str());  // links are not followed
    std::atomic<bool> cancel{false};
    std::atomic<long long> files{0}, bytes{0};
    auto root = as::scan_dir(t.root, cancel, files, bytes);
    const as::DirNode* big = nullptr;
    for (const auto& k : root->kids)
        if (k->name == "big") big = k.get();
    CHECK(big != nullptr);
    CHECK(big && big->files == 2);
    CHECK(big && big->bytes >= 300000);
    // The link is one tiny entry; the folder it points to is not counted twice.
    CHECK(root->files == 4);
    CHECK(big && root->bytes < big->bytes + 100000);
    CHECK(files.load() == 4);
}

void test_logbook() {
    std::puts("log parsers");
    auto j = [](const char* id, const char* msg, bool kernel = false) {
        as::JournalLine l;
        l.t = 1000;
        l.identifier = id;
        l.message = msg;
        l.kernel = kernel;
        return as::classify_journal(l);
    };
    auto sudo = j("sudo", " whisper : TTY=pts/4 ; PWD=/home/whisper ; USER=root ; COMMAND=/usr/bin/pacman --remove -- ninvaders");
    CHECK(sudo && sudo->category == "admin");
    CHECK(sudo && sudo->title == "Ran as administrator (sudo) by whisper: /usr/bin/pacman --remove -- ninvaders");
    CHECK(!j("sudo", "pam_unix(sudo:session): session opened for user root(uid=0) by whisper(uid=1000)"));
    auto seg = j("kernel", "zsh[952460]: segfault at 7fffc9d05668 ip 0000561ca233740d sp 00007fffc9d05668 error 6 in zsh[3940d]", true);
    CHECK(seg && seg->category == "problems" && seg->title == "zsh crashed (segmentation fault)");
    auto oom = j("kernel", "Out of memory: Killed process 4242 (blender) total-vm:9000kB", true);
    CHECK(oom && oom->title == "Out of memory: the kernel killed blender");
    CHECK(j("systemd-sleep", "System returned from sleep operation 'suspend'.")->title == "Woke from sleep");
    auto wifi = j("NetworkManager", "<info>  [1.2] policy: set 'McNamara Apt. 6' (wlan0) as default for IPv4 routing and DNS");
    CHECK(wifi && wifi->title == "Connected to McNamara Apt. 6 on wlan0");
    CHECK(!j("kernel", "[UFW BLOCK] IN=wlan0 OUT= SRC=192.168.3.1", true));
    auto fail = j("systemd", "conky.service: Failed with result 'exit-code'.");
    CHECK(fail && fail->category == "services" && fail->title.find("conky.service failed") == 0);

    as::PacmanParser pp;
    std::optional<as::LogEntry> done;
    for (const char* line : {"[2026-10-10T09:36:20-0400] [PACMAN] Running 'pacman -S esptool'",
                             "[2026-10-10T09:36:21-0400] [ALPM] transaction started",
                             "[2026-10-10T09:36:22-0400] [ALPM] installed esptool (4.8.1-1)",
                             "[2026-10-10T09:36:22-0400] [ALPM] upgraded python-pyserial (3.5-6 -> 3.5-7)",
                             "[2026-10-10T09:36:23-0400] [ALPM] transaction completed"})
        if (auto e = pp.feed(line)) done = e;
    CHECK(done && done->category == "software");
    CHECK(done && done->title == "Installed esptool; upgraded python-pyserial");
    CHECK(done && done->detail.find("pacman -S esptool") != std::string::npos);
    CHECK(near(as::parse_pacman_time("[2026-10-10T09:36:21-0400] x"), 1791639381));

    auto esp = as::recognise_tool("python3", "python3 /home/me/.arduino15/esptool/esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x10000 sketch.ino.bin");
    CHECK(esp && esp->category == "flashing" && esp->port == "/dev/ttyACM0" && esp->image == "sketch.ino.bin");
    auto ard = as::recognise_tool("arduino-cli", "arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:esp32s3 --input-file fw.bin");
    CHECK(ard && ard->port == "/dev/ttyACM0" && ard->image == "fw.bin");
    auto mk = as::recognise_tool("mkfs.ext4", "mkfs.ext4 -L data /dev/sdb1");
    CHECK(mk && mk->category == "storage" && mk->what == "Formatted /dev/sdb1 with mkfs.ext4");
    auto dd = as::recognise_tool("dd", "dd if=archlinux.iso of=/dev/sdb bs=4M status=progress");
    CHECK(dd && dd->port == "/dev/sdb" && dd->image == "archlinux.iso");
    CHECK(!as::recognise_tool("dd", "dd if=/dev/zero of=/tmp/blob bs=1M count=10"));
    CHECK(!as::recognise_tool("arduino-cli", "arduino-cli compile --fqbn x sketch"));
    CHECK(!as::recognise_tool("zsh", "zsh -c python3 esptool.py --port /dev/ttyACM0 write_flash 0x0 fw.bin"));
    CHECK(as::board_name("303a", "1001").find("ESP32") != std::string::npos);
    CHECK(as::board_name("10C4", "EA60").find("CP210x") != std::string::npos);
    CHECK(as::board_name("dead", "beef").empty());

    // A crash loop: kernel and crash-reporter lines for 8 crashes in 11 s.
    as::Repeats rep;
    std::vector<as::LogEntry> burst;
    for (int i = 0; i < 8; ++i) {
        burst.push_back({1000.0 + i * 1.5, "problems", "zsh crashed (segmentation fault)", ""});
        burst.push_back({1000.4 + i * 1.5, "problems", "zsh crashed", ""});
    }
    auto first = rep.feed(burst, 1012);
    CHECK(first.size() == 1 && first[0].title == "zsh crashed");
    auto later = rep.feed({}, 1100);
    CHECK(later.size() == 1 && later[0].title.find("zsh crashed 8 times between") == 0);
}

void test_history_records() {
    std::puts("program and connection history");
    as::ProcessLog pl;
    auto s1 = std::make_shared<as::Snapshot>();
    s1->wall = 5000;
    as::ProcessInfo shell = proc(10, "fish", 0, 1);
    as::ProcessInfo tool = proc(20, "esptool", 30, 10, 4990);
    tool.cmdline = "esptool --port /dev/ttyACM0 write_flash 0x0 fw.bin";
    tool.cpu_seconds = 3;
    tool.rss = 40e6;
    s1->procs = {shell, tool};
    auto r1 = pl.feed(*s1);
    CHECK(r1.size() == 2);
    as::ProcessInfo* t1 = nullptr;
    for (auto& p : s1->procs) if (p.name == "esptool") t1 = &p;
    CHECK(t1 && r1[1].parent == "fish");
    auto s2 = std::make_shared<as::Snapshot>();
    s2->wall = 5010;
    s2->procs = {shell};
    tool.cpu_seconds = 8;
    s2->exited = {tool};
    auto r2 = pl.feed(*s2);
    // The exited entry carries the final totals (8), ahead of the last scan (3).
    CHECK(r2.size() == 1 && r2[0].name == "esptool" && near(r2[0].ended, 5010) && near(r2[0].cpu_seconds, 8));

    int port = 0;
    CHECK(as::format_address("0100007F:0035", false, &port) == "127.0.0.1" && port == 53);
    CHECK(as::format_address("0000000000000000FFFF00000100A8C0:01BB", true, &port) == "192.168.0.1" && port == 443);
    auto socks = as::parse_proc_net("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
                                    "   0: 0100A8C0:D2F0 22B2C268:01BB 01 00000000:00000000 00:00000000 00000000  1000        0 123456 1\n", "tcp");
    CHECK(socks.size() == 1 && socks[0].remote == "104.194.178.34" && socks[0].remote_port == 443 && socks[0].state == 1 &&
          socks[0].inode == 123456 && socks[0].uid == 1000 && socks[0].local == "192.168.0.1" && socks[0].local_port == 54000);

    // Malformed or cut /proc/net lines are skipped, never thrown on.
    for (const char* bad : {"   0: 0100A8C0 22B2C268:01BB 01 0:0 00:0 0  1000 0 1 1\n",            // no port separator
                            "   0: 0100A8C0:D2F0 22B2C2:01BB 01 0:0 00:0 0  1000 0 1 1\n",         // address cut short
                            "   0: 0100A8C0:D2F0 22B2C268:01BB zz 0:0 00:0 0  1000 0 1 1\n",       // state not hex
                            "   0: 0100A8C0:D2F0 FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF:01BB 01 0:0 00:0 0 1000 0 1 1\n"})
        CHECK(as::parse_proc_net(std::string("header\n") + bad, "tcp").empty());
    CHECK(as::format_address("0100A8C0:FFFFF", false, nullptr).empty());  // port out of range

    CHECK(as::line_diff("a\nb\n", "a\nc\n") == "+ c\n- b\n");

    Tree t;
    t.write("home/.config/autostart/keep.desktop", "[Desktop Entry]\nExec=old\n");
    t.write("etc/hosts", "127.0.0.1 localhost\n");
    as::StartupWatch w(t.root + "/home", t.root + "/etc");
    CHECK(w.watching() >= 2);
    t.write("home/.config/autostart/new.desktop", "[Desktop Entry]\nExec=/tmp/evil\n");
    t.write("etc/hosts", "127.0.0.1 localhost\n1.2.3.4 bank.example\n");
    w.poll(100);
    auto entries = w.poll(103);  // reported after 2 quiet seconds
    bool sawNew = false, sawHosts = false;
    for (const auto& e : entries) {
        if (e.title.find("Autostart entry added") == 0 && e.detail.find("+ Exec=/tmp/evil") != std::string::npos) sawNew = true;
        if (e.title.find("System file changed") == 0 && e.detail.find("+ 1.2.3.4 bank.example") != std::string::npos) sawHosts = true;
    }
    CHECK(sawNew);
    CHECK(sawHosts);

    std::string dbp = t.root + "/h.db";
    {
        as::Store st(dbp);
        as::ProcessRecord pr = r1[1];
        st.add_procs({pr});
        pr.ended = 5010;
        pr.cpu_seconds = 8;
        st.add_procs({pr});  // the same process ending updates its row
        as::ConnectionRecord c{"mytool", "/home/me/mytool", "1.2.3.4", "TCP", 443, 77, 5000, 5000, true};
        CHECK(st.add_connections({c}).size() == 1);
        c.last_seen = 5015;
        CHECK(st.add_connections({c}).empty());  // known pair: not new again
        st.add_log({{5005, "devices", "Plugged in: ESP32 board", ""}});
    }
    sqlite3* db = nullptr;
    sqlite3_open(dbp.c_str(), &db);
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(db, "SELECT kind FROM v_timeline WHERE t BETWEEN 4980 AND 5020 ORDER BY t", -1, &q, nullptr);
    std::vector<std::string> kinds;
    while (sqlite3_step(q) == SQLITE_ROW) kinds.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(q, 0)));
    sqlite3_finalize(q);
    sqlite3_prepare_v2(db, "SELECT COUNT(*), MAX(ended) FROM procs", -1, &q, nullptr);
    sqlite3_step(q);
    CHECK(sqlite3_column_int(q, 0) == 1 && near(sqlite3_column_double(q, 1), 5010));
    sqlite3_finalize(q);
    sqlite3_close(db);
    CHECK((kinds == std::vector<std::string>{"program started", "first connection", "log: devices", "program ended"}));
}

void test_peerwatch() {
    std::puts("background sharing");
    as::PeerWatch w;
    std::set<std::string> allow = {"syncthing"};
    w.allowed = [&](const std::string& p) { return allow.count(p) > 0; };
    auto conns = [](const std::string& prog, int n, int base) {
        std::vector<as::ConnectionRecord> v;
        for (int i = 0; i < n; ++i) v.push_back({prog, "/usr/bin/" + prog, "10.0." + std::to_string(base) + "." + std::to_string(i), "TCP", 6881, 42, 0, 0, false});
        return v;
    };
    // A browser with three servers never alerts.
    CHECK(w.feed(conns("firefox", 3, 1), 0, 100).empty());
    // A torrent client collecting peers over a few rounds alerts once.
    CHECK(w.feed(conns("ktorrent", 5, 2), 300e3, 115).empty());
    auto a = w.feed(conns("ktorrent", 9, 3), 300e3, 130);
    CHECK(a.size() == 1 && a[0].program == "ktorrent" && a[0].peers >= 8 && a[0].pid == 42);
    CHECK(w.feed(conns("ktorrent", 12, 4), 300e3, 145).empty());  // not again the same day
    // An allowed sharer stays quiet.
    CHECK(w.feed(conns("syncthing", 20, 5), 0, 160).empty());
    // Peers older than the window are forgotten.
    CHECK(w.feed(conns("transmission", 6, 6), 0, 200).empty());
    CHECK(w.feed(conns("transmission", 4, 7), 0, 900).empty());  // the first 6 expired: only 4 now
}

void test_updates() {
    std::puts("updates");
    auto list = as::parse_update_list("linux-cachyos 7.2.2-1 -> 7.2.3-1\nmesa 1:26.1.0-1 -> 1:26.1.1-1\n\nnoise line\n", false);
    CHECK(list.size() == 2 && list[0].name == "linux-cachyos" && list[0].installed == "7.2.2-1" && list[0].available == "7.2.3-1");
    auto aur = as::parse_update_list("msi-ec-dkms-git 1-1 -> 2-1\n", true);
    CHECK(aur.size() == 1 && aur[0].aur);
    // The real pacman.conf here: cuda held in [options]; commented lines and other sections ignored.
    std::string conf = "[options]\nIgnorePkg = cuda\n#IgnorePkg   =\nIgnorePkg = cudnn nccl\n[core]\nIgnorePkg = notreally\n";
    auto held = as::pacman_ignored(conf);
    CHECK((held == std::set<std::string>{"cuda", "cudnn", "nccl"}));
    CHECK(as::needs_restart("linux-cachyos") && as::needs_restart("nvidia-utils") && as::needs_restart("linux-cachyos-nvidia-open"));
    CHECK(!as::needs_restart("linux-cachyos-headers") && !as::needs_restart("firefox"));
    CHECK(as::update_all_command({"cuda"}, true) == "sudo pacman -Syu --ignore cuda && paru -Sua --ignore cuda");
    CHECK(as::update_all_command({}, false) == "sudo pacman -Syu");
    CHECK(as::update_one_command(list[1]) == "sudo pacman -S --needed mesa");
    CHECK(as::update_one_command(aur[0]) == "paru -S msi-ec-dkms-git");
}

void test_upgradecheck() {
    CHECK(as::vercmp("4.2.1-1.1", "4.1.4-1") > 0);
    CHECK(as::vercmp("1.0-1", "1.0-2") < 0);
    CHECK(as::vercmp("1:1.0-1", "2.0-1") > 0);
    CHECK(as::vercmp("1.0a-1", "1.0-1") < 0);
    CHECK(as::vercmp("1.10-1", "1.9-1") > 0);
    CHECK(as::vercmp("2.78-1", "2.78-1") == 0);
    CHECK(as::satisfies("4.1.4-1", as::parse_dep("audit=4.1.4")));
    CHECK(!as::satisfies("4.2.1-1.1", as::parse_dep("audit=4.1.4")));
    CHECK(as::satisfies("14.2-1", as::parse_dep("gcc<15")));
    CHECK(!as::satisfies("15.1-1", as::parse_dep("gcc<15")));
    CHECK(as::soname_of_provide("libicuuc.so=76-64") == "libicuuc.so.76");
    CHECK(as::soname_of_provide("sh").empty());
    CHECK(as::parse_size("12.50 MiB") == 12.5 * 1024 * 1024);

    const std::string refused =
        "error: failed to prepare transaction (could not satisfy dependencies)\n"
        ":: installing audit (4.2.1-1.1) breaks dependency 'audit=4.1.4' required by lib32-audit\n";
    auto r = as::parse_refusals(refused);
    CHECK(r.size() == 1 && r[0].kind == as::Refusal::BreaksDep);
    CHECK(r[0].package == "audit" && r[0].version == "4.2.1-1.1" && r[0].dep == "audit=4.1.4" && r[0].by == "lib32-audit");
    auto c = as::parse_refusals(":: foo and bar are in conflict\nerror: target not found: baz\n");
    CHECK(c.size() == 2 && c[0].kind == as::Refusal::Conflict && c[1].kind == as::Refusal::NotFound && c[1].package == "baz");

    auto info = as::parse_pacman_info(
        "Name            : lib32-audit\nVersion         : 4.1.4-1\nDescription     : Userspace components of the audit\n"
        "                  framework\nRequired By     : lib32-pam\nPackager        : Someone <x@archlinux.org>\n\n"
        "Name            : lib32-audit\nVersion         : 9-9\n\n");
    CHECK(info.size() == 1 && info["lib32-audit"]["Version"] == "4.1.4-1");
    CHECK(info["lib32-audit"]["Description"] == "Userspace components of the audit framework");
    CHECK(as::info_list(info["lib32-audit"], "Required By") == std::vector<std::string>{"lib32-pam"});

    // The real case on this machine: lib32-libcap -> lib32-pam -> lib32-audit pin audit 4.1.4,
    // and all three were dropped from multilib. A fake pacman replays it.
    namespace fs = std::filesystem;
    std::string root = fs::temp_directory_path() / ("as-upg-" + std::to_string(getpid()));
    fs::create_directories(root + "/local/lib32-audit-4.1.4-1");
    fs::create_directories(root + "/local/lib32-pam-1.7.2-1");
    fs::create_directories(root + "/local/lib32-libcap-2.78-1");
    fs::create_directories(root + "/local/audit-4.1.4-1");
    fs::create_directories(root + "/sync");
    std::map<std::string, std::string> qi = {
        {"lib32-audit", "Name : lib32-audit\nVersion : 4.1.4-1\nDescription : audit 32-bit\nRequired By : lib32-pam\nInstall Reason : Installed as a dependency for another package\nPackager : A <a@archlinux.org>\n\n"},
        {"lib32-pam", "Name : lib32-pam\nVersion : 1.7.2-1\nDescription : pam 32-bit\nRequired By : lib32-libcap\nInstall Reason : Installed as a dependency for another package\nPackager : A <a@archlinux.org>\n\n"},
        {"lib32-libcap", "Name : lib32-libcap\nVersion : 2.78-1\nDescription : libcap 32-bit\nRequired By : None\nInstall Reason : Installed as a dependency for another package\nPackager : A <a@archlinux.org>\n\n"},
        {"audit", "Name : audit\nVersion : 4.1.4-1\nProvides : libaudit.so=1-64\n\n"},
    };
    int dry_runs = 0;
    as::Runner fake = [&](const std::vector<std::string>& a) -> as::CmdResult {
        std::string db;
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (a[i] == "--dbpath") db = a[i + 1];
        if (a[1] == "-Sup") {
            ++dry_runs;
            if (fs::exists(db + "/local/lib32-audit-4.1.4-1")) return {1, refused};
            // The warning line is stderr merged into the output; it must not count.
            return {0, "warning: cuda: ignoring package upgrade (12.1 -> 13.0)\naudit 4.2.1-1.1\n"};
        }
        if (a[1] == "-Qi") {
            std::string out;
            for (std::size_t i = 4; i < a.size(); ++i)
                if (qi.count(a[i])) out += qi[a[i]];
            return {0, out};
        }
        if (a[1] == "-Si") {
            std::string out;
            for (std::size_t i = 4; i < a.size(); ++i)
                if (a[i] == "audit") out += "Name : audit\nVersion : 4.2.1-1.1\nProvides : libaudit.so=1-64\nDownload Size : 1.00 MiB\nInstalled Size : 3.00 MiB\n\n";
            return {0, out};
        }
        if (a[1] == "-Rsp") return {0, "lib32-libcap\nlib32-pam\nlib32-libnsl\nlib32-libtirpc\nlib32-audit\n"};
        return {0, ""};
    };
    as::CheckInput in;
    in.sync_db = root + "/sync";
    in.local_db = root + "/local";
    in.scratch = root + "/scratch";
    in.held = {"cuda"};
    in.updates = {{"audit", "4.1.4-1", "4.2.1-1.1"}};
    in.home = root;
    in.cache_dir = root;
    std::vector<int> steps;
    in.progress = [&](int pct, const std::string&) { steps.push_back(pct); };
    auto res = as::check_upgrade(in, fake);
    in.progress = nullptr;
    CHECK(steps.size() > 3 && steps.back() == 100 && std::is_sorted(steps.begin(), steps.end()));
    CHECK(res.installs_cleanly && res.needed_fixes && res.planned == 1 && dry_runs == 2);
    CHECK(res.issues.size() == 1);
    if (!res.issues.empty()) {
        const auto& is = res.issues[0];
        CHECK(is.severity == "fail" && is.tested);
        CHECK(is.fixes.size() == 1 && is.fixes[0].kind == "remove");
        CHECK(is.fixes[0].targets == std::vector<std::string>{"lib32-libcap"});
        CHECK(is.fixes[0].command == "sudo pacman -Rns lib32-libcap");
        CHECK(is.fixes[0].removes.size() == 5 && is.fixes[0].label == "Remove broken packages");
        CHECK(is.title.find("no longer in the repositories") != std::string::npos);
        CHECK(is.explanation.find("32-bit") != std::string::npos);
    }
    CHECK(fs::exists(root + "/local/lib32-audit-4.1.4-1"));  // the real db is never touched

    // A broken copy of the database must stop the check, not fake a clean result.
    as::CheckInput bad = in;
    bad.local_db = root + "/no-such-dir";
    auto br = as::check_upgrade(bad, fake);
    CHECK(!br.installs_cleanly && br.issues.size() == 1 && br.issues[0].kind == "internal");

    auto needed = as::elf_needed("/usr/bin/ls");
    CHECK(std::find(needed.begin(), needed.end(), "libc.so.6") != needed.end());
    // An AUR program loading a library the upgrade takes away gets a rebuild warning.
    as::Runner lib = [&](const std::vector<std::string>& a) -> as::CmdResult {
        if (a[1] == "-Sup") return {0, "glibc 3.0-1\n"};
        if (a[1] == "-Qi" && a.back() == "glibc") return {0, "Name : glibc\nVersion : 2.42-1\nProvides : libc.so=6-64\n\n"};
        if (a[1] == "-Si" && a.back() == "glibc") return {0, "Name : glibc\nVersion : 3.0-1\nProvides : libc.so=7-64\n\n"};
        if (a[1] == "-Qmq") return {0, "fancy-ls-git\n"};
        if (a[1] == "-Qlq" && a.back() == "fancy-ls-git") return {0, "/usr/\n/usr/bin/\n/usr/bin/ls\n"};
        return {0, ""};
    };
    in.updates = {{"glibc", "2.42-1", "3.0-1"}};
    in.root = "/";
    auto lr = as::check_upgrade(in, lib);
    CHECK(lr.issues.size() == 1 && lr.issues[0].kind == "aur-library");
    if (!lr.issues.empty()) CHECK(lr.issues[0].fixes.size() == 1 && lr.issues[0].fixes[0].command == "paru -S fancy-ls-git");
    // A kernel update whose headers are not updated with it: DKMS drivers cannot rebuild.
    as::Runner kern = [&](const std::vector<std::string>& a) -> as::CmdResult {
        if (a[1] == "-Sup") return {0, "linux-cachyos 7.3-1\n"};
        if (a[1] == "-Qq") return {0, "linux-cachyos\nlinux-cachyos-headers\nmsi-ec-dkms-git\n"};
        std::string out;
        for (std::size_t i = 4; i < a.size(); ++i) {
            if (a[1] == "-Qi" && a[i] == "linux-cachyos") out += "Name : linux-cachyos\nVersion : 7.2-1\n\n";
            if (a[1] == "-Qi" && a[i] == "linux-cachyos-headers") out += "Name : linux-cachyos-headers\nVersion : 7.2-1\n\n";
            if (a[1] == "-Si" && a[i] == "linux-cachyos") out += "Name : linux-cachyos\nVersion : 7.3-1\n\n";
        }
        return {0, out};
    };
    in.updates = {{"linux-cachyos", "7.2-1", "7.3-1"}};
    auto kr = as::check_upgrade(in, kern);
    CHECK(kr.issues.size() == 1 && kr.issues[0].kind == "kernel" && kr.issues[0].severity == "warn");
    if (!kr.issues.empty()) CHECK(kr.issues[0].fixes.size() == 1 && kr.issues[0].fixes[0].targets[0] == "linux-cachyos");
    fs::remove_all(root);
}

}  // namespace

int main() {
    test_cpu();
    test_memory();
    test_disks();
    test_processes();
    test_cgroups();
    test_events();
    test_short_lived_group();
    test_blips_dropped();
    test_store();
    test_end_process();
    test_pause_resume();
    test_guard();
    test_dead_fan();
    test_boot_end();
    test_event_margin_text();
    test_dirscan();
    test_logbook();
    test_history_records();
    test_peerwatch();
    test_updates();
    test_upgradecheck();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
