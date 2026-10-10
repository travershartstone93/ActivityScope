// Core tests: run the measuring code against small fake /proc and /sys trees.
#include <sys/stat.h>
#include <sys/wait.h>

#include <csignal>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "actions.h"
#include "dirscan.h"
#include "logbook.h"
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

}  // namespace

int main() {
    test_cpu();
    test_memory();
    test_disks();
    test_processes();
    test_cgroups();
    test_events();
    test_short_lived_group();
    test_store();
    test_end_process();
    test_pause_resume();
    test_guard();
    test_dead_fan();
    test_boot_end();
    test_event_margin_text();
    test_dirscan();
    test_logbook();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
