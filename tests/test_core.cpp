// Core tests: run the measuring code against small fake /proc and /sys trees.
#include <sys/stat.h>
#include <sys/wait.h>

#include <csignal>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <functional>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <sqlite3.h>

#include "actions.h"
#include "askcore.h"
#include "diagnose.h"
#include "judge.h"
#include "sysid.h"
#include "dirscan.h"
#include "logbook.h"
#include "names.h"
#include "unllm/meaning.h"
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

// A child's CPU is counted while it runs. When it ends, its whole lifetime
// lands in its parent's reaped-children time; only the part never seen may be
// charged to the parent then.
void test_child_cpu_once() {
    std::puts("finished children counted once");
    const uint64_t H = uint64_t(sysconf(_SC_CLK_TCK));
    // Seconds charged in one sample: two cores, one-second samples.
    auto own = [](const as::Snapshot& s, int pid) {
        for (const auto& p : s.procs)
            if (p.key.pid == pid) return p.cpu * 2 / 100;
        return -1.0;
    };
    auto kids = [](const as::Snapshot& s, int pid) {
        for (const auto& p : s.procs)
            if (p.key.pid == pid) return p.child_cpu * 2 / 100;
        return -1.0;
    };
    auto gone = [](const Tree& t, int pid) { std::filesystem::remove_all(t.root + "/proc/" + std::to_string(pid)); };
    {
        Tree t;
        t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
        t.write("proc/20/stat", stat_line(20, "host", 'S', 1, 0, 0, 500, 10));
        as::ProcessProvider pp(t.roots());
        double clock = 0;
        auto sample = [&] {
            as::Snapshot s;
            s.mono = 100 + clock;
            s.wall = 2000 + clock;
            ++clock;
            pp.sample(s, 2, {});
            return s;
        };
        sample();
        // (a) A child born after the first sample, seen over three samples, then reaped.
        const uint64_t born = 1000 * H + H / 2;  // 2000.5
        t.write("proc/21/stat", stat_line(21, "worker", 'R', 20, H / 2, 0, born, 10));
        double seen = own(sample(), 21);
        t.write("proc/21/stat", stat_line(21, "worker", 'R', 20, H * 3 / 2, 0, born, 10));
        seen += own(sample(), 21);
        t.write("proc/21/stat", stat_line(21, "worker", 'R', 20, H * 5 / 2, 0, born, 10));
        seen += own(sample(), 21);
        CHECK(near(seen, 2.5, 0.05));
        gone(t, 21);
        uint64_t reaped = H * 5 / 2;
        t.write("proc/20/stat", stat_line(20, "host", 'S', 1, 0, 0, 500, 10, reaped));
        auto end = sample();
        CHECK(near(kids(end, 20), 0, 0.05));                 // all of it was seen: nothing more
        CHECK(near(seen + kids(end, 20), 2.5, 0.05));        // total equals the child's CPU
        // A second child is seen twice, then uses 0.3 s more before it ends:
        // only that last part lands on the parent.
        const uint64_t born2 = 1000 * H + H * 9 / 2;  // 2004.5
        t.write("proc/22/stat", stat_line(22, "worker", 'R', 20, H, 0, born2, 10));
        seen = own(sample(), 22);
        t.write("proc/22/stat", stat_line(22, "worker", 'R', 20, 2 * H, 0, born2, 10));
        seen += own(sample(), 22);
        gone(t, 22);
        reaped += 2 * H + H * 3 / 10;
        t.write("proc/20/stat", stat_line(20, "host", 'S', 1, 0, 0, 500, 10, reaped));
        end = sample();
        CHECK(near(kids(end, 20), 0.3, 0.05));
        CHECK(near(seen + kids(end, 20), 2.3, 0.05));
        // (b) Children that started and ended between two samples were never
        // seen: their 0.8 s still lands on the parent.
        reaped += H * 4 / 5;
        t.write("proc/20/stat", stat_line(20, "host", 'S', 1, 0, 0, 500, 10, reaped));
        CHECK(near(kids(sample(), 20), 0.8, 0.05));
    }
    {
        // (c) An orphan: its parent ends, systemd --user adopts it (the
        // subreaper), and reaps it later. systemd is not charged its 2 s.
        Tree t;
        t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
        t.write("proc/50/stat", stat_line(50, "systemd", 'S', 1, 0, 0, 100, 10));
        t.write("proc/60/stat", stat_line(60, "launcher", 'S', 50, H / 5, 0, 400, 10));
        t.write("proc/70/stat", stat_line(70, "worker", 'R', 60, 0, 0, 600, 10));
        as::ProcessProvider pp(t.roots());
        double clock = 0;
        auto sample = [&] {
            as::Snapshot s;
            s.mono = 100 + clock;
            s.wall = 2000 + clock;
            ++clock;
            pp.sample(s, 2, {});
            return s;
        };
        sample();
        gone(t, 60);
        t.write("proc/50/stat", stat_line(50, "systemd", 'S', 1, 0, 0, 100, 10, H / 5));
        t.write("proc/70/stat", stat_line(70, "worker", 'R', 50, H, 0, 600, 10));
        auto s1 = sample();
        CHECK(near(kids(s1, 50), 0, 0.05));
        double seen = own(s1, 70);
        t.write("proc/70/stat", stat_line(70, "worker", 'R', 50, 2 * H, 0, 600, 10));
        seen += own(sample(), 70);
        gone(t, 70);
        t.write("proc/50/stat", stat_line(50, "systemd", 'S', 1, 0, 0, 100, 10, H / 5 + 2 * H));
        auto s3 = sample();
        CHECK(near(seen, 2, 0.05));
        CHECK(near(kids(s3, 50), 0, 0.05));
    }
    {
        // (c, same interval) A shell's script and the script's worker end
        // together: the shell reaps the script, systemd --user adopts and
        // reaps the worker. Neither is charged again.
        Tree t;
        t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
        t.write("proc/50/stat", stat_line(50, "systemd", 'S', 1, 0, 0, 100, 10));
        t.write("proc/55/stat", stat_line(55, "bash", 'S', 50, 0, 0, 300, 10));
        t.write("proc/60/stat", stat_line(60, "python3", 'S', 55, H / 5, 0, 400, 10));
        t.write("proc/70/stat", stat_line(70, "worker", 'R', 60, H, 0, 600, 10));
        as::ProcessProvider pp(t.roots());
        double clock = 0;
        auto sample = [&] {
            as::Snapshot s;
            s.mono = 100 + clock;
            s.wall = 2000 + clock;
            ++clock;
            pp.sample(s, 2, {});
            return s;
        };
        sample();
        t.write("proc/70/stat", stat_line(70, "worker", 'R', 60, 2 * H, 0, 600, 10));
        sample();
        gone(t, 60);
        gone(t, 70);
        t.write("proc/55/stat", stat_line(55, "bash", 'S', 50, 0, 0, 300, 10, H / 5));
        t.write("proc/50/stat", stat_line(50, "systemd", 'S', 1, 0, 0, 100, 10, 2 * H));
        auto s2 = sample();
        CHECK(near(kids(s2, 55), 0, 0.05));
        CHECK(near(kids(s2, 50), 0, 0.05));
    }
    {
        // The parent's line was read just before it reaped the child, so the
        // child is gone this sample and the parent's jump shows up next one.
        Tree t;
        t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
        t.write("proc/20/stat", stat_line(20, "host", 'S', 1, 0, 0, 500, 10));
        t.write("proc/21/stat", stat_line(21, "worker", 'R', 20, 0, 0, 600, 10));
        as::ProcessProvider pp(t.roots());
        double clock = 0;
        auto sample = [&] {
            as::Snapshot s;
            s.mono = 100 + clock;
            s.wall = 2000 + clock;
            ++clock;
            pp.sample(s, 2, {});
            return s;
        };
        sample();
        t.write("proc/21/stat", stat_line(21, "worker", 'R', 20, H, 0, 600, 10));
        sample();
        gone(t, 21);
        CHECK(near(kids(sample(), 20), 0, 0.05));
        t.write("proc/20/stat", stat_line(20, "host", 'S', 1, 0, 0, 500, 10, H));
        CHECK(near(kids(sample(), 20), 0, 0.05));
    }
}

// The live case from 2026-10-23: a service of systemd --user burns half a
// core for 44 s, the machine gets busy, and the service ends during the busy
// spell. Its time was counted while it ran, so neither the minute's top list
// nor the event charges systemd for it again. Children never seen (5 s) still
// land on systemd, as an "unseen" row in the event.
void test_child_cpu_events() {
    std::puts("finished children not charged twice in minutes and events");
    const uint64_t H = uint64_t(sysconf(_SC_CLK_TCK));
    Tree t;
    t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
    as::ProcessProvider pp(t.roots());
    as::EventEngine e;
    std::vector<as::Event> closed;
    std::optional<as::MinuteTop> minute;
    const double T0 = 1'700'000'000;  // 20 s into a minute
    uint64_t used = 0, reaped = 0;
    for (int i = 0; i <= 100; ++i) {
        if (i >= 1 && i <= 44) used += H / 2;
        if (i <= 44) t.write("proc/70/stat", stat_line(70, "helper", 'R', 50, used, 0, 200, 10));
        if (i == 45) {
            std::filesystem::remove_all(t.root + "/proc/70");
            reaped += used;
        }
        if (i == 48) reaped += 5 * H;
        t.write("proc/50/stat", stat_line(50, "systemd", 'S', 1, 0, 0, 100, 10, reaped));
        auto s = std::make_shared<as::Snapshot>();
        s->mono = i;
        s->wall = T0 + i;
        s->cpu.logical = 2;
        s->cpu.total = i > 40 && i <= 50 ? 90 : 25;
        s->cpu.per_core.assign(2, s->cpu.total);
        s->mem.total = 8e9;
        s->mem.available = 4e9;
        s->mem.used = 4e9;
        pp.sample(*s, 2, {});
        for (auto& ev : e.feed(s)) closed.push_back(std::move(ev));
        if (auto m = e.take_minute(); m && m->minute == int64_t(T0) + 40) minute = m;
    }
    CHECK(closed.size() == 1);
    if (!closed.empty()) {
        double unseen = 0, helper = 0;
        for (const auto& c : closed[0].culprits) {
            if (c.kind == "unseen") unseen += c.cpu_seconds;
            if (c.name == "helper") helper += c.cpu_seconds;
        }
        CHECK(near(unseen, 5, 0.6));
        CHECK(helper > 5);
    }
    CHECK(minute.has_value());
    if (minute) {
        double sd = -1, helper = -1;
        for (const auto& c : minute->top) {
            if (c.name == "systemd") sd = c.cpu_seconds;
            if (c.name == "helper") helper = c.cpu_seconds;
        }
        CHECK(near(sd, 5, 0.6));       // only the never-seen children
        CHECK(near(helper, 2.5, 0.3));  // its own last five seconds
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

void test_judge() {
    std::puts("judge stats");
    using namespace as::judge;
    CHECK(median({10, 20, 30}) == 20 && median({10, 20, 30, 40}) == 20);  // lower median
    CHECK(mad({10, 20, 30}, 20) == 10);
    CHECK(percentile({40, 42, 44, 46, 48, 50, 52, 54, 56, 58}, 0.05) == 40);
    CHECK(percentile({40, 42, 44, 46, 48, 50, 52, 54, 56, 58}, 0.95) == 56);
    CHECK(percentile_rank({40, 42, 44, 46, 48, 50, 52, 54, 56, 58}, 55) == 80);
    // A steady rise is a trend; thirty flat days are not.
    std::vector<double> rising, flat;
    for (int i = 0; i < 30; ++i) {
        rising.push_back(50 + 0.5 * i + (i % 2 ? 0.3 : -0.3));
        flat.push_back(50 + (i % 2 ? 0.4 : -0.4));
    }
    auto tr = theil_sen_mk(rising);
    CHECK(tr.significant && tr.slope_per_step > 0.4 && tr.slope_per_step < 0.6);
    CHECK(!theil_sen_mk(flat).significant);
    // A step on day 20 is found as a step, at the right day.
    std::vector<double> step = flat;
    for (std::size_t i = 20; i < step.size(); ++i) step[i] += 10;
    auto ch = cusum(step);
    CHECK(ch.found && ch.index >= 19 && ch.index <= 21 && ch.shift_units > 3);
    CHECK(!cusum(flat).found);
}

void test_diagnose() {
    std::puts("diagnose");
    Tree t;
    std::string dbp = t.root + "/d.db";
    std::tm tmv{};
    tmv.tm_year = 2026 - 1900; tmv.tm_mon = 9; tmv.tm_mday = 13; tmv.tm_hour = 21; tmv.tm_min = 30; tmv.tm_isdst = -1;
    const double now = double(std::mktime(&tmv));
    {
        as::Store st(dbp);
        // Three hot events across two days, all with the same dead fan.
        for (int i = 0; i < 3; ++i) {
            as::Event e;
            e.start = now - (i < 2 ? 3600.0 * (i + 2) : 86400.0 + 3600);
            e.end = e.start + 300;
            e.kinds = as::KindHeat;
            e.primary = as::KindHeat;
            e.peak_temp = 95;
            e.summary = "hot event";
            e.dead_fans.push_back("GPU fan");
            st.add_event(e);
        }
        // A disk-wait event: low CPU, heavy disk, with a culprit.
        as::Event w;
        w.start = now - 7200;
        w.end = w.start + 120;
        w.kinds = as::KindDisk;
        w.primary = as::KindDisk;
        w.peak_cpu = 15;
        w.peak_disk_bps = 200e6;
        w.summary = "disk saturated";
        as::Culprit c;
        c.name = "updatedb";
        c.io_bytes = 5e9;
        w.culprits.push_back(c);
        st.add_event(w);
        // The kernel's own verdicts in the log.
        st.add_log({{now - 5000, "problems", "Disk read or write error: nvme0n1 reported a failed write", ""},
                    {now - 4000, "problems", "Out of memory: the kernel killed blender", ""}});
        // A leak signature: four rising daily peaks.
        for (int d = 4; d >= 1; --d) {
            as::ProcessRecord p{50 + d, now - d * 86400, now - d * 86400 + 600, "leaky", "", "", "", "", "", 1, 5,
                                uint64_t(100e6 * (6 - d) * 1.2)};
            st.add_procs({p});
        }
        st.flush();
    }
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2(dbp.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    as::Diagnose diag(db);
    auto cons = diag.run(int64_t(now) - 30 * 86400, int64_t(now), int64_t(now), nullptr);
    CHECK(cons.size() >= 4);
    // Hardware outranks everything: the dead fan and disk errors lead.
    CHECK(cons[0].rule_id == "dead_fan" || cons[0].rule_id == "disk_errors");
    bool fan = false, derr = false, oom = false, wait = false, leak = false;
    std::string leak_sentence;
    for (const auto& k : cons) {
        fan |= k.rule_id == "dead_fan" && k.sentence.find("GPU fan") != std::string::npos &&
               k.sentence.find("three hot events on two days") != std::string::npos;
        derr |= k.rule_id == "disk_errors" && k.sentence.find("Back up now") != std::string::npos;
        oom |= k.rule_id == "oom_kill" && k.sentence.find("killed blender") != std::string::npos;
        wait |= k.rule_id == "disk_wait" && k.sentence.find("points to updatedb") != std::string::npos;
        if (k.rule_id == "leak") { leak = true; leak_sentence = k.sentence; }
    }
    CHECK(fan && derr && oom && wait);
    CHECK(leak && leak_sentence.find("leaky") != std::string::npos &&
          leak_sentence.find("most consistent with a memory leak") != std::string::npos);
    // The why-path: the disk event's own conclusions.
    sqlite3_stmt* st2 = nullptr;
    sqlite3_prepare_v2(db, "SELECT id FROM events WHERE summary='disk saturated'", -1, &st2, nullptr);
    CHECK(sqlite3_step(st2) == SQLITE_ROW);
    int64_t wid = sqlite3_column_int64(st2, 0);
    sqlite3_finalize(st2);
    auto evc = diag.for_event(wid, int64_t(now));
    bool evwait = false;
    for (const auto& k : evc) evwait |= k.rule_id == "disk_wait";
    CHECK(evwait);
    sqlite3_close(db);

    // The question route end to end: the rule book answers the health check.
    as::Ask ask;
    CHECK(ask.open(dbp));
    ask.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                      "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
    as::AskResult w2 = ask.ask("what is wrong with this machine", int64_t(now));
    CHECK(!w2.refused && w2.answer.find("GPU fan") != std::string::npos);
}

void test_answer_parts() {
    std::puts("answer parts");
    // The parts in their order, single spaces, each part's own punctuation kept.
    as::AskResult r;
    r.notes = {"The records keep each process's peak, not its memory minute by minute."};
    r.body = "Blender ran twice in that window.";
    r.lead = "Yes.";
    r.scope = {"It is only 02:38, so this morning hasn't started yet. Yesterday's:", "Taking \"the update\" as Installed 6 packages (Wed 20:40):"};
    CHECK(as::compose_answer(r) ==
          "It is only 02:38, so this morning hasn't started yet. Yesterday's: Taking \"the update\" as Installed 6 packages (Wed 20:40): "
          "Yes. Blender ran twice in that window. The records keep each process's peak, not its memory minute by minute.");
    // An empty part leaves no gap, and no parts compose to nothing.
    as::AskResult e;
    CHECK(as::compose_answer(e).empty());
    e.scope = {""};
    e.body = "No crash was recorded.";
    CHECK(as::compose_answer(e) == "No crash was recorded.");
    e.notes = {"The device records hold USB devices."};
    CHECK(as::compose_answer(e) == "No crash was recorded. The device records hold USB devices.");
    // Undecided: the opening says the records can't settle the yes or no, after any lead and before the body.
    as::AskResult u;
    u.undecided = true;
    u.body = "Disk traffic reached 1.4 GB/s during four events.";
    CHECK(as::compose_answer(u) == "I can't say yes or no to that from the records. What they show: Disk traffic reached 1.4 GB/s during four events.");
    as::AskResult why = u;
    why.lead = "The fan isn't spinning.";
    CHECK(as::compose_answer(why) == "The fan isn't spinning. I can't say yes or no to that from the records. What they show: Disk traffic "
                                     "reached 1.4 GB/s during four events.");
    // A part that settles it supplies the lead in place of the opening, and the body's text stays as it was.
    u.lead = "No: at Thu 12:00 it was asleep, from Thu 11:07 to Thu 12:26. Around then:";
    u.undecided = false;
    CHECK(as::compose_answer(u) == "No: at Thu 12:00 it was asleep, from Thu 11:07 to Thu 12:26. Around then: Disk traffic reached 1.4 GB/s during four events.");
    CHECK(u.body == "Disk traffic reached 1.4 GB/s during four events.");
}

void test_askjudge() {
    std::puts("ask judge");
    Tree t;
    std::string dbp = t.root + "/j.db";
    std::tm tmv{};
    tmv.tm_year = 2026 - 1900; tmv.tm_mon = 9; tmv.tm_mday = 13; tmv.tm_hour = 21; tmv.tm_min = 30; tmv.tm_isdst = -1;
    const double now = double(std::mktime(&tmv));
    std::tm tm0 = tmv;
    tm0.tm_hour = tm0.tm_min = 0;
    tm0.tm_isdst = -1;
    const double today0 = double(std::mktime(&tm0));
    {
        as::Store st(dbp);
        auto raw = [&](double wall, double cpu, double temp) {
            as::Snapshot s;
            s.wall = wall;
            s.cpu.total = cpu;
            s.mem.total = 8e9;
            s.mem.available = 4e9;
            s.mem.used = 4e9;
            if (temp >= 0) s.cpu.temp_c = as::Reading<double>::of(temp);
            st.add_sample(s);
        };
        // Ten prior evenings at 21:15: cpu medians 40, 42, ... 58.
        for (int d = 1; d <= 10; ++d) raw(now - d * 86400 - 15 * 60, 40 + 2.0 * (10 - d), -1);
        // Today 10:00-17:59 the CPU ran at 60 C; yesterday's same hours at 50 C.
        for (int h = 10; h <= 17; ++h) {
            raw(today0 + h * 3600 + 600, 20, 60);
            raw(today0 - 86400 + h * 3600 + 600, 20, 50);
        }
        // The "now" side: three readings in the last ten minutes at 55%.
        for (int i = 0; i < 3; ++i) raw(now - 300 + i * 60, 55, -1);
        st.flush();
        st.set_meta("rollup_last_hour", std::to_string((int64_t((now - 11 * 86400) / 3600)) * 3600));
        st.rollup_hours(now);
    }
    as::Ask ask;
    CHECK(ask.open(dbp));
    ask.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                      "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
    as::AskResult n = ask.ask("is this cpu usage normal", int64_t(now));
    CHECK(n.answer.find("Yes: CPU use is 55%") == 0);
    CHECK(n.answer.find("80th percentile") != std::string::npos);
    CHECK(n.answer.find("Typical is 48%") != std::string::npos);
    as::AskResult c = ask.ask("was it hotter than yesterday", int64_t(now));
    CHECK(c.answer.find("Yes. CPU temperature runs clearly 10 C hotter") == 0);
    CHECK(c.answer.find("of eight overlapping hours") != std::string::npos);
    // The yes or no is the lead, the figures the body.
    CHECK(c.lead == "Yes." && c.body.find("CPU temperature runs clearly 10 C hotter") == 0 && c.answer == as::compose_answer(c));
    CHECK(n.lead == "Yes:" && n.body.find("CPU use is 55%") == 0);
}

void test_facts() {
    std::puts("facts");
    Tree t;
    std::string a = t.root + "/a.tsv", b = t.root + "/b.tsv";
    std::ofstream(a) << "kswapd0\tkernel\tfirst explanation\thand\nkworker/*\tkernel\ta kernel worker\thand\n";
    std::ofstream(b) << "kswapd0\tkernel\tsecond explanation\tuser\n";
    as::Facts f;
    f.load({a, b});
    CHECK(f.size() == 3);
    // The later file wins, and exact names beat globs.
    CHECK(f.lookup("kswapd0") && f.lookup("kswapd0")->explanation == "second explanation");
    CHECK(f.lookup("kworker/u64:3") && f.lookup("kworker/u64:3")->category == "kernel");
    CHECK(f.lookup("nothing") == nullptr);
}

void test_rollup() {
    std::puts("hourly rollup");
    Tree t;
    std::string db = t.root + "/r.db";
    const double HS = 1'699'999'200;  // an exact hour boundary
    {
        as::Store st(db);
        CHECK(st.ok());
        for (int i = 0; i < 12; ++i) {
            as::Snapshot s;
            s.wall = HS + 10 + i * 10;
            s.cpu.total = 10.0 * (i + 1);                       // 10..120
            s.mem.total = 8e9;
            s.mem.available = 4e9;
            s.mem.used = 4e9;
            if (i < 8) s.cpu.temp_c = as::Reading<double>::of(50);
            s.power.on_ac = as::Reading<bool>::of(i >= 9);      // 9 on battery, 3 plugged in
            st.add_sample(s);
        }
        st.flush();
        st.rollup_hours(HS + 3700);
        // Hand-computed, lower nearest rank: sorted cpu 10..120, idx(0.5*11)=5 -> 60,
        // idx(0.05*11)=0 -> 10, idx(0.95*11)=10 -> 110.
        auto cpu = st.hourly("cpu", HS - 1, HS + 3601);
        CHECK(cpu.size() == 1);
        CHECK(cpu[0].n == 12 && cpu[0].med == 60 && cpu[0].p05 == 10 && cpu[0].p95 == 110 && cpu[0].max == 120);
        CHECK(cpu[0].dropouts == 0);
        CHECK(cpu[0].on_battery_frac > 0.74 && cpu[0].on_battery_frac < 0.76);
        // Four samples had no temperature: counted, never invented.
        auto temp = st.hourly("temp", HS - 1, HS + 3601);
        CHECK(temp.size() == 1 && temp[0].n == 8 && temp[0].dropouts == 4 && temp[0].med == 50 && temp[0].max == 50);
        // Rolling again adds nothing.
        st.rollup_hours(HS + 3700);
        CHECK(st.hourly("cpu", 0, 2e9).size() == 1);
        // Raw samples die at 24 h; the rollup survives the prune.
        st.rollup_hours(HS + 30 * 3600);
        st.prune(HS + 30 * 3600, 24, 30);
        CHECK(st.samples(0, 2e9).empty());
        CHECK(st.hourly("cpu", 0, 2e9).size() == 1);
    }
}

// One number from the database, or NAN when the query fails or finds nothing.
double sql_number(const std::string& db, const std::string& sql) {
    sqlite3* h = nullptr;
    double v = NAN;
    if (sqlite3_open(db.c_str(), &h) == SQLITE_OK) {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(h, sql.c_str(), -1, &q, nullptr) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW &&
            sqlite3_column_type(q, 0) != SQLITE_NULL)
            v = sqlite3_column_double(q, 0);
        sqlite3_finalize(q);
    }
    sqlite3_close(h);
    return v;
}

// Swap size is kept with every sample, so "how full" can be answered; battery
// level and plug state survive in the hourly rollup once the seconds are
// pruned, so a battery question about last Sunday can be answered.
void test_swap_battery_history() {
    std::puts("swap size and battery history");
    Tree t;
    std::string db = t.root + "/b.db";
    const double HS = 1'699'999'200;  // an exact hour boundary
    {
        as::Store st(db);
        CHECK(st.ok());
        for (int i = 0; i < 60; ++i) {
            as::Snapshot s;
            s.wall = HS + 30 + 60 * i;
            s.mem.total = 8e9;
            s.mem.available = 4e9;
            s.mem.used = 4e9;
            s.mem.swap_total = 8'280'000'000;
            s.mem.swap_free = i < 30 ? 1'000'000'000 : 0;
            // Plugged in at 100%, unplugged for 50 minutes down to 18%, then charging.
            bool ac = i < 5 || i >= 55;
            double pct = i < 5 ? 100 : i < 55 ? 100 - (i - 5) * 82.0 / 49 : 18 + (i - 54) * 0.5;
            s.power.on_ac = as::Reading<bool>::of(ac);
            s.power.battery_pct = as::Reading<double>::of(pct);
            st.add_sample(s);
        }
        st.flush();
        st.rollup_hours(HS + 3700);
        st.rollup_hours(HS + 30 * 3600);
        st.prune(HS + 30 * 3600, 24, 30);
        auto batt = st.hourly("battery_pct", 0, 2e9);
        CHECK(batt.size() == 1 && near(batt[0].min, 18) && near(batt[0].max, 100));
    }
    // The raw seconds are pruned by now; the hour remains.
    CHECK(sql_number(db, "SELECT COUNT(*) FROM samples") == 0);
    CHECK(near(sql_number(db, "SELECT med FROM samples_hourly WHERE metric = 'swap_total'"), 8.28e9, 1));
    CHECK(near(sql_number(db, "SELECT max FROM samples_hourly WHERE metric = 'swap_used'"), 8.28e9, 1));
    CHECK(near(sql_number(db, "SELECT max FROM samples_hourly WHERE metric = 'battery_pct'"), 100));
    CHECK(near(sql_number(db, "SELECT min FROM samples_hourly WHERE metric = 'battery_pct'"), 18));
    CHECK(near(sql_number(db, "SELECT n FROM samples_hourly WHERE metric = 'battery_pct'"), 60));
    CHECK(near(sql_number(db, "SELECT min FROM samples_hourly WHERE metric = 'on_ac'"), 0));
    CHECK(near(sql_number(db, "SELECT max FROM samples_hourly WHERE metric = 'on_ac'"), 1));
    CHECK(near(sql_number(db, "SELECT on_battery_frac FROM samples_hourly WHERE metric = 'battery_pct'"), 50.0 / 60, 0.001));
}

void test_swap_column() {
    std::puts("swap size per sample");
    Tree t;
    std::string db = t.root + "/s.db";
    {
        sqlite3* old = nullptr;
        sqlite3_open(db.c_str(), &old);
        sqlite3_exec(old, "CREATE TABLE samples(t REAL PRIMARY KEY, cpu REAL, kernel REAL, mem_pct REAL, swap_used REAL, pressure REAL, "
                          "disk_r REAL, disk_w REAL, net_rx REAL, net_tx REAL, gpu REAL, gpu_mem REAL, temp REAL, fan REAL, power REAL, "
                          "freq REAL, procs INTEGER, cores TEXT); INSERT INTO samples(t, cpu, swap_used) VALUES(5, 1, 100)",
                     nullptr, nullptr, nullptr);
        sqlite3_close(old);
    }
    {
        as::Store st(db);
        as::Snapshot s;
        s.wall = 1000;
        s.mem.total = 8e9;
        s.mem.swap_total = 8'280'000'000;
        s.mem.swap_free = 0;
        st.add_sample(s);
        st.flush();
        auto rows = st.samples(0, 2000);
        CHECK(rows.size() == 2 && rows[0].swap_total == -1 && near(rows[1].swap_total, 8.28e9, 1));
    }
    CHECK(near(sql_number(db, "SELECT swap_total FROM samples WHERE t = 1000"), 8.28e9, 1));
    CHECK(near(sql_number(db, "SELECT swap_used FROM samples WHERE t = 1000"), 8.28e9, 1));
    CHECK(std::isnan(sql_number(db, "SELECT swap_total FROM samples WHERE t = 5")));  // unknown, never invented
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
    // D-Bus activated units hold a colon in their name; the name ends at ": ".
    auto dbus = j("systemd", "dbus-:1.2-org.kde.KSplash@2.service: Failed with result 'exit-code'.");
    CHECK(dbus && dbus->title == "dbus-:1.2-org.kde.KSplash@2.service failed: Failed with result 'exit-code'.");
    auto dumped = j("systemd", "dbus-:1.2-org.kde.kwalletd6@0.service: Main process exited, code=dumped, status=11/SEGV");
    CHECK(dumped && dumped->title == "dbus-:1.2-org.kde.kwalletd6@0.service failed: Main process exited, code=dumped, status=11/SEGV");

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
    // A read that lands inside a transaction (after a scriptlet's line) resumes where the transaction can be read
    // whole next time, its command with it: the kernel upgrade of Oct 11 10:42 was lost this way.
    {
        const std::string before = "[2026-10-11T10:30:00-0400] [PACMAN] Running 'pacman -S mixxx'\n"
                                   "[2026-10-11T10:30:01-0400] [ALPM] transaction started\n"
                                   "[2026-10-11T10:30:02-0400] [ALPM] installed mixxx (2.5.6-7)\n"
                                   "[2026-10-11T10:30:03-0400] [ALPM] transaction completed\n"
                                   "[2026-10-11T10:34:39-0400] [PACMAN] Running 'pacman -Syu'\n"
                                   "[2026-10-11T10:42:17-0400] [ALPM] transaction started\n"
                                   "[2026-10-11T10:43:45-0400] [ALPM] upgraded linux-cachyos (7.2.2-1 -> 7.2.9-2)\n"
                                   "[2026-10-11T10:43:50-0400] [ALPM-SCRIPTLET]   -> Building the initramfs\n";
        const std::string after = "[2026-10-11T10:44:10-0400] [ALPM] upgraded linux-firmware (1:20260810-2 -> 1:20260916-1)\n"
                                  "[2026-10-11T10:45:21-0400] [ALPM] transaction completed\n";
        std::istringstream first(before);
        long long resume = 0;
        const auto got1 = as::read_pacman(first, 0, &resume);
        CHECK(got1.size() == 1 && got1[0].title == "Installed mixxx");
        const std::string all = before + after;
        std::istringstream second(all.substr(std::size_t(std::min<long long>(resume, (long long)all.size()))));
        long long resume2 = 0;
        const auto got2 = as::read_pacman(second, resume, &resume2);
        CHECK(got2.size() == 1 && got2[0].title == "Upgraded linux-cachyos, linux-firmware" &&
              got2[0].detail.find("pacman -Syu") != std::string::npos);
        CHECK(resume2 == (long long)all.size());
    }

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
    // A command that merely QUOTES a flashing tool (a curl payload carrying
    // chat text about idf.py) is never a flash: the tool must be the command.
    CHECK(!as::recognise_tool("curl", "curl -4 -s http://127.0.0.1:11434/api/generate -d { \"prompt\": "
                                      "\"Flashing: esptool, idf.py flash took about 7 s, esptool.py write_flash\" }"));
    auto idf = as::recognise_tool("idf.py", "idf.py -p /dev/ttyUSB0 flash monitor");
    CHECK(idf && idf->category == "flashing" && idf->port == "/dev/ttyUSB0");
    auto idf2 = as::recognise_tool("python3", "python3 /opt/esp/idf.py flash");
    CHECK(idf2 && idf2->category == "flashing");
    auto modesp = as::recognise_tool("python3", "python3 -m esptool --port /dev/ttyUSB0 write_flash 0x0 fw.bin");
    CHECK(modesp && modesp->category == "flashing");
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
        // A row left open by a crash is closed with an estimated end; a row
        // matching a live process is left alone.
        as::ProcessRecord ghost{777, 4000, 0, "ghost", "", "", "", "", "", 1, 1, 0};
        as::ProcessRecord livep{888, 4500, 0, "alive", "", "", "", "", "", 1, 1, 0};
        st.add_procs({ghost, livep});
        CHECK(st.last_sample_time() == 0 || st.last_sample_time() > 0);  // callable either way
        CHECK(st.close_stale_procs({livep}, 5010) == 1);
        CHECK(st.close_stale_procs({livep}, 5010) == 0);  // already closed
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
    CHECK(sqlite3_column_int(q, 0) == 3 && near(sqlite3_column_double(q, 1), 5010));
    sqlite3_finalize(q);
    sqlite3_prepare_v2(db, "SELECT detail FROM v_timeline WHERE kind='program ended' AND title LIKE 'ghost%'", -1, &q, nullptr);
    CHECK(sqlite3_step(q) == SQLITE_ROW &&
          std::string(reinterpret_cast<const char*>(sqlite3_column_text(q, 0))).find("end estimated") != std::string::npos);
    sqlite3_finalize(q);
    sqlite3_close(db);
    CHECK((kinds == std::vector<std::string>{"program started", "first connection", "log: devices", "program ended", "program ended"}));
}

// The startup watcher goes by how long a file lived, never by its name. A
// file that comes and goes inside one quiet spell (an installer's or sed's
// temporary file) is not logged; a file replaced in place is one change.
void test_startup_lifetime() {
    std::puts("startup watcher: temporary and replaced files");
    Tree t;
    const std::string home = t.root + "/home";
    const std::string bin = home + "/.local/bin/";
    const std::string units = home + "/.config/systemd/user/";
    t.write("home/.local/share/claude/versions/2.1.308", "#!/bin/sh\necho old\n");
    t.write("home/.local/share/claude/versions/2.1.309", "#!/bin/sh\necho new\n");
    t.write("home/.local/bin/unllm", "#!/bin/sh\n");
    t.write("home/.config/systemd/user/ollama-warm.service", "[Service]\nExecStart=/usr/bin/curl old\n");
    CHECK(symlink((home + "/.local/share/claude/versions/2.1.308").c_str(), (bin + "claude").c_str()) == 0);
    as::StartupWatch w(home, t.root + "/etc");
    // Claude Code's self-update: the new link under a temporary name, renamed over the old one.
    CHECK(symlink((home + "/.local/share/claude/versions/2.1.309").c_str(), (bin + "claude.tmp.1497955.1792709056263").c_str()) == 0);
    std::rename((bin + "claude.tmp.1497955.1792709056263").c_str(), (bin + "claude").c_str());
    // sed -i: a temporary copy holding the new text, renamed over the unit.
    t.write("home/.config/systemd/user/sedS3jmuZ", "[Service]\nExecStart=/usr/bin/curl new\n");
    std::rename((units + "sedS3jmuZ").c_str(), (units + "ollama-warm.service").c_str());
    // A scratch file written and deleted, and a real new program.
    t.write("home/.local/bin/build-1234", "x");
    std::remove((bin + "build-1234").c_str());
    t.write("home/.local/bin/newtool", "#!/bin/sh\n");
    w.poll(100);
    auto entries = w.poll(103);
    std::vector<std::string> titles;
    for (const auto& e : entries) titles.push_back(e.title);
    auto has = [&](const std::string& s) { return std::find(titles.begin(), titles.end(), s) != titles.end(); };
    CHECK(titles.size() == 3);
    CHECK(has("Program in ~/.local/bin changed: " + bin + "claude"));
    CHECK(has("Your background service changed: " + units + "ollama-warm.service"));
    CHECK(has("Program in ~/.local/bin added: " + bin + "newtool"));
    for (const auto& e : entries)
        if (e.title.find("ollama-warm") != std::string::npos) CHECK(e.detail.find("+ ExecStart=/usr/bin/curl new") != std::string::npos);
    // A download into a temporary file, still being written after the quiet
    // spell, then renamed into place: one change, when it is done.
    {
        std::ofstream f(bin + "claude.tmp.77");
        f << "part one\n" << std::flush;
        w.poll(110);
        f << "part two\n" << std::flush;
        w.poll(111.5);
        f << "part three\n" << std::flush;
        CHECK(w.poll(113).empty());
    }
    std::rename((bin + "claude.tmp.77").c_str(), (bin + "claude").c_str());
    w.poll(114);
    entries = w.poll(117);
    CHECK(entries.size() == 1 && entries[0].title == "Program in ~/.local/bin changed: " + bin + "claude");
    // A program deleted for good.
    std::remove((bin + "newtool").c_str());
    w.poll(120);
    entries = w.poll(123);
    CHECK(entries.size() == 1 && entries[0].title == "Program in ~/.local/bin removed: " + bin + "newtool");
}

void test_peerwatch() {
    std::puts("background sharing");
    as::PeerWatch w;
    std::set<std::string> allow = {"syncthing"};
    w.allowed = [&](const std::string& p) { return allow.count(p) > 0; };
    auto conns = [](const std::string& prog, int n, int base, int port = 6881) {
        std::vector<as::ConnectionRecord> v;
        for (int i = 0; i < n; ++i) v.push_back({prog, "/usr/bin/" + prog, "10.0." + std::to_string(base) + "." + std::to_string(i), "TCP", port, 42, 0, 0, false});
        return v;
    };
    // A browser with three servers never alerts.
    CHECK(w.feed(conns("firefox", 3, 1), 0, 100).empty());
    // Twenty web servers on ordinary ports are browsing, not sharing.
    CHECK(w.feed(conns("firefox", 20, 9, 443), 0, 100).empty());
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

void test_askcore() {
    std::puts("ask");
    namespace fs = std::filesystem;
    std::string dir = fs::temp_directory_path() / ("as-ask-" + std::to_string(getpid()));
    fs::create_directories(dir);
    std::string dbp = dir + "/h.db";
    std::tm tmv{};
    tmv.tm_year = 2026 - 1900; tmv.tm_mon = 9; tmv.tm_mday = 13; tmv.tm_hour = 21; tmv.tm_min = 30; tmv.tm_isdst = -1;
    const double now = double(std::mktime(&tmv));  // a real local evening, so "tonight" holds the fixture
    {
        as::Store st(dbp);
        as::Event e;
        e.start = now - 2 * 3600;
        e.end = now - 2 * 3600 + 300;
        e.kinds = as::KindCpu | as::KindHeat;
        e.peak_temp = 96;
        e.peak_cpu = 61;
        e.min_margin = 4;
        e.summary = "CPU peaked at 61%, the CPU reached 96 C, mostly blender";
        as::Culprit c;
        c.name = "blender";
        c.share = 0.74;
        e.culprits.push_back(c);
        st.add_event(e);
        st.add_log({{now - 3600, "problems", "mixxx crashed", ""}});
        st.add_log({{now - 4 * 3600, "devices", "Plugged in: USB drive SanDisk", ""}});
        st.add_log({{now - 3 * 86400, "flashing", "esptool flashed esp32 firmware", ""}});
        st.add_log({{now - 3 * 86400 + 100, "software", "upgraded 281 packages", ""}});
        as::ProcessRecord p{10, now - 9000, now - 8000, "blender", "", "", "", "", "", 1, 50, 0};
        st.add_procs({p});
        // Firefox ran days ago: known to the gazetteer, absent today.
        as::ProcessRecord ff{11, now - 5 * 86400, now - 5 * 86400 + 600, "firefox", "", "", "", "", "", 1, 20, 0};
        st.add_procs({ff});
        // A question about chrome handed to another command: chrome itself never ran.
        as::ProcessRecord qa{12, now - 7000, now - 6990, "ask", "/usr/local/bin/ask", "/usr/local/bin/ask --branch chrome safe to run?",
                             "", "", "", 1, 0.5, 0};
        st.add_procs({qa});
        // A heat event whose margin was never read (-1): it must never
        // surface as "within -1 C of the shutdown point".
        as::Event m;
        m.start = now - 3 * 3600;
        m.end = now - 3 * 3600 + 60;
        m.kinds = as::KindHeat;
        m.peak_temp = 80;
        m.min_margin = -1;
        m.summary = "the CPU reached 80 C";
        st.add_event(m);
        st.flush();
    }
    as::Ask ask;
    CHECK(ask.open(dbp));
    ask.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                      "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
    ask.set_miss_log(dir + "/misses.log");

    as::AskResult heat = ask.ask("why was it so hot tonight", int64_t(now));
    CHECK(!heat.refused);
    CHECK(heat.answer.find("The CPU reached 96 C") != std::string::npos);
    CHECK(heat.answer.find("Blender was the main cause") != std::string::npos);
    CHECK(heat.answer.find("shutdown point") != std::string::npos);
    CHECK(!heat.receipts.empty());

    as::AskResult crash = ask.ask("what crashed today", int64_t(now));
    CHECK(crash.answer.find("One crash was recorded") != std::string::npos);
    CHECK(crash.receipts.size() == 1 && crash.receipts[0].find("mixxx crashed") != std::string::npos);

    as::AskResult none = ask.ask("did anything crash last week", int64_t(now));
    CHECK(none.answer.find("No crashes were recorded") != std::string::npos && none.receipts.empty());

    // Half understood: the refusal becomes a clarifying question, and the
    // miss lands in the local log.
    as::AskResult steak = ask.ask("how do i cook a steak", int64_t(now));
    CHECK(steak.refused && steak.answer.find("Are you asking about heat") != std::string::npos);
    {
        std::ifstream mf(dir + "/misses.log");
        std::string line;
        CHECK(bool(std::getline(mf, line)) && line.find("steak") != std::string::npos);
    }

    as::AskResult story = ask.ask("what did blender do tonight", int64_t(now));
    CHECK(!story.refused && !story.receipts.empty());

    // A polar question about a program gets its yes with the evidence.
    as::AskResult ran = ask.ask("did blender run today", int64_t(now));
    CHECK(ran.answer.find("Yes, Blender ran once") != std::string::npos);
    as::AskResult norun = ask.ask("did ollama run today", int64_t(now));
    CHECK(norun.refused || norun.answer.find("No") == 0);  // unknown program: refused or a clean no

    // Fragments and pronouns ride the conversation state.
    ask.ask("what did blender do tonight", int64_t(now));
    as::AskResult times = ask.ask("how many times?", int64_t(now));
    CHECK(times.answer.find("Blender ran once") != std::string::npos);
    ask.ask("what did blender do tonight", int64_t(now));
    as::AskResult itq = ask.ask("did it crash", int64_t(now));
    CHECK(itq.answer.find("No") == 0);
    // A chatty coat changes nothing: same answer, word for word.
    as::AskResult coat = ask.ask("could you tell me what crashed today", int64_t(now));
    CHECK(coat.answer == crash.answer);
    // "help" is never captured by the conversation state.
    as::AskResult help2 = ask.ask("help", int64_t(now));
    CHECK(!help2.refused && help2.answer.find("Ask about") == 0);
    // "how long" questions get a duration.
    as::AskResult dur = ask.ask("how long was blender running tonight", int64_t(now));
    CHECK(dur.answer.find("Blender ran for") == 0);

    // What things are: the fact table joined with the measured history.
    ask.load_facts({"facts/base.tsv"});
    as::AskResult wib = ask.ask("what is blender", int64_t(now));
    CHECK(wib.answer.find("Blender is the 3D modelling") == 0);
    CHECK(wib.answer.find("It has run once") != std::string::npos);
    as::AskResult wik = ask.ask("what is kswapd0", int64_t(now));
    CHECK(wik.answer.find("Kswapd0 is the kernel's memory reclaim thread") == 0);
    CHECK(wik.answer.find("not recorded in the program history") != std::string::npos);
    as::AskResult wif = ask.ask("what is flurbinator", int64_t(now));
    CHECK(!wif.refused && wif.answer.find("I don't know what flurbinator is") == 0);

    // Counting and superlatives.
    as::AskResult cnt = ask.ask("how many crashes today", int64_t(now));
    CHECK(cnt.answer.find("One crash was recorded") != std::string::npos);
    as::AskResult most = ask.ask("which program used the most memory today", int64_t(now));
    CHECK(most.answer.find("Blender used the most memory") != std::string::npos);

    // The classics get the fixed palatable reply, never a cold refusal.
    as::AskResult joke = ask.ask("tell me a joke", int64_t(now));
    CHECK(!joke.refused && !joke.answer.empty() && joke.answer.find("can't answer") == std::string::npos);
    as::AskResult life = ask.ask("what is the meaning of life", int64_t(now));
    CHECK(!life.refused && life.answer.find("42") != std::string::npos);

    // Identity facts read from fixture roots.
    {
        fs::create_directories(dir + "/sys/class/dmi/id");
        fs::create_directories(dir + "/etc");
        fs::create_directories(dir + "/proc");
        std::ofstream(dir + "/sys/class/dmi/id/sys_vendor") << "TestVendor\n";
        std::ofstream(dir + "/sys/class/dmi/id/product_name") << "TestBook 9\n";
        std::ofstream(dir + "/etc/os-release") << "NAME=Test\nPRETTY_NAME=\"Test Linux\"\n";
        std::ofstream(dir + "/proc/uptime") << "90000.5 1234.0\n";
        as::SystemId id = as::system_identity({dir + "/proc", dir + "/sys", dir + "/etc"});
        CHECK(id.vendor == "TestVendor" && id.product == "TestBook 9");
        CHECK(id.os_pretty == "Test Linux");
        CHECK(id.uptime_seconds == 90000);
        CHECK(!id.kernel.empty() && !id.hostname.empty());  // live uname, any value
    }

    // A story question with no searchable words tells everything in the
    // window through the timeline, resource events first.
    as::AskResult today = ask.ask("what happened today", int64_t(now));
    CHECK(!today.refused && today.answer.find("In that window: two heat events") == 0);
    CHECK(today.answer.find("records cover") == std::string::npos);
    CHECK(!today.receipts.empty() && today.receipts[0].find("CPU peaked") != std::string::npos);

    // Device questions reach the devices log category.
    as::AskResult usb = ask.ask("what usb devices did i plug in today", int64_t(now));
    CHECK(usb.answer.find("device event") != std::string::npos);
    CHECK(!usb.receipts.empty() && usb.receipts[0].find("USB drive") != std::string::npos);
    // None in the window: what the device records hold is a note after the count.
    as::AskResult usb0 = ask.ask("what usb devices did i plug in yesterday", int64_t(now));
    CHECK(usb0.notes.size() == 1 && usb0.notes[0].rfind("The device records hold", 0) == 0 && usb0.answer == as::compose_answer(usb0) &&
          usb0.answer.find(usb0.body + " The device records hold") == 0);

    // "When did i ..." reads the whole history and names the moment, even
    // when the record is older than the default day.
    as::AskResult fl = ask.ask("when did i flash the esp32", int64_t(now));
    CHECK(fl.answer.find("The last flashing event was") == 0 && fl.answer.find("esptool") != std::string::npos);
    as::AskResult lastup = ask.ask("when did i last update the system", int64_t(now));
    CHECK(lastup.answer.find("The last software change was") == 0 && lastup.answer.find("281 packages") != std::string::npos);
    as::AskResult lastran = ask.ask("when did blender last run", int64_t(now));
    CHECK(lastran.answer.find("Blender ran last on") == 0);

    // "Why was the disk busy" is a disk question, never a CPU answer.
    as::AskResult disk = ask.ask("why was the disk busy this afternoon", int64_t(now));
    CHECK(disk.answer.find("isk traffic") != std::string::npos);

    // Twin rows born of boot-time drift collapse into one process.
    {
        std::string dbp2 = dir + "/twin.db";
        {
            as::Store st2(dbp2);
            as::ProcessRecord a{77, now - 5000, 0, "blender", "", "", "", "", "", 1, 0.5, 10};
            st2.add_procs({a});
            as::ProcessRecord b{77, now - 4999, now - 100, "blender", "", "", "", "", "", 1, 30, 20};
            st2.add_procs({b});
            st2.flush();
        }
        as::Ask ask2;
        CHECK(ask2.open(dbp2));
        as::AskResult once = ask2.ask("did blender run today", int64_t(now));
        CHECK(once.answer.find("Yes, Blender ran once") != std::string::npos);
    }
    {
        std::string dbp3 = dir + "/slept.db";
        {
            as::Store st3(dbp3);
            as::Event e;
            e.start = now - 2 * 3600 - 840;
            e.end = now - 2 * 3600 - 660;
            e.kinds = as::KindHeat;
            e.peak_temp = 90;
            e.summary = "the CPU reached 90 C";
            st3.add_event(e);
            st3.add_log({{now - 2 * 3600 - 300, "session", "Went to sleep", ""}});
            st3.add_log({{now - 2 * 3600 + 1200, "session", "Woke from sleep", ""}});
            st3.flush();
        }
        as::Ask ask3;
        CHECK(ask3.open(dbp3));
        ask3.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                           "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
        // A moment the machine slept through (19:25 to 19:50) in a window whose records can't settle the yes or no: the
        // moment supplies the lead in place of the can't-say opening, and the body is as the handler left it.
        as::AskResult und = ask3.ask("did the heat act up at 7:30 tonight", int64_t(now));
        CHECK(und.lead == "No: at Tue 19:30 it was asleep, from Tue 19:25 to Tue 19:50. Around then:" && !und.undecided && und.scope.empty() &&
              und.body.find("The CPU reached 90 C during one event.") == 0 && und.answer == und.lead + " " + und.body);
        // One the records settle keeps its own yes, and the moment is said before it as how the question was taken.
        as::AskResult yes = ask3.ask("was there trouble with the heat at 7:30 tonight", int64_t(now));
        CHECK(yes.lead.empty() && yes.scope.size() == 1 && yes.scope[0] == "At Tue 19:30 it was asleep, from Tue 19:25 to Tue 19:50. Around then:" &&
              yes.answer == as::compose_answer(yes) && yes.answer.find("Around then: Yes, the CPU reached 90 C") != std::string::npos);
    }

    // The full identity answer and a live question, against the real machine:
    // only shape is asserted, the numbers are whatever today is.
    as::AskResult whoami = ask.ask("what is this machine", int64_t(now));
    CHECK(!whoami.refused && whoami.answer.find("This is") == 0 && !whoami.receipts.empty());
    as::AskResult hot = ask.ask("how hot is it right now", int64_t(now));
    CHECK(!hot.refused && hot.answer.find("right now") != std::string::npos);

    // A program the facts know but the records never saw run is said so, though its name was once an argument to
    // another command (T27, T46: "ask --branch chrome safe to run?" ran here, chrome did not).
    as::AskResult chrome = ask.ask("chrome safe to run?", int64_t(now));
    CHECK(chrome.branch == "never-ran:chrome" && chrome.answer.find("hasn't run on this machine") != std::string::npos);
    CHECK(ask.ask("did chrome phone home overnight?", int64_t(now)).branch == "never-ran:chrome");
    // Asked whether to trust it, the answer offers to check a downloaded copy by its path (T66).
    as::AskResult launcher = ask.ask("can i trust the steam launcher i downloaded", int64_t(now));
    CHECK(launcher.branch == "never-ran:steam" && launcher.answer.find("path") != std::string::npos);
    CHECK(chrome.answer.find("path") != std::string::npos);
    // Its tags still say how the question was read, as on every other answer.
    CHECK(launcher.topic == "trust" && !launcher.span.empty() && !chrome.topic.empty());

    // ---- conversation: follow-ups, corrections, clarify replies ----
    {
        as::Ask c;
        CHECK(c.open(dbp));
        c.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                        "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
        c.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        c.load_facts({"facts/base.tsv"});
        auto at = [&](const char* q) { return c.ask(q, int64_t(now)); };
        // A follow-up with nothing before it.
        CHECK(at("why?").answer.find("Why what? Ask me about the machine first") == 0);
        // "why?" points at the answer just given.
        at("why was it so hot tonight");
        as::AskResult why = at("why?");
        CHECK(why.branch == "followup:why" && why.answer.find("lender") != std::string::npos);
        // "when was that" names the moment.
        at("what crashed today");
        as::AskResult when = at("when was that");
        CHECK(when.branch == "followup:when" && when.answer.find("That was ") == 0);
        // "are you sure" shows the records.
        at("what crashed today");
        as::AskResult sure = at("are you sure");
        CHECK(sure.branch == "followup:evidence" && !sure.receipts.empty());
        // Corrections swap one thing into the last question.
        at("did blender run today");
        as::AskResult fix = at("no i meant firefox");
        CHECK(fix.answer.find("No, Firefox did not run in that window.") == 0);
        // Chat-speak and a typo, with the correction shown.
        as::AskResult typo = at("wat crashd 2day");
        CHECK(typo.answer.find("One crash was recorded") != std::string::npos);
        bool shown = false;
        for (const auto& rc : typo.receipts) shown |= rc == "spelling: crashd -> crash";
        CHECK(shown);
        // A clarifying question understands its "yes".
        CHECK(at("how do i cook a steak").answer.find("Are you asking about heat") != std::string::npos);
        as::AskResult yes = at("yes");
        // A present-tense question with no time named reads as now since 2026-10-22, so the
        // confirmed topic answers live; a past-tense one would answer from the records.
        CHECK(!yes.refused && (yes.branch.rfind("live:heat", 0) == 0 || yes.branch.rfind("mask:heat", 0) == 0));
        // Never a margin of -1.
        CHECK(at("how hot did it get today").answer.find("-1 C") == std::string::npos);
        // "This morning" asked before it began.
        std::tm early = tmv;
        early.tm_hour = 2;
        early.tm_min = 38;
        early.tm_isdst = -1;
        as::AskResult morning = c.ask("what happened this morning", int64_t(std::mktime(&early)));
        CHECK(morning.answer.find("It is only 02:38, so this morning hasn't started yet. Yesterday's: ") == 0);
        // How the question was taken is the scope, before the body it leaves untouched.
        CHECK(morning.scope.size() == 1 && morning.scope[0] == "It is only 02:38, so this morning hasn't started yet. Yesterday's:" &&
              morning.answer == morning.scope[0] + " " + morning.body);
        // Summaries are paragraphs.
        as::AskResult sum = at("summarize today");
        CHECK(sum.answer.find("In that window:") == 0 && sum.answer.find("heat event") != std::string::npos);
        // Either/or, conditions, worries, advice, the GPU.
        CHECK(at("is blender or firefox worse for heat").answer.find("Blender weighed more for heat") == 0);
        CHECK(at("how hot does it get when blender is running").answer.find("while blender was running") !=
              std::string::npos);
        CHECK(at("should i be worried about the temperature").answer.find("No: nothing in the records") == 0);
        CHECK(at("what should i do about the heat").answer.find("blender did most of that work") != std::string::npos);
        CHECK(!at("how is the gpu doing").refused);
        // The live set keeps its branches.
        CHECK(at("how hot is it right now").branch == "live:heat");
        CHECK(at("am i on battery").branch == "live:battery");
        CHECK(at("what is using the cpu now").branch == "live:slow");
        CHECK(at("what is this machine").branch == "identity");
    }

    // ---- off the machine: the standard refusal, never a guess ----
    {
        as::Ask h;
        CHECK(h.open(dbp));
        h.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                        "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
        h.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        h.load_facts({"facts/base.tsv"});
        auto at = [&](const char* q) { return h.ask(q, int64_t(now)); };
        for (const char* q : {"what is the weather today", "write me a poem about the sea", "what is 2 plus 2"}) {
            as::AskResult r = at(q);
            CHECK(r.refused && (r.branch == "refuse" || r.branch == "clarify"));
        }
        CHECK(at("what is photosynthesis").answer.find("I don't know what photosynthesis is") == 0);
        // The machine answers exactly as before.
        CHECK(at("how hot is it right now").branch == "live:heat");
        CHECK(at("what is kswapd0").answer.find("Kswapd0 is") == 0);
    }

    fs::remove_all(dir);
}

// The question catalogue (data/kinds) and its matcher: each phrasing filed
// under the kind whose description answers it, and the traced questions of the
// 50-question test landing on a kind that answers them.
void test_kinds() {
    std::puts("kinds");
    std::map<std::string, std::string> filed;  // phrasing -> its kind, as shipped
    {
        std::ifstream pf("data/kinds/phrasings.tsv");
        for (std::string line; std::getline(pf, line);)
            if (auto tab = line.find('\t'); tab != std::string::npos) filed[line.substr(tab + 1)] = line.substr(0, tab);
    }
    // The Wi-Fi link speed is recorded; a list of failures is not a count; crashing and rebooting is an unclean stop.
    CHECK(filed["how fast is my wifi rn"] == "network.wifi.value.now");
    CHECK(filed["service failures since yesterday"] == "startup.which.window");
    CHECK(filed["how many times has my laptop crashed and rebooted this week"] == "crashes.yesno.window.system");

    const char* em = std::getenv("ASK_EMBED_MODEL");
    const char* home = std::getenv("HOME");
    const std::string model = em ? em : std::string(home ? home : "") + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: matcher checks skipped");
        return;
    }
    // The phrasing vectors are kept beside the test binary: only a changed phrasings.tsv pays for them again.
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string bin = n > 0 ? std::string(exe, std::size_t(n)) : std::string("./ascore_tests");
    const std::string cache = bin.substr(0, bin.rfind('/')) + "/kinds-cache";
    std::filesystem::create_directories(cache);
    as::Kinds kinds;
    CHECK(kinds.load("data/kinds", cache, model));
    if (!kinds.ready()) return;
    auto lands = [&](const char* q, std::set<std::string> right) {
        const std::string got = kinds.match(q).id;
        if (!right.count(got)) std::fprintf(stderr, "  \"%s\" -> %s\n", q, got.c_str());
        return right.count(got) > 0;
    };
    // Traced questions whose way of asking had no phrasing; the right kinds are the blind labellers' picks.
    CHECK(lands("is that fan control thing running as root legit", {"trust.identity"}));
    CHECK(lands("how much memory was blender using when it got killed the other week", {"memory.oom.list.window"}));
    CHECK(lands("when did i set up the cpu power cap and what does it actually do", {"config.when.window"}));
    CHECK(lands("when did i put cuda on here and what version is it", {"updates.identity.package"}));
    CHECK(lands("what got killed at 10 past 10 yesterday morning", {"memory.oom.list.window"}));
    CHECK(lands("what services failed yesterday", {"startup.which.window"}));
    CHECK(lands("how many times has the laptop cut out by itself this month", {"crashes.yesno.window.system"}));
    // Sleep and wake, and how long a program was open, have kinds of their own.
    CHECK(lands("when did the laptop wake up on wednesday", {"battery.sleep.when.window"}));
    CHECK(lands("did the laptop go to sleep while i was out at lunch yesterday or did it stay on the whole time",
                {"battery.sleep.list.window", "battery.sleep.when.window"}));
    CHECK(lands("how long was the movie player open last night", {"story.often.window.process"}));
    CHECK(lands("how long did i leave that mp3 open in haruna on tuesday", {"story.often.window.process"}));
    // How much the drive filled since a day is free space over time, not disk traffic.
    CHECK(lands("how much has my drive filled up since last friday", {"disk.space.trend.window"}));
    // What the session log, the admin log, the first runs and the connections hold have kinds of their own; so do
    // the fan's loudness, the machine at a moment and a count of running programs. The new 50's questions, then
    // other wordings of each.
    CHECK(lands("how loud did the fan get monday evening", {"heat.fan.value.window"}));
    CHECK(lands("how noisy did the fan get on wednesday night", {"heat.fan.value.window"}));
    CHECK(lands("when did i last plug the charger in", {"battery.charger.when.window"}));
    CHECK(lands("what time did i plug the laptop in today", {"battery.charger.when.window"}));
    CHECK(lands("how many times did i unplug the charger this week", {"battery.charger.list.window"}));
    CHECK(lands("what did i run with sudo this week", {"trust.admin.list.window"}));
    CHECK(lands("which commands did i run as root since monday", {"trust.admin.list.window"}));
    CHECK(lands("how many times did i close the lid this week", {"battery.lid.list.window", "battery.lid.when.window"}));
    CHECK(lands("how often did i shut the lid yesterday", {"battery.lid.list.window", "battery.lid.when.window"}));
    CHECK(lands("which apps connected to the internet for the first time today", {"network.new.list.window"}));
    CHECK(lands("did any program connect out for the first time this week", {"network.new.list.window"}));
    CHECK(lands("which programs ran for the first time yesterday", {"trust.list.window"}));
    CHECK(lands("when did i last log in", {"startup.login.when.window"}));
    CHECK(lands("how many times did i log in this week", {"startup.login.list.window", "startup.login.when.window"}));
    CHECK(lands("what was the laptop doing at 3am last night", {"diagnose.list.window"}));
    CHECK(lands("what was my computer doing at 2am", {"diagnose.list.window"}));
    CHECK(lands("how many programs were running at lunch time today", {"slow.procs.value.window"}));
    CHECK(lands("how many processes are running right now", {"slow.procs.value.window"}));
    // What the records cannot hold: browser tabs and pages, what was typed or on screen, traffic per website.
    CHECK(lands("how many tabs have i got open in firefox", {"story.tabs.none"}));
    CHECK(lands("which websites did i visit yesterday", {"story.tabs.none"}));
    CHECK(lands("what was i typing when the laptop shut off on sunday", {"story.typing.none"}));
    CHECK(lands("what was on the screen when the laptop died", {"story.typing.none"}));
    CHECK(lands("which website used the most bandwidth yesterday", {"network.site.none"}));
    CHECK(lands("how much data did youtube use today", {"network.site.none"}));
    // Their neighbours keep their own kinds.
    CHECK(lands("is the charger plugged in right now", {"battery.yesno.now"}));
    CHECK(lands("when did i plug the charger back in yesterday morning and how low did the battery get", {"battery.value.window"}));
    CHECK(lands("when did i last reboot", {"startup.when.reboot"}));
    CHECK(lands("what was keeping the laptop busy at 3am on tuesday", {"slow.which.window"}));
    CHECK(lands("wat was the laptop up to the other night before i shut the lid", {"diagnose.list.window"}));
    CHECK(lands("how many times has the laptop cut out by itself this month", {"crashes.yesno.window.system"}));
    // The traced 95's questions the matcher sent to another kind, and a second wording of each shape.
    CHECK(lands("firmware update history check", {"flash.list.window"}));
    CHECK(lands("show past bios and firmware updates", {"flash.list.window"}));
    CHECK(lands("did my laptop throttle at 3pm when i had the zoom call", {"heat.value.window"}));
    CHECK(lands("was the cpu throttling during the meeting at 11am", {"heat.value.window"}));
    CHECK(lands("services started at boot", {"startup.list.now"}));
    CHECK(lands("daemons started at boot", {"startup.list.now"}));
    CHECK(lands("bandwidth usage today", {"network.value.window"}));
    CHECK(lands("how much bandwidth did i use this week", {"network.value.window"}));
    CHECK(lands("disk writes during update", {"disk.value.window"}));
    CHECK(lands("disk io during the package upgrade", {"disk.value.window"}));
    CHECK(lands("what does the arduino cli thing running do", {"trust.identity"}));
    CHECK(lands("what does the esptool thing running do", {"trust.identity"}));
    CHECK(lands("why slow this morning", {"slow.cause.window"}));
    CHECK(lands("why so slow this morning", {"slow.cause.window"}));
    CHECK(lands("whats draining batt while lid closed last night", {"battery.value.window"}));
    CHECK(lands("how much battery went while the laptop was shut last night", {"battery.value.window"}));
    CHECK(lands("did something mine crypto on my laptop today", {"slow.which.window"}));
    CHECK(lands("was anything cryptomining on this machine last night", {"slow.which.window"}));
    CHECK(lands("net out while downloading", {"network.wifi.when.window"}));
    CHECK(lands("did the connection drop during my download yesterday", {"network.wifi.when.window"}));
    CHECK(lands("did a bluetooth device connect last night", {"devices.list.window"}));
    CHECK(lands("did my bluetooth speaker connect this morning", {"devices.list.window"}));
    CHECK(lands("how long was the cpu above 80% today", {"slow.above.value.window"}));
    CHECK(lands("how long was the cpu above 90 percent yesterday", {"slow.above.value.window"}));
    // What the records cannot hold: BIOS settings, monitors and screens, reminders (ask answers, it doesn't act).
    CHECK(lands("bios settings changed", {"identity.bios.settings.none"}));
    CHECK(lands("did someone mess with the uefi settings", {"identity.bios.settings.none"}));
    CHECK(lands("was the external monitor connected while i was gaming", {"devices.monitor.none"}));
    CHECK(lands("was the hdmi screen plugged in while i watched the film", {"devices.monitor.none"}));
    CHECK(lands("can you remind me tomorrow to clean the fans", {"none.reminder"}));
    CHECK(lands("could you remind me next week to update the system", {"none.reminder"}));
    // Their neighbours keep their own kinds.
    CHECK(lands("which services started since this morning", {"startup.list.window"}));
    CHECK(lands("which apps used the most bandwidth today", {"network.which.window"}));
    CHECK(lands("is it safe to update the bios now", {"identity.bios.flash"}));
    CHECK(lands("is bluetooth connected right now", {"devices.bluetooth.now"}));
    CHECK(lands("did anything get killed for memory last night", {"memory.oom.list.window"}));
    CHECK(lands("when did i last close the lid", {"battery.lid.when.window"}));
    CHECK(lands("how high did the cpu load go earlier today", {"slow.value.window"}));
    // The traced 95: whether someone used the machine is a kind of its own; a device that dropped out, and why, is
    // the device log's. The traced question, then other wordings.
    CHECK(lands("did my roommate use my laptop while i was out", {"story.used.window"}));
    CHECK(lands("did my brother go on my computer while i was at school", {"story.used.window"}));
    CHECK(lands("has someone been using my laptop today", {"story.used.window"}));
    CHECK(lands("my mouse disconnected at 3pm, why", {"devices.when.window"}));
    CHECK(lands("why did my headphones cut out at 9pm", {"devices.when.window"}));
    // Their neighbours keep their own kinds.
    CHECK(lands("what went on with my laptop while i was out", {"diagnose.list.window"}));
    CHECK(lands("did the computer reboot itself at 4pm", {"startup.often.reboot", "startup.when.reboot", "crashes.yesno.window.system"}));
}

// Everyday words to programs (names.h), against rows copied from this laptop's records on 2026-10-23, the unit
// descriptions systemd gave for them, their desktop entries and their harvested facts.
void test_names() {
    std::puts("program names");
    namespace fs = std::filesystem;
    const std::string dir = fs::temp_directory_path() / ("as-names-" + std::to_string(getpid()));
    fs::create_directories(dir + "/apps");
    fs::create_directories(dir + "/kinds");
    const std::string dbp = dir + "/h.db";
    { as::Store st(dbp); }  // the schema
    {
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        const char* rows =
            "INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES "
            "(1693063, 1792705816.46, 1792705862.47, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 8.2, 427720704),"
            "(1694398, 1792705906.39, NULL, 'plasmashell', '/usr/bin/plasmashell', '/usr/bin/plasmashell --no-respawn', 'user', 'plasma-plasmashell.service', 774, 'systemd', 396.9, 450064384),"
            "(1843640, 1792719238.67, 1792724191.98, 'ktorrent', '/usr/bin/ktorrent', '/usr/bin/ktorrent magnet:?xt=urn:btih:0000', 'user', 'app-firefox@fc022eb11b464f22a3cd3a4e2aea0542.service', 774, 'systemd', 58.1, 165228544),"
            "(1846301, 1792719437.55, 1792724185.98, 'haruna', '/usr/bin/haruna', '/usr/bin/haruna /home/user/Videos/film.mp4', 'user', 'app-org.kde.haruna@140956f3bdac41a08a5384edfc500dbf.service', 774, 'systemd', 441.3, 458625024),"
            "(885, 1791327278.24, NULL, 'fan-control.sh', '', '/bin/bash /usr/local/sbin/fan-control.sh', 'root', 'fan-control.service', 1, 'systemd', 261.1, 2011136),"
            "(1935812, 1792728057.68, NULL, 'sleep', '', 'sleep 1', 'root', 'fan-control.service', 885, 'fan-control.sh', 0, 2658304),"
            "(1005431, 1792617645.43, NULL, 'node-MainThread', '/usr/bin/node', 'node realm-companion.js', 'user', 'app-realm\\x2dos@0d4a935caa294f50b76112e83ca0b973.service', 774, 'systemd', 227.1, 104009728),"
            "(1569587, 1792695120.38, 1792700404.07, 'node-MainThread', '/usr/bin/node', 'node /home/user/.npm/_npx/0000/node_modules/.bin/mcp-server --preset mail', 'user', 'vte-spawn-f973063d-30da-459a-8ae9-95b827b50676.scope', 1569561, 'npm exec @softe', 1.9, 195510272),"
            "(1032663, 1792045628.93, 1792045653.12, 'node', '/home/user/project/.venv/lib/python3.14/site-packages/playwright/driver/node', '/home/user/project/.venv/lib/python3.14/site-packages/playwright/driver/node /home/user/project/.venv/lib/python3.14/site-packages/playwright/driver/package/cli.js run-driver', 'user', 'project-scan.service', 1029327, 'python', 1.71, 173002752),"
            "(1524, 1792464441.4, 1792688836.09, 'kwin_wayland_wrapper', '/usr/bin/kwin_wayland_wrapper', '/usr/bin/kwin_wayland_wrapper --xwayland', 'user', 'plasma-kwin_wayland.service', 774, 'systemd', 0, 60915712),"
            "(1497050, 1792688848.34, NULL, 'kwin_wayland', '', '/usr/bin/kwin_wayland --wayland-fd 7 --socket wayland-0 --xwayland-fd 8 --xwayland-fd 9 --xwayland-display :0', 'user', 'plasma-kwin_wayland.service', 1497045, 'kwin_wayland_wrapper', 904.0, 258220032),"
            "(1854838, 1792720074.8, 1792720078.98, 'python3', '/usr/bin/python3.14', 'python3 /home/user/.local/bin/weird --offline --no-prompt /usr/local/bin/ollama', 'user', 'run-p1851251-i1861925.scope', 1854837, 'sh', 1.0, 138993664),"
            "(2194, 1792464447.6, 1792688836.09, 'gnome-terminal-server', '/usr/lib/gnome-terminal-server', '/usr/lib/gnome-terminal-server', 'user', 'gnome-terminal-server.service', 774, 'systemd', 1313.6, 61640704),"
            "(1464270, 1792686390.93, 1792686394.98, 'gnome-terminal', '/usr/bin/gnome-terminal', '/usr/bin/gnome-terminal', 'user', 'app-org.gnome.Terminal@9bc057645dc04058991dbf00e94e7c29.service', 774, 'systemd', 0.1, 31907840),"
            "(1168789, 1792628639.07, 1792678309.71, 'fish', '/usr/bin/fish', 'fish', 'user', 'vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope', 2194, 'gnome-terminal-server', 0.2, 10997760),"
            "(1168842, 1792628641.42, 1792678309.71, 'claude', '/home/user/.local/share/claude/versions/2.1.307', 'claude --resume', 'user', 'vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope', 1168789, 'fish', 597.0, 730000000),"
            "(1, 1792464425.11, NULL, 'systemd', '', '/usr/lib/systemd/systemd --switched-root --system --deserialize=57 splash', 'root', 'init.scope', 0, '', 11.4, 15216640),"
            "(648, 1792464431.37, NULL, 'NetworkManager', '', '/usr/bin/NetworkManager --no-daemon', 'root', 'NetworkManager.service', 1, 'systemd', 150.4, 28557312),"
            "(1108, 1792464437.63, NULL, 'upowerd', '', '/usr/lib/upowerd', 'root', 'upower.service', 1, 'systemd', 30.8, 13230080),"
            "(1709, 1792464442.25, NULL, 'udisksd', '', '/usr/lib/udisks2/udisksd', 'root', 'udisks2.service', 1, 'systemd', 0.9, 15089664),"
            "(1978, 1792464444.01, 1792688652.09, 'conky', '/usr/bin/conky', '/usr/bin/conky -c /home/user/.config/conky/conky.conf', 'user', 'conky.service', 774, 'systemd', 1386.0, 33259520),"
            "(1036, 1792464436.2, NULL, 'pipewire', '/usr/bin/pipewire', '/usr/bin/pipewire', 'user', 'pipewire.service', 774, 'systemd', 120.0, 9650176),"
            "(1700001, 1792710000.0, 1792712000.0, 'firefox', '/usr/lib/firefox/firefox', '/usr/lib/firefox/firefox', 'user', 'app-firefox@fc022eb11b464f22a3cd3a4e2aea0542.service', 774, 'systemd', 300.0, 900000000),"
            "(1192391, 1791687786.25, 1791687788.27, 'as', '/usr/bin/as', 'as -I /home/user/repo-work/ActivityScope/src/core --gdwarf-5 --64 -o CMakeFiles/x.o', 'user', 'vte-spawn-ca0c921e-f151-41e3-84ef-64e22628dc1a.scope', 1192301, 'c++', 0.37, 76365824),"
            "(732, 1791730550.74, NULL, 'systemd-oomd', '', '/usr/lib/systemd/systemd-oomd', 'systemd-oom', 'systemd-oomd.service', 1, 'systemd', 264.12, 6176768),"
            "(1572853, 1792695285.2, 1792695298.4, '(system-sleep)', '', '(system-sleep)', 'root', 'systemd-suspend.service', 1572850, 'systemd-sleep', 0, 2000000);"
            "INSERT INTO minute_top(minute, pid, start_ticks, name, cmdline, unit, cpu_seconds, peak_rss, io_bytes, exited) VALUES "
            "(1792665900, 117, 0, 'kswapd0', '', '', 3.92, 0, 0, 0),"
            "(1792665960, 1497050, 0, 'kwin_wayland', '/usr/bin/kwin_wayland --wayland-fd 7 --socket wayland-0', 'plasma-kwin_wayland.service', 2.5, 258220032, 0, 0);";
        char* err = nullptr;
        CHECK(sqlite3_exec(db, rows, nullptr, nullptr, &err) == SQLITE_OK);
        if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
        sqlite3_close(db);
    }
    // The harvested facts for these programs, as factharvest wrote them on this laptop; the shipped notes on top.
    std::ofstream(dir + "/local.tsv")
        << "haruna\tapp\topen source media player built with Qt/QML and libmpv\tdesktop\n"
           "ktorrent\tapp\ta BitTorrent program by KDE\tdesktop\n"
           "plasmashell\tapp\tplasma\tdesktop\n"
           "kwin_wayland\tsystem-service\tKDE Window Manager (Login Manager Version) (ran inside plasma-login-kwin_wayland.service)\tunit:plasma-login-kwin_wayland.service\n"
           "kwin_wayland_wrapper\tsystem-service\tKDE Wayland Compositor (ran inside plasma-kwin_wayland.service)\tunit:plasma-kwin_wayland.service\n"
           "gnome-terminal-server\tsystem-service\tGNOME Terminal Server (ran inside gnome-terminal-server.service)\tunit:gnome-terminal-server.service\n"
           "fan-control.sh\tsystem-service\tMSI Cooler Boost Fan Control (ran inside fan-control.service)\tunit:fan-control.service\n"
           "node-MainThread\tapp\tevented I/O for V8 javascript (\"Current\" release) (from the nodejs package); here it ran pyright-langserver\tpacman:nodejs\n"
           "python3\tcli-tool\tan interpreted, interactive, object-oriented programming language\tman\n"
           "gnome-terminal\tcli-tool\ta terminal emulator for GNOME\tman\n"
           "as\tcli-tool\tthe portable GNU assembler\tman\n"
           "systemd-oomd\tcli-tool\ta userspace out-of-memory (OOM) killer\tman\n";
    as::Facts facts;
    facts.load({dir + "/local.tsv", "facts/base.tsv"});
    // Their desktop entries, the [Desktop Entry] lines as installed.
    std::ofstream(dir + "/apps/org.kde.haruna.desktop")
        << "[Desktop Entry]\nName=Haruna\nGenericName=Media Player\nComment=Open source media player built with Qt/QML and libmpv\nExec=haruna %U\n";
    std::ofstream(dir + "/apps/org.kde.ktorrent.desktop")
        << "[Desktop Entry]\nName=KTorrent\nGenericName=BitTorrent Client\nExec=ktorrent %U\nComment=A BitTorrent program by KDE\n";
    std::ofstream(dir + "/apps/org.gnome.Terminal.desktop")
        << "[Desktop Entry]\nName=Terminal\nComment=Use the command line\nExec=gnome-terminal\n[Desktop Action new-window]\nName=New Window\nExec=gnome-terminal --window\n";
    std::ofstream(dir + "/apps/org.kde.plasmashell.desktop") << "[Desktop Entry]\nExec=/usr/bin/plasmashell\nName=Plasma\n";
    std::ofstream(dir + "/apps/firefox.desktop")
        << "[Desktop Entry]\nExec=/usr/lib/firefox/firefox %u\nName=Firefox\nComment=Fast and private browser\nGenericName=Web Browser\n";
    std::ofstream(dir + "/apps/conky.desktop") << "[Desktop Entry]\nName=conky\nExec=conky --daemonize --pause=1\n";
    // What systemd says each unit is (systemctl show -p Description); a terminal tab's from a live one.
    const std::map<std::string, std::string> units = {
        {"fan-control.service", "MSI Cooler Boost Fan Control"},
        {"plasma-plasmashell.service", "KDE Plasma Workspace"},
        {"plasma-kwin_wayland.service", "KDE Wayland Compositor"},
        {"gnome-terminal-server.service", "GNOME Terminal Server"},
        {"vte-spawn-*.scope", "VTE child process 1497536 launched by org.gnome.Terminal process 1497459"},
        {"app-realm-os@*.service", "REALM//OS"},
        {"init.scope", "System and Service Manager"},
        {"NetworkManager.service", "Network Manager"},
        {"upower.service", "Daemon for power management"},
        {"udisks2.service", "Disk Manager"},
        {"conky.service", "Conky system monitor"},
        {"pipewire.service", "PipeWire Multimedia Service"},
        {"systemd-suspend.service", "System Suspend"},
    };
    std::tm tmv{};
    tmv.tm_year = 2026 - 1900; tmv.tm_mon = 9; tmv.tm_mday = 23; tmv.tm_hour = 0; tmv.tm_min = 1; tmv.tm_sec = 30; tmv.tm_isdst = -1;
    const int64_t now = int64_t(std::mktime(&tmv));  // the clock of the 50-question test

    const char* home = std::getenv("HOME");
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model_dir = em ? em : std::string(home ? home : "") + "/.local/share/unllm/models/bge-small-en-v1.5";
    unllm::Meaning model;
    CHECK(model.load_model(model_dir));  // the same bge-small the kinds matcher uses
    sqlite3* db = nullptr;
    sqlite3_open_v2(dbp.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    unllm::Analyzer an;
    an.set_synonyms(unllm::load_synonyms("vendor/unllm/data/synonyms.txt"));
    as::Names names;
    names.open(db);
    names.set_model(&model);
    names.set_facts(&facts);
    names.set_analyzer(&an);
    names.set_unit_descriptions(units);
    names.set_desktop_dirs({dir + "/apps"});
    auto first = [&](const char* q) {
        auto m = names.find(q, an.read(q, now), now);
        return m.empty() ? std::string() : m[0].program.name;
    };
    // The everyday words of the 50-question test, each to the program the records hold.
    CHECK(first("my taskbar kept restarting yesterday afternoon, how many times was that") == "plasmashell");
    CHECK(first("the torrent app didnt crash again did it") == "ktorrent");
    CHECK(first("how long was the movie player open last night") == "haruna");
    CHECK(first("is that fan control thing running as root legit") == "fan-control.sh");
    CHECK(first("how much cpu did the window manager use yesterday") == "kwin_wayland");
    CHECK(first("whats the python thing that keeps running in the evenings") == "python3");
    {
        auto m = names.find("whats that node thing running in the background",
                            an.read("whats that node thing running in the background", now), now);
        CHECK(!m.empty() && m[0].program.name == "node-MainThread" && m[0].program.script == "realm-companion.js");
    }
    {
        // "the terminal" yesterday morning: a GNOME Terminal tab, whose scope says it was launched by
        // org.gnome.Terminal. The words name the tab's unit, not one program in it.
        const char* q = "what was eating the cpu in the half hour before the terminal got killed yesterday morning";
        auto m = names.find(q, an.read(q, now), now);
        CHECK(!m.empty() && m[0].by_unit && m[0].program.unit == "vte-spawn-*.scope" && m[0].words == "terminal" &&
              m[0].via == "Terminal");
    }
    // Nonsense and the machine's own topics name no program.
    for (const char* q : {"did the sun crash yesterday", "how do i cook a steak", "did my mum use the laptop",
                          "is the fan running right now", "whats the fan speed", "how much battery life is left",
                          "how much disk usage today", "how hot was the cpu yesterday", "how much memory is free",
                          "is my wifi slow", "what happened yesterday", "is anything running as root",
                          "what got killed yesterday"})
        CHECK(first(q).empty());
    // The machine's own word names the machine, never a program: "my laptop throttle" is not the suspend hook whose
    // unit is "System Suspend" (T19).
    for (const char* q : {"did my laptop throttle at 3pm", "does the computer throttle when its hot"}) CHECK(first(q).empty());
    names.gather(0, 0);
    for (const char* p : {"the sun", "a steak", "my mum"}) CHECK(names.resolve(p, now).empty());
    // Exact names: the script on the command line, without its extension; case does not matter.
    {
        auto m = names.resolve("realm-companion", now);
        CHECK(!m.empty() && m[0].program.name == "node-MainThread" && m[0].score == 1.0f);
        auto f = names.resolve("Fan-Control", now);
        CHECK(!f.empty() && f[0].program.name == "fan-control.sh");
    }
    CHECK(as::is_program("node", "node-MainThread", "/usr/bin/node", "node realm-companion.js"));
    CHECK(as::is_program("Node-MainThread", "node-MainThread", "/usr/bin/node", "node realm-companion.js"));
    CHECK(as::is_program("realm-companion.js", "node-MainThread", "/usr/bin/node", "node realm-companion.js"));
    CHECK(as::is_program("python", "python3", "/usr/bin/python3.14", "python3 /home/user/.local/bin/weird --offline"));
    CHECK(as::is_program("fan-control", "fan-control.sh", "", "/bin/bash /usr/local/sbin/fan-control.sh"));
    CHECK(as::is_program("kwin_wayland", "kwin_wayland", "", "/usr/bin/kwin_wayland --wayland-fd 7"));
    CHECK(!as::is_program("firefox", "node-MainThread", "/usr/bin/node", "node realm-companion.js"));
    CHECK(!as::is_program("sleep", "fan-control.sh", "", "/bin/bash /usr/local/sbin/fan-control.sh"));
    CHECK(!as::is_program("2.1", "claude", "/home/user/.local/share/claude/versions/2.1.307", "claude --resume"));
    // The first argument names the program only when it is a file (a path, or a name with an extension): a word handed
    // to another command ("ask chrome safe to run?") names no program that ran.
    CHECK(!as::is_program("chrome", "ask", "/usr/local/bin/ask", "/usr/local/bin/ask --branch chrome safe to run?"));
    CHECK(!as::is_program("steam", "ask", "", "./build/ask --branch steam keeps crashing"));
    CHECK(as::is_program("weird", "python3", "/usr/bin/python3.14", "python3 /home/user/.local/bin/weird --offline"));
    CHECK(as::is_program("fan-control", "bash", "/usr/bin/bash", "/bin/bash /usr/local/sbin/fan-control.sh"));
    CHECK(as::is_program("rewrite", "python3", "/usr/bin/python3.14", "python3 rewrite.py"));
    CHECK(as::fold_unit("vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope") == "vte-spawn-*.scope");
    CHECK(as::fold_unit("app-realm\\x2dos@0d4a935caa294f50b76112e83ca0b973.service") == "app-realm-os@*.service");
    CHECK(as::fold_unit("run-p1851251-i1861925.scope") == "run-p*-i*.scope");
    CHECK(as::fold_unit("fan-control.service") == "fan-control.service");
    CHECK(as::fold_unit("user@1000.service") == "user@1000.service");
    sqlite3_close(db);

    // Through ask: the reading carries the program by the records' name, and the live check finds a process by
    // its executable's name (comm node-MainThread, exe /usr/bin/node).
    {
        std::ofstream(dir + "/kinds/kinds.tsv") << "id\ttopic\tform\twindow\tdata\tanswer\treply\n"
                                                   "fixture.none\tnone\tnone\teither\trecorded\tA fixture.\t\n";
        std::ofstream(dir + "/kinds/phrasings.tsv") << "fixture.none\tthis is a fixture phrasing\n";
        Tree t;
        t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
        t.write("proc/4242/stat", stat_line(4242, "node-MainThread", 'S', 1, 100, 0, 500, 100));
        t.write("proc/4242/cmdline", std::string("node\0realm-companion.js\0", 24));
        symlink("/usr/bin/node", (t.root + "/proc/4242/exe").c_str());
        as::Ask ask;
        CHECK(ask.open(dbp));
        ask.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                          "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
        ask.load_facts({dir + "/local.tsv", "facts/base.tsv"});
        fs::create_directories(dir + "/cache");
        CHECK(ask.load_kinds(dir + "/kinds", dir + "/cache", model_dir));
        ask.set_roots(t.roots());
        as::AskResult wm = ask.ask("how much cpu did the window manager use yesterday", now);
        CHECK(std::find(wm.programs.begin(), wm.programs.end(), "window manager -> kwin_wayland") != wm.programs.end());
        CHECK(!ask.named().empty() && ask.named()[0].program.name == "kwin_wayland");
        as::AskResult nd = ask.ask("is node using a lot of cpu right now", now);
        CHECK(nd.answer.find("isn't running") == std::string::npos && nd.answer.find("node") != std::string::npos);
        CHECK(ask.ask("what is kswapd0", now).answer.find("Kswapd0 is the kernel's memory reclaim thread") == 0);
        // A terminal tab is a unit: shown, but no one program in it joins the reading.
        as::AskResult tab = ask.ask("what was eating the cpu in the half hour before the terminal got killed yesterday morning", now);
        CHECK((tab.programs == std::vector<std::string>{"terminal -> unit vte-spawn-*.scope"}));
        // The person asleep is the night, not the sleep command the records hold (T59).
        for (const char* q : {"how hot did the cpu get last night while i was asleep", "how hot did the cpu get while i was asleep"}) {
            as::AskResult night = ask.ask(q, now);
            CHECK(night.programs.empty() && night.answer.find("While sleep") == std::string::npos);
        }
    }
    fs::remove_all(dir);
}

// The answers from the records (askcore.cpp answer_records), on rows copied from this laptop's records on
// 2026-10-23 at the clock of the 50-question test, each question answered as its catalogue kind: traffic summed
// over the time each reading covers, the swap's size, the battery's plug moments and its history from the log,
// stories that name what happened, out-of-memory kills, how long a program was open, and dates on long spans.
void test_records_data() {
    std::puts("records answers");
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model_dir = em ? em : std::string(home ? home : "") + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model_dir + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: skipped");
        return;
    }
    const std::string dir = fs::temp_directory_path() / ("as-records-" + std::to_string(getpid()));
    fs::create_directories(dir + "/kinds");
    fs::create_directories(dir + "/cache");
    auto at = [](int mday, int h, int m, int s) {
        std::tm tmv{};
        tmv.tm_year = 2026 - 1900; tmv.tm_mon = 9; tmv.tm_mday = mday; tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = s; tmv.tm_isdst = -1;
        return int64_t(std::mktime(&tmv));
    };
    const int64_t now = at(23, 0, 1, 30);  // the clock of the 50-question test
    const std::string dbp = dir + "/h.db";
    { as::Store st(dbp); }  // the schema
    {
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        auto exec = [&](const std::string& sql) {
            char* err = nullptr;
            const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
            if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
            return ok;
        };
        CHECK(exec("BEGIN"));
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO samples(t, net_rx, net_tx, swap_used, swap_total, battery_pct, on_ac, mem_pct) "
                               "VALUES (?, ?, ?, ?, ?, ?, ?, ?)", -1, &st, nullptr);
        auto sample = [&](int64_t t, double rx, double tx, double swap, double swap_total, double pct, double ac, double mem) {
            const double v[] = {rx, tx, swap, swap_total, pct, ac, mem};
            sqlite3_bind_int64(st, 1, t);
            for (int i = 0; i < 7; ++i) v[i] < 0 ? sqlite3_bind_null(st, i + 2) : sqlite3_bind_double(st, i + 2, v[i]);
            sqlite3_step(st);
            sqlite3_reset(st);
        };
        const double cap = 8283222016;  // SwapTotal of the zram device
        // Q49: network readings once a second from 10:15 to 11:00, asleep until 12:00, then half an hour more.
        for (int64_t t = at(22, 10, 15, 0); t < at(22, 11, 0, 0); ++t) sample(t, 10000, 5000, -1, -1, -1, -1, -1);
        for (int64_t t = at(22, 12, 0, 0); t < at(22, 12, 30, 0); ++t) sample(t, 20000, 1000, -1, -1, -1, -1, -1);
        // Q40: an hour of swap, at the cap from 07:10 to 07:30; the size recorded with each reading.
        for (int64_t t = at(22, 7, 0, 0); t < at(22, 8, 0, 0); ++t) {
            const int64_t m = (t - at(22, 7, 0, 0)) / 60;
            sample(t, -1, -1, m < 10 ? 4e9 : m < 30 ? cap : 7e9, cap, -1, -1, -1);
        }
        // The same device on Wednesday evening, before the size was recorded (NULL).
        for (int64_t t = at(21, 20, 0, 0); t < at(21, 20, 10, 0); ++t) sample(t, -1, -1, t < at(21, 20, 5, 0) ? 6e9 : cap, -1, -1, -1, -1);
        // Q9: the battery every 10 s; unplugged at 08:37:49 at 98%, plugged back in at 09:07:16 at 75%.
        for (int64_t t = at(22, 8, 0, 5); t < at(22, 10, 0, 0); t += 10) {
            const int64_t off = at(22, 8, 37, 49), on = at(22, 9, 7, 16);
            const double pct = t < off ? 98 : t < on ? 98 - 23.0 * double(t - off) / double(on - off) : std::min(100.0, 75 + double(t - on) / 120.0);
            sample(t, -1, -1, -1, -1, pct, t >= off && t < on ? 0 : 1, -1);
        }
        // Memory and swap at the 10:11:49 kill.
        sample(at(22, 10, 11, 48), -1, -1, 8283033600, cap, -1, -1, 89.8);
        sqlite3_finalize(st);
        auto row = [&](int64_t t, const char* cat, const std::string& title, const std::string& detail) {
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(db, "INSERT INTO log(t, category, title, detail) VALUES (?, ?, ?, ?)", -1, &q, nullptr);
            sqlite3_bind_int64(q, 1, t);
            sqlite3_bind_text(q, 2, cat, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 3, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 4, detail.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        };
        auto failed = [&](int64_t t, const std::string& unit, const std::string& result) {
            // Rows written before 2026-10-23 cut a D-Bus unit's name at its first colon; the detail keeps the line.
            const std::string cut = unit.find(':') == std::string::npos ? unit : unit.substr(0, unit.find(':'));
            const std::string rest = unit.find(':') == std::string::npos ? "" : unit.substr(unit.find(':') + 1) + ": ";
            row(t, "services", cut + " failed: " + rest + "Failed with result '" + result + "'.",
                "systemd: " + unit + ": Failed with result '" + result + "'.");
        };
        // The power rows LogWatch writes, Thursday and the Sunday evening of Q30 (no readings kept from then).
        row(at(22, 8, 37, 49), "session", "Unplugged from power at 98% battery", "");
        row(at(22, 9, 7, 16), "session", "Plugged into power at 75% battery", "");
        row(at(22, 23, 29, 59), "session", "Unplugged from power at 100% battery", "");
        row(at(22, 23, 46, 21), "session", "Plugged into power at 88% battery", "");
        row(at(18, 19, 7, 44), "session", "Unplugged from power at 100% battery", "");
        row(at(18, 19, 58, 17), "session", "Plugged into power at 18% battery", "");
        // Q10: Thursday afternoon's log, as recorded.
        row(at(22, 12, 26, 13), "session", "Lid opened", "");
        row(at(22, 12, 26, 15), "session", "Woke from sleep", "");
        row(at(22, 12, 26, 26), "startup", "System file added: /etc/resolv.conf", "");
        row(at(22, 13, 7, 11), "session", "Power button pressed", "");
        failed(at(22, 13, 7, 20), "dbus-:1.2-org.kde.kdeconnect@0.service", "exit-code");
        failed(at(22, 13, 7, 20), "dbus-:1.2-org.kde.kwalletd6@0.service", "exit-code");
        row(at(22, 13, 7, 21), "session", "Logged out (Removed session 3.)", "");
        for (const auto& [t, prog] : std::vector<std::pair<int64_t, const char*>>{
                 {at(22, 14, 35, 6), "ascore_tests"}, {at(22, 14, 35, 12), "ask"}, {at(22, 14, 38, 20), "ascore_tests"},
                 {at(22, 14, 38, 27), "ask"}, {at(22, 14, 40, 25), "ascore_tests"}, {at(22, 14, 40, 30), "ask"},
                 {at(22, 22, 56, 30), "ktorrent"}})
            row(t, "problems", std::string(prog) + " crashed", std::string("systemd-coredump: Process 1 (") + prog + ") of user 1000 dumped core.");
        row(at(22, 14, 38, 32), "problems", "ask crashed 2 times between 14:38:27 and 14:38:32", "");
        row(at(22, 14, 54, 43), "session", "Lid closed", "");
        row(at(22, 14, 54, 45), "session", "Went to sleep", "");
        row(at(22, 14, 54, 58), "session", "Woke from sleep", "");
        for (int i = 0; i < 6; ++i) failed(at(22, 15, 49, 40) + i * 1200, "dbus-:1.2-org.kde.KSplash@" + std::to_string(i + 2) + ".service", "exit-code");
        // Q38's sleep, and Q35's Wednesday evening from the wake to the lid.
        row(at(21, 0, 10, 13), "session", "Went to sleep", "");
        row(at(21, 17, 19, 0), "session", "Woke from sleep", "");
        row(at(21, 20, 17, 9), "session", "Unplugged from power at 98% battery", "");
        failed(at(21, 20, 20, 47), "vte-spawn-76f4a2c7-8660-4ef6-8b4d-3b911b3fba8f.scope", "oom-kill");
        failed(at(21, 20, 25, 27), "run-p1169605-i1193312.scope", "oom-kill");
        row(at(21, 20, 26, 6), "problems", "ktorrent crashed", "");
        row(at(21, 20, 40, 50), "software", "Installed 6 packages; removed onnxruntime-cpu", "");
        row(at(21, 20, 54, 4), "problems", "unllm_rerank crashed", "");
        row(at(21, 20, 54, 12), "problems", "unllm_rerank crashed 2 times between 20:54:04 and 20:54:12", "");
        row(at(21, 21, 16, 5), "session", "Plugged into power at 36% battery", "");
        row(at(21, 21, 36, 15), "session", "Lid closed", "");
        row(at(21, 21, 36, 17), "session", "Went to sleep", "");
        // Q39's kills on Thursday, and Q32's on Oct 9 (the kernel's, with its own line) with another one on Oct 12.
        failed(at(22, 7, 23, 31), "ollama.service", "oom-kill");
        failed(at(22, 10, 11, 49), "vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope", "oom-kill");
        failed(at(22, 10, 53, 53), "ollama.service", "oom-kill");
        failed(at(9, 22, 22, 33), "vte-spawn-9e3b58a1-f420-4255-9357-64508d2f98ee.scope", "oom-kill");
        row(at(9, 22, 22, 33), "problems", "Out of memory: the kernel killed blender",
            "kernel: Out of memory: Killed process 443368 (blender) total-vm:12034096kB, anon-rss:4894212kB, file-rss:8928kB, "
            "shmem-rss:0kB, UID:1000 pgtables:12020kB oom_score_adj:200");
        row(at(12, 20, 0, 0), "problems", "Out of memory: the kernel killed python",
            "kernel: Out of memory: Killed process 5555 (python) total-vm:9000000kB, anon-rss:3000000kB, file-rss:1000kB, "
            "shmem-rss:0kB, UID:1000 pgtables:100kB oom_score_adj:0");
        const std::string tab = "vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope";
        const int64_t kill = at(22, 10, 11, 49);
        auto proc = [&](int64_t pid, int64_t started, int64_t ended, const std::string& name, const std::string& exe,
                        const std::string& cmdline, const std::string& unit, const std::string& parent, double cpu, double rss) {
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(db, "INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) "
                                   "VALUES (?, ?, ?, ?, ?, ?, 'user', ?, 1, ?, ?, ?)", -1, &q, nullptr);
            sqlite3_bind_int64(q, 1, pid);
            sqlite3_bind_int64(q, 2, started);
            ended ? sqlite3_bind_int64(q, 3, ended) : sqlite3_bind_null(q, 3);
            const std::string* text[] = {&name, &exe, &cmdline, &unit, &parent};
            for (int i = 0; i < 4; ++i) sqlite3_bind_text(q, 4 + i, text[i]->c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 8, parent.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(q, 9, cpu);
            sqlite3_bind_double(q, 10, rss);
            sqlite3_step(q);
            sqlite3_finalize(q);
        };
        // The terminal tab killed at 10:11:49: its shell under gnome-terminal-server and what ran inside.
        proc(1168789, at(21, 20, 23, 59), kill, "fish", "/usr/bin/fish", "fish", tab, "gnome-terminal-server", 0.2, 1.1e7);
        proc(1412362, at(22, 9, 59, 23), kill, "exe", "", "unllm --serve", tab, "systemd", 120, 3.4e9);
        proc(1168842, at(21, 20, 24, 1), kill, "claude", "/home/user/.local/share/claude/versions/2.1.307", "claude --resume", tab, "fish", 597, 7.3e8);
        proc(1410499, at(22, 9, 57, 28), kill, "claude", "/home/user/.local/share/claude/versions/2.1.307", "claude --resume", tab, "fish", 330, 7.0e8);
        proc(1411432, at(22, 9, 58, 22), kill, "clangd.main", "/usr/bin/clangd", "clangd --background-index", tab, "claude", 40, 3.9e8);
        proc(1362281, at(22, 8, 58, 34), kill, "clangd.main", "/usr/bin/clangd", "clangd --background-index", tab, "claude", 30, 3.3e8);
        proc(1350791, at(22, 8, 40, 30), kill, "node-MainThread", "/usr/bin/node",
             "node /home/user/.npm/_npx/0000/node_modules/.bin/mcp-server --preset mail", tab, "npm exec @softe", 2, 1.7e8);
        proc(1425035, at(22, 10, 11, 45), kill, "unllm", "/home/user/repo-work/unllm/build/unllm", "/home/user/repo-work/unllm/build/unllm --branch can a sem",
             tab, "python3", 0.5, 1e7);
        proc(1455000, at(22, 7, 23, 33), 0, "ollama", "/usr/local/bin/ollama", "/usr/local/bin/ollama serve", "ollama.service", "systemd", 50, 5e8);
        exec("INSERT INTO minute_top(minute, pid, start_ticks, name, cmdline, unit, cpu_seconds, peak_rss, io_bytes, exited) VALUES "
             "(" + std::to_string(kill - 109) + ", 1412362, 0, 'exe', 'unllm --serve', '" + tab + "', 16.2, 1899741184, 0, 0),"
             "(" + std::to_string(kill - 109) + ", 1410499, 0, 'claude', 'claude --resume', '" + tab + "', 239.6, 450002944, 0, 0),"
             "(" + std::to_string(kill - 169) + ", 1422883, 0, 'python', '/home/user/.cache/unllm-train-gpu-venv/bin/python -I x.py', '" + tab +
             "', 87.9, 913006592, 0, 0)");
        // Q34: Monday's last busy spell and the stop at 22:44:30, its last readings and busiest programs.
        exec("INSERT INTO events(id, start, \"end\", kinds, primary_kind, summary, peak_cpu, peak_mem, peak_temp, peak_fan, total_cpu_seconds, min_margin, guard_limit) VALUES "
             "(1152, " + std::to_string(at(19, 22, 38, 15)) + ", " + std::to_string(at(19, 22, 39, 49)) +
             ", 30, 2, 'CPU peaked at 48%, the CPU reached 86 C, mostly firefox', 48.3, 56.2, 86, 80, 399.1, 13, 100),"
             "(1153, " + std::to_string(at(19, 22, 44, 30)) + ", " + std::to_string(at(19, 22, 44, 30)) +
             ", 32, 32, 'The machine stopped without warning on Mon Oct 19 at 22:44:30.', 59.3, 0, 98, 100, 0, 3, 100)");
        exec("INSERT INTO event_samples(event_id, t, cpu, mem_pct, pressure, disk_bps, gpu, temp, fan, guard) VALUES "
             "(1153, " + std::to_string(at(19, 22, 44, 29)) + ", 28.0, 60, 0, 0, -1, 91, 100, 93),"
             "(1153, " + std::to_string(at(19, 22, 44, 30)) + ", 34.2, 60, 0, 0, -1, 95, 85, 97),"
             "(1153, " + std::to_string(at(19, 22, 44, 31)) + ", 37.4, 60, 0, 0, -1, 95, 100, 96)");
        exec("INSERT INTO event_culprits(event_id, rank, kind, name, cmdline, pid, share, cpu_seconds, peak_rss) VALUES "
             "(1153, 0, 'process', 'python', '/home/user/.cache/unllm-train-gpu-venv/bin/python -I rerank_4b50.py', 875680, 0.25, 133.5, 1565863936),"
             "(1153, 1, 'process', 'firefox', '/usr/lib/firefox/firefox', 631744, 0.20, 108.0, 431714304),"
             "(1153, 2, 'process', 'Isolated Web Co', '/usr/lib/firefox/firefox -contentproc -isForBrowser -prefsHandle 0:52867', 841610, 0.17, 91.5, 140115968),"
             "(1152, 0, 'process', 'firefox', '/usr/lib/firefox/firefox', 631744, 0.32, 127.7, 431714304)");
        const int64_t h22 = at(19, 22, 0, 0);
        exec("INSERT INTO samples_hourly(hour_ts, metric, n, med, p05, p95, max) VALUES "
             "(" + std::to_string(h22) + ", 'temp', 3440, 79, 70, 90, 98), (" + std::to_string(h22) + ", 'fan', 3440, 80, 80, 85, 100),"
             "(" + std::to_string(h22) + ", 'gpu', 3383, 33, 0, 100, 100), (" + std::to_string(h22) + ", 'cpu', 3440, 23, 5, 43, 72.9)");
        // Q35: what ran on Wednesday evening.
        proc(700001, at(21, 17, 42, 19), at(21, 18, 4, 25), "blender", "/usr/bin/blender", "/usr/bin/blender", "app-blender.scope", "systemd", 600, 1.5e9);
        proc(700002, at(21, 19, 25, 37), at(21, 20, 20, 37), "blender", "/usr/bin/blender", "/usr/bin/blender", "app-blender.scope", "systemd", 900, 1.34e9);
        proc(700003, at(21, 19, 12, 9), at(21, 20, 26, 9), "ktorrent", "/usr/bin/ktorrent", "/usr/bin/ktorrent magnet:?xt=urn:btih:0000", "app-ktorrent.scope", "systemd", 300, 2.5e8);
        proc(700004, at(21, 20, 19, 59), at(21, 20, 20, 49), "python", "/home/user/.cache/unllm-train-gpu-venv/bin/python",
             "/home/user/.cache/unllm-train-gpu-venv/bin/python -I rerank_4b50.py", "vte-spawn-76f4a2c7-8660-4ef6-8b4d-3b911b3fba8f.scope", "fish", 400, 1.092e10);
        // Q5 and Q38: haruna's runs.
        proc(1846301, at(22, 21, 37, 17), at(22, 22, 56, 25), "haruna", "/usr/bin/haruna",
             "/usr/bin/haruna /home/user/Videos/film.mp4", "app-haruna.service", "systemd", 441, 4.6e8);
        proc(755619, at(20, 17, 40, 16), at(21, 17, 20, 31), "haruna", "/usr/bin/haruna", "/usr/bin/haruna /home/user/Music/song.mp3",
             "app-haruna.service", "systemd", 139, 3e8);
        // Q29: free space from the hourly rollups, Fri Oct 16 to Thursday night; Q49's hourly traffic (one hour
        // only in the rollup, one overlapping the readings kept); Q30's neighbour Saturday in hourly battery rows.
        exec("INSERT INTO samples_hourly(hour_ts, metric, n, med, p05, p95, max, min, on_battery_frac) VALUES "
             "(" + std::to_string(at(16, 0, 0, 0)) + ", 'disk_free_pct', 3600, 51.92, 51.9, 51.95, 51.96, 51.9, 0),"
             "(" + std::to_string(at(22, 22, 0, 0)) + ", 'disk_free_pct', 3600, 25.0, 25.0, 25.1, 25.1, 25.0, 0),"
             "(" + std::to_string(at(22, 8, 0, 0)) + ", 'net_rxtx', 3600, 1000, 0, 5000, 90000, 0, 0),"
             "(" + std::to_string(at(22, 10, 0, 0)) + ", 'net_rxtx', 2700, 15000, 15000, 15000, 15000, 15000, 0),"
             "(" + std::to_string(at(17, 19, 0, 0)) + ", 'battery_pct', 3600, 80, 62, 98, 100, 60, 1),"
             "(" + std::to_string(at(17, 20, 0, 0)) + ", 'battery_pct', 3600, 45, 31, 58, 60, 30, 1),"
             "(" + std::to_string(at(17, 21, 0, 0)) + ", 'battery_pct', 3600, 50, 32, 68, 70, 30, 0)");
        CHECK(exec("COMMIT"));
        sqlite3_close(db);
    }
    // The catalogue's rows for these kinds, one phrasing each: every question is answered as its kind.
    {
        std::ofstream k(dir + "/kinds/kinds.tsv");
        k << "id\ttopic\tform\twindow\tdata\tanswer\treply\n"
             "memory.swap.value.now\tmemory\ta current value\tnow\trecorded\tSwap now.\t\n"
             "memory.swap.value.window\tmemory\ta value over a window\twindow\trecorded\tSwap over a window.\t\n"
             "memory.oom.list.window\tmemory\ta list\twindow\trecorded\tThe kills.\t\n"
             "disk.space.trend.window\tdisk\ta comparison or trend\twindow\trecorded\tFree space.\t\n"
             "network.value.window\tnetwork\ta value over a window\twindow\trecorded\tTraffic.\t\n"
             "battery.value.window\tbattery\ta value over a window\twindow\trecorded\tThe battery.\t\n"
             "diagnose.list.window\tdiagnose\ta list\twindow\trecorded\tThe story.\t\n"
             "story.often.window.process\tstory\thow often or how long\twindow\trecorded\tHow long a program was open.\t\n";
        std::ofstream p(dir + "/kinds/phrasings.tsv");
        for (const char* id : {"memory.swap.value.now", "memory.swap.value.window", "memory.oom.list.window", "disk.space.trend.window",
                               "network.value.window", "battery.value.window", "diagnose.list.window", "story.often.window.process"})
            p << id << "\tfixture phrasing for " << id << "\n";
    }
    // The live machine: all of its swap is one zram device, nearly full.
    Tree t;
    t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
    t.write("proc/meminfo", "MemTotal: 16178404 kB\nMemAvailable: 8000000 kB\nSwapTotal: 8089084 kB\nSwapFree: 84432 kB\n");
    t.write("proc/swaps", "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n/dev/zram0                              partition\t8089084\t\t8004652\t\t100\n");
    t.write("sys/block/zram0/mm_stat", "8196763648 2100000000 2200000000 0 2300000000 0 0 0 0\n");
    as::Ask ask;
    CHECK(ask.open(dbp));
    ask.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                      "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
    ask.load_facts({"facts/base.tsv"});
    CHECK(ask.load_kinds(dir + "/kinds", dir + "/cache", model_dir));
    ask.set_roots(t.roots());
    auto as_kind = [&](const char* kind, const char* q) {
        ask.force_reading(kind);
        as::AskResult r = ask.ask(q, now);
        ask.force_reading("");
        return r;
    };
    // Each check prints the answer it read when it fails.
    auto has = [](const as::AskResult& r, const std::string& s) {
        const bool ok = r.answer.find(s) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  wanted \"%s\" in: %s\n", s.c_str(), r.answer.c_str());
        return ok;
    };
    auto lacks = [](const as::AskResult& r, const std::string& s) {
        const bool ok = r.answer.find(s) == std::string::npos;
        if (!ok) std::fprintf(stderr, "  did not want \"%s\" in: %s\n", s.c_str(), r.answer.c_str());
        return ok;
    };

    // 1. Q49: each reading counts for the second it covers; the gap and the hour the readings already hold add
    // nothing; download and upload apart, download first. 63 MB down, 15 MB up, 4 MB more from the hourly record.
    {
        as::AskResult r = as_kind("network.value.window", "how much did i download yesterday");
        CHECK(has(r, "63 MB") && has(r, "15 MB") && has(r, "82 MB"));
        CHECK(r.answer.find("63 MB") < r.answer.find("15 MB"));
        CHECK(lacks(r, "282 MB"));                      // the average stretched over the sleep: the old figure
        CHECK(has(r, "Thu 11:00") && has(r, "Thu 12:00"));  // the gap is named
    }
    // 2. Q40: full means of the size recorded with each reading; how long it sat at the cap.
    {
        as::AskResult r = as_kind("memory.swap.value.window", "how full did the swap get yesterday");
        CHECK(has(r, "100%") && has(r, "of 8.3 GB"));
        CHECK(has(r, "at the cap") && has(r, "Thu 07:10"));
        CHECK(has(r, "All of it is zram"));
        // Wednesday's readings have no size: the same device now (every size recorded matches), and said so.
        as::AskResult w = as_kind("memory.swap.value.window", "how full did the swap get on wednesday");
        CHECK(has(w, "100%") && has(w, "wasn't recorded"));
        // The swap now: all of it is zram, not "some".
        as::AskResult n = as_kind("memory.swap.value.now", "how full is the swap");
        CHECK(has(n, "All of it is zram") && lacks(n, "Some of it"));
        // Another machine's live swap (4 GB, recorded sizes 8.3 GB): Wednesday's size is not known, so bytes only.
        Tree t2;
        t2.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
        t2.write("proc/meminfo", "MemTotal: 16178404 kB\nMemAvailable: 8000000 kB\nSwapTotal: 4194304 kB\nSwapFree: 4194304 kB\n");
        as::Ask other;
        CHECK(other.open(dbp));
        other.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                            "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
        CHECK(other.load_kinds(dir + "/kinds", dir + "/cache", model_dir));
        other.set_roots(t2.roots());
        other.force_reading("memory.swap.value.window");
        as::AskResult o = other.ask("how full did the swap get on wednesday", now);
        CHECK(has(o, "8.3 GB") && lacks(o, "%"));
    }
    // 3. Q9: the moments it was unplugged and plugged back in, with their levels, and the low point.
    {
        as::AskResult r = as_kind("battery.value.window", "when did i plug the charger back in yesterday morning and how low did the battery get");
        CHECK(has(r, "unplugged at Thu 08:37 (98%)") && has(r, "plugged back in at Thu 09:07 (75%)"));
        CHECK(has(r, "lowest 75%"));
        CHECK(lacks(r, "23:29"));  // the night's unplug is not the morning's
        CHECK(has(r, "between Thu 05:00 and Thu 12:00"));  // a short span keeps the short form
    }
    // 4. Q30: no readings kept from Sunday: the log's power rows give the drain, and the answer says so.
    {
        as::AskResult r = as_kind("battery.value.window", "how fast was the battery going down on sunday evening");
        CHECK(has(r, "100%") && has(r, "18%") && has(r, "97% an hour") && has(r, "Sun 19:07") && has(r, "Sun 19:58"));
        CHECK(has(r, "log"));
        // Saturday has hourly battery rows: those come first, and the answer says so.
        as::AskResult s = as_kind("battery.value.window", "how fast was the battery going down on saturday evening");
        CHECK(has(s, "hourly") && has(s, "35% an hour") && has(s, "lowest 30%"));
    }
    // 5. Q10: the story names what happened; a folded "2 times" row counts one more crash.
    {
        as::AskResult r = as_kind("diagnose.list.window", "anything weird happen since lunch yesterday");
        CHECK(has(r, "ascore_tests crashed 3 times") && has(r, "ask crashed 4 times") && has(r, "ktorrent crashed"));
        // (Session rows are told as moments by their heads since round 3: "lid closed, then went to sleep".)
        CHECK(has(r, "KSplash") && has(r, "6 times") && has(r, "went to sleep") && has(r, "power button pressed"));
        CHECK(has(r, "KSplash@2-7.service failed (exit-code) 6 times"));  // the instances as a range, not the first one
        CHECK(lacks(r, "crashes or errors") && lacks(r, "power or session events"));
    }
    // Q34: a window that ends where an event is named ("before it cut out"): what the machine was doing right
    // before, its last readings and busiest programs with their command lines.
    {
        as::AskResult r = as_kind("diagnose.list.window", "what was the laptop doing between 10 and 10:44 on monday night before it cut out");
        CHECK(has(r, "Right before Mon 22:44"));
        CHECK(has(r, "96 C") && has(r, "37%") && has(r, "rerank_4b50.py") && has(r, "firefox"));
        CHECK(has(r, "98 C"));  // the hour's maximum, from the hourly record
    }
    // Q35: before the lid closed: the stretch since the machine last woke, with what ran and what happened.
    {
        as::AskResult r = as_kind("diagnose.list.window", "what was the laptop up to between 17:00 and 21:36 on wednesday before i shut the lid");
        CHECK(has(r, "since Wed 17:19") && has(r, "blender") && has(r, "ktorrent crashed") && has(r, "Installed 6 packages"));
        CHECK(has(r, "oom-kill") && has(r, "unllm_rerank crashed 2 times"));
    }
    // 6. Q39: the unit that was killed, what started it, what it held and its main processes.
    {
        as::AskResult r = as_kind("memory.oom.list.window", "what got killed at 10 past 10 yesterday morning");
        CHECK(has(r, "vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope") && has(r, "gnome-terminal-server"));
        CHECK(has(r, "unllm --serve (3.4 GB") && has(r, "claude --resume") && has(r, "clangd --background-index"));
        CHECK(has(r, "systemd-oomd") && has(r, "3.3 GB"));  // the minutes before: 1.9 + 0.45 + 0.91 GB held
        CHECK(lacks(r, "ollama"));
        // Q32: kills filtered by the program named, over the other week; the kernel's own figure.
        as::AskResult b = as_kind("memory.oom.list.window", "how much memory was blender using when it got killed the other week");
        CHECK(has(b, "blender") && has(b, "5.0 GB") && has(b, "Oct 9") && has(b, "kernel"));
        CHECK(lacks(b, "python"));
    }
    // 7. Q5 and Q38: how long a program was open, cut to the window; the longest run whole, and the sleep in it.
    {
        as::AskResult r = as_kind("story.often.window.process", "how long was haruna open last night");
        CHECK(has(r, "one hour and 19 minutes") && has(r, "Thu 21:37") && has(r, "Thu 22:56") && has(r, "film.mp4"));
        as::AskResult m = as_kind("story.often.window.process", "how long did i leave that mp3 open in haruna on tuesday");
        CHECK(has(m, "six hours and 19 minutes") && has(m, "song.mp3") && has(m, "23 hours and 40 minutes"));
        CHECK(has(m, "six hours and 31 minutes"));  // awake: the laptop slept from Wed 00:10 to 17:19
    }
    // 8. Q29: a span longer than six days names its dates.
    {
        as::AskResult r = as_kind("disk.space.trend.window", "how much has my drive filled up since last friday");
        CHECK(has(r, "Fri Oct 16 00:00") && has(r, "Thu Oct 22 22:00"));
        CHECK(r.span.find("Oct 16") != std::string::npos);
    }
    fs::remove_all(dir);
}

// The answers from the log and the records (crashes and failed units, restarts, sleep and wake, boots, updates, trust,
// config) on rows copied from this laptop's records at 00:01 on 2026-10-23 (tests/records-log.sql), at the clock of
// the 50-question test. The kind is held fixed where the fault was in its answer.
void test_records_log() {
    std::puts("records: crashes, failed units, sleep, boots, updates, trust, config");
    namespace fs = std::filesystem;
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model = em ? em : home + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: records checks skipped");
        return;
    }
    const std::string dir = fs::temp_directory_path() / ("as-reclog-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const std::string dbp = dir + "/h.db";
    { as::Store st(dbp); }  // the schema
    {
        std::ifstream f("tests/records-log.sql");
        std::string sql;
        for (std::string line; std::getline(f, line);) sql += line + "\n";
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        char* err = nullptr;
        CHECK(!sql.empty() && sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK);
        if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
        sqlite3_close(db);
    }
    std::tm tmv{};
    tmv.tm_year = 2026 - 1900; tmv.tm_mon = 9; tmv.tm_mday = 23; tmv.tm_hour = 0; tmv.tm_min = 1; tmv.tm_sec = 30; tmv.tm_isdst = -1;
    const int64_t now = int64_t(std::mktime(&tmv));
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string bin = n > 0 ? std::string(exe, std::size_t(n)) : std::string("./ascore_tests");
    const std::string cache = bin.substr(0, bin.rfind('/')) + "/kinds-cache";
    fs::create_directories(cache);
    // The harvested note on the fan script, as factharvest wrote it on this laptop.
    std::ofstream(dir + "/local.tsv") << "fan-control.sh\tsystem-service\tMSI Cooler Boost Fan Control (ran inside fan-control.service)\tunit:fan-control.service\n";
    // Running now: the fan script under its unit.
    Tree t;
    t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
    t.write("proc/926/stat", stat_line(926, "fan-control.sh", 'S', 1, 100, 0, 500, 100));
    t.write("proc/926/cmdline", std::string("/bin/bash\0/usr/local/sbin/fan-control.sh\0", 41));
    t.write("proc/926/cgroup", "0::/system.slice/fan-control.service\n");
    // Each question a new conversation; "" lets the catalogue choose the kind.
    auto ask_as = [&](const std::string& kind, const char* q) {
        as::Ask a;
        a.open(dbp);
        a.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                        "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
        a.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        a.load_facts({dir + "/local.tsv", "facts/base.tsv"});
        a.load_kinds("data/kinds", cache, model);
        a.set_roots(t.roots());
        a.force_reading(kind);
        return a.ask(q, now);
    };
    auto has = [](const as::AskResult& r, const char* text) {
        const bool ok = r.answer.find(text) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  no \"%s\" in [%s] %s\n", text, r.branch.c_str(), r.answer.c_str());
        return ok;
    };
    auto lacks = [](const as::AskResult& r, const char* text) {
        const bool ok = r.answer.find(text) == std::string::npos;
        if (!ok) std::fprintf(stderr, "  \"%s\" in [%s] %s\n", text, r.branch.c_str(), r.answer.c_str());
        return ok;
    };

    // Crashes: core dumps, failed units and out-of-memory kills, by program. Ollama never dumped core on Thursday;
    // systemd-oomd killed ollama.service twice.
    as::AskResult ol = ask_as("crashes.list.window", "can u clairify the spesifics on the ollama crash yesterday");
    CHECK(has(ol, "07:23") && has(ol, "10:53") && has(ol, "memory") && lacks(ol, "No crashes"));
    // The terminal tab where "ollama show" once ran is not ollama's unit; and its unit failing is no yes to "did it crash".
    CHECK(has(ol, "twice") && lacks(ol, "vte-spawn") && lacks(ol, "Yes"));
    // ktorrent crashed at 22:56 on Thursday, and on Monday and Wednesday before. (The analyzer reads a question as
    // yes or no only when it starts with "did", "is" and the like, so the tag question gets no "Yes".)
    as::AskResult kt = ask_as("crashes.list.window", "ktorrent didnt crash again did it");
    CHECK(has(kt, "22:56") && has(kt, "Mon 18:03") && has(kt, "Wed 20:26"));
    CHECK(has(ask_as("crashes.list.window", "did ktorrent crash again"), "Yes"));
    CHECK(has(ask_as("crashes.when.window", "when did ktorrent last crash"), "October 22 at 22:56"));
    as::AskResult most = ask_as("crashes.which.window", "what crashed the most yesterday");
    CHECK(has(most, "KSplash") && most.answer.find("KSplash") < most.answer.find("ollama"));

    // The failed units, ranked, by their real names.
    as::AskResult sv = ask_as("startup.which.window", "what services failed yesterday");
    CHECK(has(sv, "leven unit failures") && has(sv, "org.kde.KSplash") && has(sv, "six times") && has(sv, "ollama.service"));
    CHECK(lacks(sv, "dbus-") && lacks(sv, "startup change"));

    // A program's restarts: plasmashell started 11 times on Thursday afternoon (the boot-time one ended at 13:07).
    as::AskResult ps = ask_as("startup.often.window", "how many times did plasmashell restart yesterday afternoon");
    CHECK(has(ps, "leven times") && has(ps, "13:07") && has(ps, "17:51"));
    CHECK(has(ask_as("", "my taskbar kept restarting yesterday afternoon, how many times was that"), "leven times"));

    // Sleep and wake.
    as::AskResult lunch = ask_as("battery.sleep.when.window",
                                 "did the laptop go to sleep while i was out at lunch yesterday or did it stay on the whole time");
    CHECK(has(lunch, "Yes") && has(lunch, "11:07") && has(lunch, "12:26"));
    as::AskResult slept = ask_as("battery.sleep.list.window", "when did the laptop sleep yesterday");
    CHECK(has(slept, "06:45") && has(slept, "11:07") && has(slept, "14:54"));
    as::AskResult asleep = ask_as("battery.sleep.often.window", "how long was the laptop asleep yesterday");
    CHECK(has(asleep, "three") && has(asleep, "Wed 21:36"));
    // Boots answer the window asked, and a wake when the machine woke rather than booted.
    as::AskResult wed = ask_as("startup.when.reboot", "what time did the laptop start up on wednesday");
    CHECK(has(wed, "woke at Wed 17:19") && lacks(wed, "It last started"));
    as::AskResult one = ask_as("startup.often.reboot", "did i restart around 1 yesterday? all my windows closed on me");
    CHECK(has(one, "No") && has(one, "12:26") && has(one, "13:07") && has(one, "logged in"));
    // With no time named: the last boot, and the last wake from sleep after it.
    CHECK(has(ask_as("startup.when.reboot", "when did the laptop last wake up"), "woke from sleep at Thu 14:54"));
    // Boots before the journal reaches, from the recorder's own pid 1 rows.
    CHECK(has(ask_as("startup.when.reboot", "what time did the laptop start up on october 11"), "10:55"));
    // Unclean stops this month: six, with dates; a count gets no "Yes".
    as::AskResult cut = ask_as("crashes.yesno.window.system", "how many times has the laptop cut out by itself this month");
    CHECK(has(cut, "6 times") && has(cut, "Oct 5 22:57") && lacks(cut, "Yes"));
    // A moment the laptop slept through is said first (T32): at noon yesterday it was asleep, from 11:07 to 12:26.
    as::AskResult noon = ask_as("", "was there a spike in disk io at noon yesterday");
    CHECK(has(noon, "asleep") && has(noon, "11:07") && has(noon, "12:26") && lacks(noon, "can't say"));
    CHECK(has(ask_as("", "how hot was it at 11:30 yesterday"), "asleep"));
    CHECK(lacks(ask_as("", "how hot was it at 14:00 yesterday"), "asleep"));
    // A clause about the person that names nothing the records hold is not placed on some record (T19: "when i had the
    // zoom call" landed on "ask crashed" at 14:40): the time asked stays, and the answer says the clause matched nothing.
    for (const auto& [q, span] : std::vector<std::pair<const char*, const char*>>{
             {"did my laptop throttle at 3pm when i had the zoom call", "Thu 14:15 to Thu 15:45"},
             {"how busy was the cpu at 2pm yesterday when we were on the call", "Thu 13:15 to Thu 14:45"}}) {
        as::AskResult z = ask_as("heat.value.window", q);
        CHECK(z.span == span && has(z, "Nothing in the records matches"));
        for (const auto& rc : z.receipts) CHECK(rc.find("ask crashed") == std::string::npos);
    }
    // A question the records cannot hold about a program that never ran gets both (T88): what is not recorded, then
    // that the program never ran.
    as::AskResult tabs = ask_as("", "is chrome tracking which websites i visit");
    CHECK(tabs.branch.rfind("unanswered:", 0) == 0 && has(tabs, "hasn't run on this machine"));
    // "during the update": the latest software change, Wed 20:40, and the two minutes before it and the minute after (T36).
    for (const char* q : {"disk writes during update", "how hot did it get during the last update"}) {
        as::AskResult u = ask_as("disk.value.window", q);
        CHECK(u.span == "Wed 20:38 to Wed 20:41" && has(u, "Installed 6 packages"));
    }
    // A program the records never saw but installed by hand on the PATH ("the widget cli thing": widget-cli) is said
    // never to have run, and checked as that file (T41).
    {
        const std::string bin = dir + "/bin", path = std::getenv("PATH") ? std::getenv("PATH") : "";
        fs::create_directories(bin);
        std::ofstream(bin + "/widget-cli") << "#!/bin/sh\n";
        fs::permissions(bin + "/widget-cli", fs::perms::owner_all);
        setenv("PATH", (bin + ":" + path).c_str(), 1);
        as::AskResult w = ask_as("trust.identity", "what does the widget cli thing running do");
        CHECK(has(w, "widget-cli") && has(w, "hasn't run on this machine") && has(w, bin.c_str()) && w.branch != "trust:which");
        setenv("PATH", path.c_str(), 1);
    }
    // The person's bedtime is not the laptop's sleep (T9): "since i went to bed" runs from last night, not from the
    // 14:54 sleep.
    as::AskResult bed = ask_as("", "ok so what changed on the machine since i went to bed");
    CHECK(bed.span.rfind("Thu 18:00", 0) == 0);
    for (const auto& rc : bed.receipts) CHECK(rc.rfind("event: ", 0) != 0);
    // A time named that has ended is not now (T20): asked at 13:59, "this morning" is answered over the morning by the
    // kind about a window, though the nearest phrasing is about now. A time up to now keeps the kind about now.
    {
        const int64_t afternoon = now + 13 * 3600 + 57 * 60 + 38;  // Fri 13:59:08
        auto kind_at = [&](const char* q) {
            as::Ask a;
            a.open(dbp);
            a.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                            "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
            a.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
            a.load_facts({dir + "/local.tsv", "facts/base.tsv"});
            a.load_kinds("data/kinds", cache, model);
            a.set_roots(t.roots());
            const as::AskResult r = a.ask(q, afternoon);
            if (r.readings.empty()) return std::string();
            std::fprintf(stderr, "  [%s] %s -> %s\n", r.branch.c_str(), q, r.readings[0].c_str());
            return r.readings[0];
        };
        CHECK(kind_at("battery percent this morning") == "battery.value.window");
        CHECK(kind_at("swap percent this morning") == "memory.swap.value.window");
        CHECK(kind_at("whats eating my memory this morning") == "memory.which.window");
        CHECK(kind_at("how full is the disk today") == "disk.space.value.now");
    }

    // Updates: the last upgrade of packages, not the last install; a named package's history and version.
    as::AskResult full = ask_as("updates.when.window", "when did i last do a full system update");
    CHECK(has(full, "October 11 at 20:35") && has(full, "278") && full.answer.find("October 11") < full.answer.find("October 21"));
    as::AskResult cuda = ask_as("", "when did i put cuda on here and what version is it");
    CHECK(!cuda.refused && has(cuda, "20:59") && has(cuda, "13.4.2-1") && has(cuda, "13.3.1-1"));
    // One of 278 upgraded is not upgraded "with" the first three others in the list.
    CHECK(lacks(ask_as("updates.identity.package", "what version of perl do i have"), "with bcg729"));

    // Trust: the script the root process runs, and that it is running.
    as::AskResult fan = ask_as("trust.identity", "is that fan control thing running as root legit");
    CHECK(has(fan, "/usr/local/sbin/fan-control.sh") && has(fan, "running") && has(fan, "fan-control.service"));
    CHECK(lacks(fan, "pacman"));
    for (const auto& rc : fan.receipts) CHECK(rc.find("no process") == std::string::npos);

    // Config: hindsight's snapshots, none after the clock counted; a file is named by its name or a clear part of it.
    {
        const std::string fake = dir + "/home", repo = fake + "/.local/share/hindsight/repo";
        fs::create_directories(repo + "/etc/systemd/system");
        fs::create_directories(repo + "/home");
        std::system(("git init -q '" + repo + "'").c_str());
        auto snap = [&](const char* date) {
            std::system(("cd '" + repo + "' && export GIT_AUTHOR_DATE='" + date + "' GIT_COMMITTER_DATE='" + date +
                         "' && git -c user.name=t -c user.email=t@t add -A && git -c user.name=t -c user.email=t@t commit -qm snapshot >/dev/null 2>&1")
                            .c_str());
        };
        std::ofstream(repo + "/home/.claude__settings.json") << "{\"a\": 1}\n";
        std::ofstream(repo + "/etc/hostname") << "laptop\n";
        std::ofstream(repo + "/etc/usb_modeswitch.setup") << "# setup\n";
        snap("2026-10-12T10:00:00");
        std::ofstream(repo + "/etc/systemd/system/cpu-power-limits.service") << "[Unit]\nDescription=Cap CPU power\n";
        snap("2026-10-17T06:53:14");
        std::ofstream(repo + "/home/.claude__settings.json") << "{\"a\": 2}\n";
        snap("2026-10-23T00:11:39");  // after the clock
        setenv("HOME", fake.c_str(), 1);
        as::AskResult cap = ask_as("config.when.window", "when did i set up the cpu power cap and what does it actually do");
        CHECK(lacks(cap, "settings.json") && has(cap, "October 16 at 14:22") && has(cap, "October 17"));
        // "set up" is no file's name; the unit's second install, three minutes later, is its latest change.
        CHECK(lacks(cap, "usb_modeswitch") && has(cap, "October 16 at 14:25"));
        as::AskResult set = ask_as("config.when.window", "when did my settings.json file change");
        CHECK(has(set, "settings.json") && has(set, "October 12") && lacks(set, "October 23"));
        setenv("HOME", home.c_str(), 1);
    }
    fs::remove_all(dir);
}

// The event answers (heat, CPU, memory, GPU, disk) of the 50-question test, against rows shaped like the records
// copied on 2026-10-23: 'unseen' culprit rows (a parent's finished children, which also have rows of their own), a
// per-minute row that books a killed tab's whole lifetime of CPU to systemd in one minute, a disk event left open
// across the night's sleep, the twin row of a restarted process, the out-of-memory kills in the log.
void test_event_answers() {
    std::puts("event answers");
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model_dir = em ? em : std::string(home ? home : "") + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model_dir + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: event answer checks skipped");
        return;
    }
    const std::string dir = fs::temp_directory_path() / ("as-events-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const std::string dbp = dir + "/h.db";
    auto at = [](int day, int h, int m, int s) {
        std::tm tm{};
        tm.tm_year = 2026 - 1900; tm.tm_mon = 9; tm.tm_mday = day; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s; tm.tm_isdst = -1;
        return int64_t(std::mktime(&tm));
    };
    auto T = [&](int h, int m, int s = 0) { return at(22, h, m, s); };  // Thursday, "yesterday"
    const int64_t now = at(23, 0, 1, 30);                                 // the clock of the 50-question test
    { as::Store st(dbp); }
    {
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        auto exec = [&](const std::string& sql) {
            char* err = nullptr;
            const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
            if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
            CHECK(ok);
        };
        auto num = [](double v) { return std::to_string(v); };
        exec("BEGIN");
        // Per-second readings (here one every 30 s) for the awake morning and the evening; twelve cores.
        const std::string cores = "'1,1,1,1,1,1,1,1,1,1,1,1'";
        auto sample = [&](int64_t t, double cpu, double temp, double fan, double disk_r) {
            exec("INSERT OR REPLACE INTO samples(t, cpu, temp, fan, disk_r, disk_w, gpu, mem_pct, cores) VALUES(" + num(double(t)) + "," +
                 num(cpu) + "," + num(temp) + "," + num(fan) + "," + num(disk_r) + ",1e6,10,80," + cores + ")");
        };
        for (int64_t t = T(6, 45); t <= T(11, 7); t += 30) sample(t, 20, 60, 60, 1e8);
        for (int64_t t = T(18, 0); t < at(23, 0, 0, 0); t += 30) sample(t, 4, 46, 40, 1e6);
        sample(T(10, 2, 25), 20, 95, 60, 2.11e9);                      // the morning's hottest moment and disk peak
        for (int64_t t : {T(9, 21, 55), T(9, 35, 56), T(10, 8, 45), T(10, 9, 0), T(10, 9, 30), T(10, 9, 51)})
            sample(t, 20, 60, 85, 1e8);                                // the fan at its peak
        sample(T(10, 11, 0), 15, 60, 60, 1e8);                         // the machine used 108 s in the 10:11 minute
        sample(T(10, 11, 30), 15, 60, 60, 1e8);
        sample(T(22, 10, 0), 28.5, 52, 55, 1e6);                       // the busiest moment while haruna was open
        sample(T(23, 30, 0), 47, 60, 60, 1e6);                         // after haruna closed
        sample(T(23, 40, 0), 30, 73, 72, 1e6);                         // the evening's hottest moment
        // Events. 1: a disk event opened on Wednesday evening and closed at the first sample after the night's sleep.
        exec("INSERT INTO events(id, start, end, kinds, summary, peak_cpu, peak_disk, total_cpu_seconds) VALUES"
             "(1," + num(double(at(21, 21, 15, 56))) + "," + num(double(T(6, 45, 26))) + ",5,'disk 2.5 GB/s',90,2.48e9,6000),"
             "(2," + num(double(T(10, 2, 0))) + "," + num(double(T(10, 5, 0))) + ",5,'disk 2.1 GB/s',100,2.11e9,3000)");
        exec("INSERT INTO event_samples(event_id, t, cpu, disk_bps) VALUES"
             "(1," + num(double(at(21, 21, 16, 0))) + ",90,2.48e9),(1," + num(double(at(21, 21, 30, 0))) + ",50,1e9),"
             "(1," + num(double(T(6, 45, 26))) + ",10,1e6),(2," + num(double(T(10, 2, 25))) + ",100,2.11e9),(2," +
             num(double(T(10, 3, 0))) + ",60,1e9)");
        exec("INSERT INTO event_culprits(event_id, kind, name, cmdline, pid, cpu_seconds, share, io_bytes, gpu_mem) VALUES"
             "(1,'unseen','systemd','',774,5692,0.95,0,0),(1,'process','claude','claude -p',1168900,200,0.03,2e11,0),"
             "(2,'unseen','systemd','',774,2500,0.83,0,0),(2,'process','claude','claude -p',1168901,300,0.1,1.2e12,0),"
             "(2,'process','python3','python3 rewrite50.py',1168902,100,0.03,8.5e11,0)");
        // GPU: the event at 09:19 holds Ollama's runner with the most video memory; the busier one by CPU is exe.
        exec("INSERT INTO events(id, start, end, kinds, summary, peak_cpu, peak_gpu, total_cpu_seconds) VALUES"
             "(3," + num(double(T(9, 19, 36))) + "," + num(double(T(9, 23, 2))) + ",9,'gpu',60,84,1000),"
             "(4," + num(double(T(8, 46, 0))) + "," + num(double(T(8, 54, 0))) + ",9,'gpu',60,100,1000)");
        exec("INSERT INTO event_culprits(event_id, kind, name, cmdline, pid, cpu_seconds, share, io_bytes, gpu_mem) VALUES"
             "(3,'process','exe','unllm --serve',1372352,600,0.6,0,2.54e9),"
             "(3,'process','llama-server','/usr/local/lib/ollama/llama-server --model m',1376606,300,0.3,0,4.01e9),"
             "(4,'process','exe','unllm --serve',1372352,900,0.9,0,2.5e9)");
        // CPU: spikes where Ollama holds the big shares, the killed tab's lifetime booked to systemd, and the evening.
        exec("INSERT INTO events(id, start, end, kinds, summary, peak_cpu, peak_temp, min_margin, total_cpu_seconds) VALUES"
             "(5," + num(double(T(9, 30, 0))) + "," + num(double(T(9, 40, 0))) + ",1,'cpu',100,70,-1,2000),"
             "(6," + num(double(T(10, 11, 30))) + "," + num(double(T(10, 12, 0))) + ",1,'cpu',100,70,-1,12000),"
             "(7," + num(double(T(22, 10, 0))) + "," + num(double(T(22, 11, 0))) + ",1,'cpu',28.5,52,-1,100),"
             "(8," + num(double(T(23, 30, 0))) + "," + num(double(T(23, 32, 0))) + ",1,'cpu',47,60,-1,300),"
             "(9," + num(double(T(10, 2, 0))) + "," + num(double(T(10, 4, 0))) + ",16,'heat',60,95,5,500),"
             "(10," + num(double(T(23, 40, 0))) + "," + num(double(T(23, 41, 0))) + ",16,'heat',30,73,-1,100)");
        exec("INSERT INTO event_culprits(event_id, kind, name, cmdline, pid, cpu_seconds, share) VALUES"
             "(5,'process','ollama','/usr/local/bin/ollama serve',1276284,1200,0.6),"
             "(5,'process','llama-server','/usr/local/lib/ollama/llama-server --model m',1376606,600,0.3),"
             "(5,'process','claude','claude --resume',1168842,100,0.05),"
             "(6,'unseen','systemd','',774,11958,0.99),(6,'process','claude','claude --resume',1410499,30,0.0025),"
             "(7,'process','claude','claude --resume',1497737,50,0.5),(7,'process','haruna','/usr/bin/haruna movie.mp4',1846301,30,0.3),"
             "(8,'process','python3','python3 askjudge.py',1900000,240,0.8),"
             "(9,'unseen','systemd','',774,450,0.9),(9,'process','claude','claude -p',1168901,50,0.1),"
             "(10,'process','python3','python3 askjudge.py',1900000,90,0.9)");
        // Per-minute lists.
        auto minutes = [&](int64_t from, int64_t to, int pid, const char* name, const char* cmdline, const char* unit, double cpu) {
            for (int64_t m = from; m < to; m += 60)
                exec("INSERT INTO minute_top(minute, pid, start_ticks, name, cmdline, unit, cpu_seconds, peak_rss, io_bytes, exited) VALUES(" +
                     num(double(m)) + "," + std::to_string(pid) + ",0,'" + name + "','" + cmdline + "','" + unit + "'," + num(cpu) + ",1e8,0,0)");
        };
        const char* tab = "vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope";
        minutes(T(9, 42), T(10, 12), 1410499, "claude", "claude --resume", tab, 20);   // 600 s in the half hour before the kill
        minutes(T(9, 42), T(10, 12), 1372352, "exe", "unllm --serve", "app-unllm.service", 15);
        minutes(T(9, 42), T(10, 12), 1400000, "python", "python askkind40.py", tab, 9);
        minutes(T(10, 11), T(10, 12), 774, "systemd", "/usr/lib/systemd/systemd --user", "init.scope", 11958);
        minutes(T(9, 53), T(9, 54), 774, "systemd", "/usr/lib/systemd/systemd --user", "init.scope", 323);
        minutes(T(8, 0), T(9, 0), 1168903, "python3", "python3 rewrite50.py", tab, 60);  // busy, but earlier
        minutes(T(7, 40), T(7, 41), 1168842, "claude", "claude --resume", tab, 4187);    // a lump: the machine used 144 s
        minutes(T(13, 0), T(15, 0), 1497737, "claude", "claude --resume", tab, 30);
        minutes(T(9, 0), T(10, 0), 1276284, "ollama", "/usr/local/bin/ollama serve", "ollama.service", 13);
        minutes(T(9, 0), T(10, 0), 1376606, "llama-server", "/usr/local/lib/ollama/llama-server --model m", "ollama.service", 8);
        minutes(T(7, 0), T(12, 0), 1530, "kwin_wayland", "/usr/bin/kwin_wayland --wayland-fd 7", "plasma-kwin_wayland.service", 3);
        minutes(T(13, 10), T(18, 10), 1497050, "kwin_wayland", "/usr/bin/kwin_wayland --wayland-fd 7", "plasma-kwin_wayland.service", 3);
        minutes(T(21, 38), T(22, 56), 1846301, "haruna", "/usr/bin/haruna movie.mp4", "app-org.kde.haruna@1.service", 5.6);
        // Processes.
        auto proc = [&](int pid, int64_t started, int64_t ended, const char* name, const char* exe, const char* cmdline,
                        const char* unit, int ppid, const char* parent, double cpu, double rss) {
            exec("INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES(" +
                 std::to_string(pid) + "," + num(double(started)) + "," + (ended ? num(double(ended)) : std::string("NULL")) + ",'" +
                 name + "','" + exe + "','" + cmdline + "','user','" + unit + "'," + std::to_string(ppid) + ",'" + parent + "'," +
                 num(cpu) + "," + num(rss) + ")");
        };
        proc(774, at(19, 22, 47, 11), 0, "systemd", "", "/usr/lib/systemd/systemd --user", "init.scope", 1, "systemd", 19, 1e7);
        proc(1168789, at(21, 20, 23, 0), T(10, 11, 49), "fish", "/usr/bin/fish", "fish", tab, 2194, "gnome-terminal-server", 0.2, 1e7);
        proc(1168842, at(21, 20, 24, 1), T(10, 11, 49), "claude", "/home/user/.local/share/claude/versions/2.1.307",
             "claude --resume", tab, 1168789, "fish", 1075, 7e8);
        proc(1410499, T(9, 57, 28), T(10, 11, 49), "claude", "/home/user/.local/share/claude/versions/2.1.308", "claude --resume", tab,
             1168789, "fish", 124, 7e8);
        proc(1497737, T(13, 7, 40), 0, "claude", "/home/user/.local/share/claude/versions/2.1.308", "claude --resume",
             "vte-spawn-1.scope", 1497700, "fish", 2000, 7e8);
        proc(1372352, T(9, 0, 0), T(10, 11, 49), "exe", "", "unllm --serve", "app-unllm.service", 1, "systemd", 700, 3e9);
        proc(1276284, T(7, 23, 40), T(10, 53, 53), "ollama", "/usr/local/bin/ollama", "/usr/local/bin/ollama serve", "ollama.service", 1,
             "systemd", 699, 6e8);
        proc(1376606, T(9, 0, 0), T(9, 59, 59), "llama-server", "/usr/local/lib/ollama/llama-server",
             "/usr/local/lib/ollama/llama-server --model m", "ollama.service", 1276284, "ollama", 480, 5.6e9);
        proc(1455323, T(10, 53, 56), T(16, 17, 40), "ollama", "/usr/local/bin/ollama", "/usr/local/bin/ollama serve", "ollama.service", 1,
             "systemd", 50, 6e8);  // the twin row: not a stop
        proc(1455323, T(10, 53, 58), 0, "ollama", "/usr/local/bin/ollama", "/usr/local/bin/ollama serve", "ollama.service", 1, "systemd",
             1900, 6e8);
        proc(1200000, at(21, 22, 0, 0), T(7, 23, 31), "ollama", "/usr/local/bin/ollama", "/usr/local/bin/ollama serve", "ollama.service", 1,
             "systemd", 400, 6e8);
        proc(1237638, T(6, 50, 0), T(7, 23, 31), "llama-server", "/usr/local/lib/ollama/llama-server",
             "/usr/local/lib/ollama/llama-server --model m", "ollama.service", 1200000, "ollama", 300, 5.98e9);
        for (int i = 0; i < 3; ++i)
            proc(1250000 + i, T(7, 10 + 10 * i, 0), T(7, 12 + 10 * i, 0), "unllm", "/home/user/repo-work/unllm/build/unllm",
                 "unllm --script q.txt", tab, 1168842, "claude", 40, 3.1e9);
        proc(1700000, T(19, 10, 0), T(19, 50, 0), "clangd", "/usr/bin/clangd", "clangd --background-index", tab, 1497737, "claude", 30, 9.1e8);
        proc(1260000, T(7, 26, 0), T(7, 27, 0), "firefox", "/usr/lib/firefox/firefox", "/usr/lib/firefox/firefox", "app-firefox@1.service",
             1, "systemd", 5, 4e8);
        proc(1260100, T(8, 31, 50), T(8, 32, 50), "firefox", "/usr/lib/firefox/firefox", "/usr/lib/firefox/firefox",
             "app-firefox@2.service", 1, "systemd", 5, 4e8);
        proc(1260200, T(10, 36, 43), T(10, 37, 6), "firefox", "/usr/lib/firefox/firefox", "/usr/lib/firefox/firefox",
             "app-firefox@3.service", 1, "systemd", 5, 4e8);
        proc(1846301, T(21, 37, 17), T(22, 56, 25), "haruna", "/usr/bin/haruna", "/usr/bin/haruna movie.mp4", "app-org.kde.haruna@1.service",
             1, "systemd", 441, 4.6e8);
        proc(1530, at(19, 22, 47, 30), T(13, 7, 0), "kwin_wayland", "", "/usr/bin/kwin_wayland --wayland-fd 7", "plasma-kwin_wayland.service",
             1524, "kwin_wayland_wrapper", 10551, 2.6e8);
        proc(1497050, T(13, 7, 28), 0, "kwin_wayland", "", "/usr/bin/kwin_wayland --wayland-fd 7", "plasma-kwin_wayland.service", 1497045,
             "kwin_wayland_wrapper", 904, 2.6e8);
        proc(1300000, T(6, 46, 0), 0, "python3", "/usr/bin/python3.14", "python3 /home/user/repo-work/unllm/tools/progress_tray.py",
             "app-progress.service", 1, "systemd", 263, 5e7);
        proc(1005431, at(21, 17, 20, 45), 0, "node-MainThread", "/usr/bin/node", "node realm-companion.js",
             "app-realm\\x2dos@0d4a935caa294f50b76112e83ca0b973.service", 774, "systemd", 227, 1e8);
        proc(1600000, T(13, 7, 50), 0, "node-MainThread", "/usr/bin/node", "node /usr/lib/node_modules/pyright/langserver.index.js --stdio",
             "vte-spawn-1.scope", 1497737, "claude", 30, 2e8);
        for (int i = 0; i < 39; ++i) {  // weird, every five minutes or so from 18:17 to 21:39
            const int64_t s = T(18, 17) + i * 319;
            proc(1800000 + i, s, s + 30, "python3", "/usr/bin/python3.14",
                 "python3 /home/user/.local/bin/weird --offline --no-prompt /usr/local/bin/ollama", "run-p1-i1.scope", 1799999, "sh", 20, 1.4e8);
            minutes(s - s % 60, s - s % 60 + 60, 1800000 + i, "python3",
                    "python3 /home/user/.local/bin/weird --offline --no-prompt /usr/local/bin/ollama", "run-p1-i1.scope", 20);
        }
        exec("INSERT INTO log(t, category, title, detail) VALUES"
             "(" + num(double(T(7, 23, 31))) + ",'services','ollama.service failed: Failed with result ''oom-kill''.',''),"
             "(" + num(double(T(10, 11, 49))) + ",'services','" + tab + " failed: Failed with result ''oom-kill''.',''),"
             "(" + num(double(T(10, 53, 53))) + ",'services','ollama.service failed: Failed with result ''oom-kill''.',''),"
             "(" + num(double(T(11, 7, 40))) + ",'session','Lid closed',''),(" + num(double(T(11, 7, 41))) + ",'session','Went to sleep',''),"
             "(" + num(double(T(6, 45, 25))) + ",'session','Lid opened',''),(" + num(double(T(6, 45, 28))) + ",'session','Woke from sleep','')");
        exec("COMMIT");
        sqlite3_close(db);
    }
    // The live processes for "that node thing": the dashboard companion since Wednesday, a language server since 13:07.
    Tree t;
    const int64_t boot = at(19, 22, 47, 0);
    t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime " + std::to_string(boot) + "\n");
    const long hz = sysconf(_SC_CLK_TCK);
    t.write("proc/1005431/stat", stat_line(1005431, "node-MainThread", 'S', 774, uint64_t(227 * hz), 0,
                                           uint64_t((at(21, 17, 20, 45) - boot) * hz), 100));
    t.write("proc/1005431/cmdline", std::string("node\0realm-companion.js\0", 24));
    t.write("proc/1005431/cgroup", "0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                                   "app-realm\\x2dos@0d4a935caa294f50b76112e83ca0b973.service\n");
    symlink("/usr/bin/node", (t.root + "/proc/1005431/exe").c_str());
    t.write("proc/1600000/stat", stat_line(1600000, "node-MainThread", 'S', 1497737, uint64_t(30 * hz), 0,
                                           uint64_t((T(13, 7, 50) - boot) * hz), 100));
    t.write("proc/1600000/cmdline", std::string("node\0/usr/lib/node_modules/pyright/langserver.index.js\0--stdio\0", 63));
    t.write("proc/1600000/cgroup", "0::/user.slice/user-1000.slice/user@1000.service/app.slice/vte-spawn-1.scope\n");
    symlink("/usr/bin/node", (t.root + "/proc/1600000/exe").c_str());

    char exe[4096];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string bin = len > 0 ? std::string(exe, std::size_t(len)) : std::string("./ascore_tests");
    const std::string cache = bin.substr(0, bin.rfind('/')) + "/kinds-cache";
    fs::create_directories(cache);
    // Each question on its own, as the testbed asks them: no conversation carries a program over.
    auto asked = [&](const char* q) {
        as::Ask ask;
        CHECK(ask.open(dbp));
        ask.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin", "vendor/unllm/data/banter.txt",
                          "vendor/unllm/data/rewrites.txt");
        ask.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        ask.load_facts({"facts/base.tsv"});
        CHECK(ask.load_kinds("data/kinds", cache, model_dir));
        ask.set_roots(t.roots());
        as::AskResult r = ask.ask(q, now);
        std::fprintf(stderr, "  [%s] %s -> %s (%s)\n", r.branch.c_str(), q, r.answer.c_str(), r.span.c_str());
        return r;
    };
    auto said = [&](const char* q) { return asked(q).answer; };
    auto has = [](const std::string& a, const char* s) { return a.find(s) != std::string::npos; };

    // 1, Q3: the disk's own measure (bytes moved), only the part of an event inside the window, never the unseen rows.
    {
        const std::string a = said("something was thrashing the drive yesterday morning, what was it");
        CHECK(has(a, "2.1 GB/s") && !has(a, "2.5 GB/s"));
        CHECK(has(a, "Claude") && !has(a, "ystemd"));
        CHECK(has(a, "read") && has(a, "wrote"));
    }
    // 2, Q15: the GPU's users by video memory, in the half hour around 9:20.
    {
        const std::string a = said("what was using the gpu around 9:20 yesterday");
        CHECK(has(a, "lama-server") && has(a, "4.0 GB"));
        CHECK(a.find("lama-server") < (has(a, "exe") ? a.find("exe") : a.size()));
        CHECK(!has(a, "100%"));
    }
    // 3, Q16: two windows compared, peak and typical each.
    {
        const std::string a = said("was it running hotter yesterday morning or last night");
        CHECK(!has(a, "can't say"));
        CHECK(has(a, "95 C") && has(a, "73 C") && has(a, "average"));
        CHECK(has(a, "Thu 05:00") && a.find("Thu 05:00") < a.find("Thu 18:00"));  // the morning named as the hotter one first
    }
    // 4, Q17: per-minute CPU for each, Ollama with its runners; the lump the machine could not have used is not claude's.
    {
        const std::string a = said("did claude use more cpu than ollama yesterday");
        CHECK(a.rfind("Yes", 0) == 0);
        CHECK(has(a, "1,260") && has(a, "lama-server"));
    }
    // 5, Q18: the fan's peak moments, and whether the program ran at them.
    {
        const std::string no = said("was firefox open when the fan hit its peak yesterday");
        CHECK(no.rfind("No", 0) == 0 && has(no, "85%") && has(no, "09:21") && has(no, "10:08"));
        const std::string yes = said("was claude open when the fan hit its peak yesterday");
        CHECK(yes.rfind("Yes", 0) == 0);
    }
    // 6, Q19: memory ranked by memory in the hour asked (right since the time fix: a guard).
    CHECK(has(said("what was the biggest memory hog between 7 and 8 yesterday morning"), "lama-server used the most memory"));
    // 7, Q21: the kill placed from the records, then the process that came back after it.
    {
        const std::string a = said("is ollama still running since it got killed yesterday");
        CHECK(a.rfind("Yes", 0) == 0 && has(a, "1455323") && has(a, "10:53"));
    }
    // 8, Q22: the program's run spans are the window; how hard is the average, with the peak.
    {
        const std::string a = said("how hard was the cpu working while i watched haruna last night");
        CHECK(has(a, "average") && has(a, "28.5%") && !has(a, "47%"));
        const std::string m = said("how hard was the cpu working while i watched the movie last night");
        CHECK(!has(m, "47%") || has(m, "movie"));
        CHECK(has(m, "average"));
        // A clause that names no program (T29: no game ran) says so once, then answers for the whole window; where the
        // per-second readings don't reach the window's start, from the events too, so the morning's 95 C counts.
        for (const char* q : {"did it overheat while gaming yesterday", "did the cpu overheat while i was gaming yesterday"}) {
            const std::string g = said(q);
            CHECK(has(g, "couldn't tell which program") && !has(g, "\"gaming\" and \"gaming\"") &&
                  !has(g, "(I tried") && has(g, "95 C") && has(g, "whole"));
        }
    }
    // 9, Q23: the half hour before the tab's kill at 10:11:49; claude first, never systemd's booked lump.
    {
        as::AskResult r = asked("what was eating the cpu in the half hour before the terminal got killed yesterday morning");
        CHECK(r.span == "Thu 09:41 to Thu 10:11");
        CHECK(has(r.answer, "Claude") && !has(r.answer, "ystemd"));
        CHECK(r.answer.find("Claude") < r.answer.find("exe"));
        // With no length named, the span before the lid closed starts when it last woke (Q35's shape).
        CHECK(asked("what was the laptop up to yesterday before i shut the lid").span == "Thu 06:45 to Thu 11:07");
    }
    // 10, Q14 and Q45: the program's CPU seconds in the window, its runners counted with it; never "the main cause".
    {
        const std::string a = said("how much cpu did ollama use between 9 and 10 yesterday morning");
        CHECK(has(a, "1,260") && !has(a, "main cause"));
        const std::string k = said("how much cpu did kwin_wayland use yesterday");
        CHECK(has(k, "1,800") && !has(k, "10,551") && !has(k, "main cause"));
    }
    // 11, Q44: the interpreter's script named, and when it ran.
    {
        const as::AskResult r = asked("whats the python thing that keeps running in the evenings");
        CHECK(has(r.answer, "weird") && has(r.answer, "18:17"));
        // "in the evenings" is a habit: the last whole evening, with no note that this evening hasn't started.
        CHECK(!has(r.answer, "hasn't started") && r.span == "Thu 18:00 to Thu 23:00");
        CHECK(has(said("whats the python thing that was running in the evening"), "hasn't started"));
    }
    // 12, Q50: what the live processes are, not only their CPU share.
    {
        const std::string a = said("whats that node thing running in the background");
        CHECK(has(a, "realm-companion.js") && has(a, "Wed 17:20") && has(a, "app-realm-os"));
    }
    fs::remove_all(dir);
}

// Three answers' shapes, on rows shaped like the records copied on 2026-10-23 (generic names and paths), at the clock of
// the 50-question test: RAM and swap set side by side (Q2), the story of an afternoon told in moments without the
// rows that recur every day (Q10), and what startup gained in a day, without the temporary files (Q47).
void test_answer_shapes() {
    std::puts("answer shapes: either/or memory, story moments, startup additions");
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model_dir = em ? em : std::string(home ? home : "") + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model_dir + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: answer shape checks skipped");
        return;
    }
    const std::string dir = fs::temp_directory_path() / ("as-shapes-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const std::string dbp = dir + "/h.db";
    auto at = [](int day, int h, int m, int s) {
        std::tm tm{};
        tm.tm_year = 2026 - 1900; tm.tm_mon = 9; tm.tm_mday = day; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s; tm.tm_isdst = -1;
        return int64_t(std::mktime(&tm));
    };
    auto T = [&](int h, int m, int s = 0) { return at(22, h, m, s); };  // Thursday, "yesterday"
    const int64_t now = at(23, 0, 1, 30);                                 // the clock of the 50-question test
    const double cap = 8283222016;                                        // the zram swap's size, to the byte
    { as::Store st(dbp); }
    {
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        auto exec = [&](const std::string& sql) {
            char* err = nullptr;
            const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
            if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
            CHECK(ok);
        };
        exec("BEGIN");
        // Readings once a second, without the swap's size (rows from before it was kept).
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO samples(t, cpu, mem_pct, swap_used, pressure) VALUES (?, 20, ?, ?, ?)", -1, &st, nullptr);
        auto sample = [&](int64_t t, double mem, double swap, double pressure) {
            sqlite3_bind_int64(st, 1, t);
            sqlite3_bind_double(st, 2, mem);
            sqlite3_bind_double(st, 3, swap);
            sqlite3_bind_double(st, 4, pressure);
            sqlite3_step(st);
            sqlite3_reset(st);
        };
        // Q2: the late morning until the lid closed. RAM climbs to 89.9% at 10:53:51; the swap sits at its cap from
        // 10:01:26 to 10:53:53; programs wait on memory from 10:30, up to 14.9% at 10:50.
        for (int64_t t = T(10, 0); t < T(11, 7, 41); ++t)
            sample(t, t == T(10, 53, 51) ? 89.9 : 60 + 25.0 * double(t - T(10, 0)) / 4000.0,
                   t >= T(10, 1, 26) && t <= T(10, 53, 53) ? cap : 6e9,
                   t == T(10, 50) ? 14.9 : t >= T(10, 30) && t <= T(10, 53, 53) ? 4 : 0.5);
        // The afternoon: the swap full again for a while, nobody waiting; the evening the same, RAM at half.
        for (int64_t t = T(16, 0); t < T(16, 17, 32); ++t) sample(t, 55, t >= T(16, 5) ? cap : 7.9e9, 0.5);
        for (int64_t t = T(18, 0); t < T(18, 30); ++t) sample(t, 50, cap, 0.3);
        sqlite3_finalize(st);
        auto row = [&](int64_t t, const char* cat, const std::string& title, const std::string& detail = "") {
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(db, "INSERT INTO log(t, category, title, detail) VALUES (?, ?, ?, ?)", -1, &q, nullptr);
            sqlite3_bind_int64(q, 1, t);
            sqlite3_bind_text(q, 2, cat, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 3, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 4, detail.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        };
        auto failed = [&](int64_t t, const std::string& unit, const std::string& result) {
            const std::string cut = unit.find(':') == std::string::npos ? unit : unit.substr(0, unit.find(':'));
            const std::string rest = unit.find(':') == std::string::npos ? "" : unit.substr(unit.find(':') + 1) + ": ";
            row(t, "services", cut + " failed: " + rest + "Failed with result '" + result + "'.",
                "systemd: " + unit + ": Failed with result '" + result + "'.");
        };
        const std::string bin = "/home/user/.local/bin/", units = "/home/user/.config/systemd/user/";
        // Two weeks before: what recurs nearly every day (Wi-Fi rewriting resolv.conf, a program updating itself in
        // ~/.local/bin, sleeping and waking, the charger) and a crash on two days.
        for (int d = 9; d <= 21; ++d) {
            row(at(d, 8, 0, 0), "startup", "System file added: /etc/resolv.conf");
            if (d % 4 != 0) {
                row(at(d, 18, 0, 0), "startup", "Program in ~/.local/bin added: " + bin + "claude", "Link to /home/user/.local/share/claude/versions/2.1.300\n");
                row(at(d, 18, 0, 0), "startup", "Program in ~/.local/bin removed: " + bin + "claude.tmp." + std::to_string(1000 + d) + ".1792600000000");
            }
            row(at(d, 7, 0, 0), "session", "Woke from sleep");
            row(at(d, 9, 0, 0), "session", "Unplugged from power at 90% battery");
            row(at(d, 10, 0, 0), "session", "Plugged into power at 60% battery");
            row(at(d, 23, 0, 0), "session", "Went to sleep");
        }
        row(at(19, 18, 3, 14), "problems", "ktorrent crashed");
        row(at(21, 20, 26, 6), "problems", "ktorrent crashed");
        // Q47: Thursday morning's startup rows, as the old watcher wrote them: sed's temporary files show only as removed,
        // in the same second as the file they were renamed onto; claude's updater writes claude.tmp.* and renames it.
        row(T(6, 45, 41), "startup", "System file added: /etc/resolv.conf");
        row(T(7, 3, 29), "startup", "Program in ~/.local/bin added: " + bin + "claude", "Link to /home/user/.local/share/claude/versions/2.1.308\n");
        row(T(7, 3, 29), "startup", "Program in ~/.local/bin removed: " + bin + "claude.tmp.1168842.1792667009308");
        row(T(8, 4, 11), "startup", "Your background service added: " + units + "model-warm.service",
            "+ Description=Preload the reader model\n+ ExecStart=/usr/bin/curl -sf http://127.0.0.1:11434/api/generate\n");
        row(T(8, 4, 11), "startup", "Your background service added: " + units + "model-warm.service.bak-2026-10-22",
            "+ [Unit]\n+ Description=Preload the old model\n");
        row(T(8, 4, 11), "startup", "Your background service removed: " + units + "sedS3jmuZ");
        row(T(8, 44, 33), "startup", "Program in ~/.local/bin added: " + bin + "unllm",
            "Link to /home/user/repo-work/unllm/build/unllm\nThe file can be run as a program.\n");
        row(T(8, 57, 39), "startup", "Your background service added: " + units + "model-warm.service", "+ ExecStart=/usr/bin/curl -sf\n");
        row(T(8, 57, 39), "startup", "Your background service removed: " + units + "sedrGLO0y");
        // The watcher since round 2 writes "changed" for a file written over; one never seen before is no addition.
        row(T(9, 30, 0), "startup", "Your background service changed: " + units + "old-sync.service", "+ ExecStart=/usr/bin/true\n");
        // Q2: the two kills for memory before lunch.
        failed(T(10, 11, 49), "vte-spawn-6a3f4d97-f7ac-4414-bec4-39196307af6b.scope", "oom-kill");
        failed(T(10, 53, 53), "ollama.service", "oom-kill");
        row(T(11, 7, 40), "session", "Lid closed");
        row(T(11, 7, 41), "session", "Went to sleep");
        // Q10: Thursday afternoon's log, as recorded.
        row(T(12, 26, 13), "session", "Lid opened");
        row(T(12, 26, 15), "session", "Woke from sleep");
        row(T(12, 26, 26), "startup", "System file added: /etc/resolv.conf");
        row(T(13, 7, 11), "session", "Power button pressed");
        failed(T(13, 7, 20), "dbus-:1.2-org.kde.kdeconnect@0.service", "exit-code");
        failed(T(13, 7, 20), "dbus-:1.2-org.kde.kwalletd6@0.service", "exit-code");
        row(T(13, 7, 21), "session", "Logged out (Removed session 3.)");
        row(T(13, 7, 21), "session", "Logged in: 'greeter' with class 'greeter' and type 'wayland'.");
        row(T(13, 7, 21), "session", "Logged in: 'greeter' with class 'manager-early' and type 'unspecified'.");
        row(T(13, 7, 27), "session", "Logged in: 'user' with class 'user' and type 'wayland'.");
        row(T(13, 7, 32), "session", "Logged out (Removed session c2.)");
        row(T(13, 7, 43), "session", "Logged out (Removed session 4.)");
        for (const auto& [t, prog] : std::vector<std::pair<int64_t, const char*>>{
                 {T(14, 35, 6), "ascore_tests"}, {T(14, 35, 12), "ask"}, {T(14, 38, 20), "ascore_tests"},
                 {T(14, 38, 27), "ask"}, {T(14, 40, 25), "ascore_tests"}, {T(14, 40, 30), "ask"}, {T(22, 56, 30), "ktorrent"}})
            row(t, "problems", std::string(prog) + " crashed");
        row(T(14, 38, 32), "problems", "ask crashed 2 times between 14:38:27 and 14:38:32");
        row(T(14, 54, 43), "session", "Lid closed");
        row(T(14, 54, 44), "startup", "System file added: /etc/resolv.conf");
        row(T(14, 54, 44), "session", "Lid opened");
        row(T(14, 54, 45), "session", "Went to sleep");
        row(T(14, 54, 58), "session", "Woke from sleep");
        row(T(14, 55, 5), "startup", "System file added: /etc/resolv.conf");
        for (int i = 0; i < 6; ++i) failed(T(15, 49, 40) + i * 1200, "dbus-:1.2-org.kde.KSplash@" + std::to_string(i + 2) + ".service", "exit-code");
        for (const int64_t t : {T(18, 44, 17), T(18, 45, 30)}) {
            row(t, "startup", "Program in ~/.local/bin added: " + bin + "claude", "Link to /home/user/.local/share/claude/versions/2.1.309\n");
            row(t, "startup", "Program in ~/.local/bin removed: " + bin + "claude.tmp." + std::to_string(t) + ".1792709056263");
        }
        row(T(23, 29, 59), "session", "Unplugged from power at 100% battery");
        row(T(23, 46, 21), "session", "Plugged into power at 88% battery");
        // The desktop shell: runs of days before, then started again eleven times on Thursday afternoon, each run as the
        // one before ended. Ollama's model runner starts again every 20 s all the time; that is its normal life.
        auto proc = [&](int64_t pid, int64_t started, int64_t ended, const char* name, const char* unit) {
            exec("INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES (" +
                 std::to_string(pid) + "," + std::to_string(started) + "," + (ended ? std::to_string(ended) : std::string("NULL")) + ",'" +
                 name + "','/usr/bin/" + name + "','" + name + "','user','" + unit + "',774,'systemd',10,1e8)");
        };
        proc(1536, at(16, 17, 35, 8), at(18, 22, 6, 32), "plasmashell", "plasma-plasmashell.service");
        proc(2065, at(19, 7, 21, 48), at(19, 22, 44, 45), "plasmashell", "plasma-plasmashell.service");
        proc(1692, at(19, 22, 47, 22), T(13, 7, 16), "plasmashell", "plasma-plasmashell.service");
        const int64_t shell[] = {T(13, 7, 29), T(15, 48, 38), T(15, 49, 16), T(16, 32, 9), T(16, 38, 51), T(16, 39, 16),
                                 T(17, 45, 33), T(17, 50, 16), T(17, 51, 1), T(17, 51, 26), T(17, 51, 46)};
        for (int i = 0; i < 11; ++i) proc(1497162 + i, shell[i], i < 10 ? shell[i + 1] + 2 : 0, "plasmashell", "plasma-plasmashell.service");
        for (int i = 0; i < 40; ++i) {
            proc(900000 + i, at(21, 9, 0, 0) + i * 20, at(21, 9, 0, 0) + i * 20 + 19, "llama-server", "ollama.service");
            proc(910000 + i, T(13, 0) + i * 20, T(13, 0) + i * 20 + 19, "llama-server", "ollama.service");
        }
        // Q2: the biggest holders of memory before lunch, from the per-minute lists.
        exec("INSERT INTO minute_top(minute, pid, start_ticks, name, cmdline, unit, cpu_seconds, peak_rss, io_bytes, exited) VALUES "
             "(" + std::to_string(T(10, 5)) + ", 1413056, 0, 'llama-server', '/usr/local/bin/ollama runner', 'ollama.service', 30, 7.52e9, 0, 0),"
             "(" + std::to_string(T(10, 40)) + ", 1444639, 0, 'exe', 'unllm --serve', 'vte-spawn-1.scope', 40, 4.8e9, 0, 0)");
        exec("COMMIT");
        sqlite3_close(db);
    }
    // The live machine: 16.6 GB of RAM, all of its swap one zram device of exactly the size the readings peaked at.
    Tree t;
    t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
    t.write("proc/meminfo", "MemTotal: 16178404 kB\nMemAvailable: 8000000 kB\nSwapTotal: 8089084 kB\nSwapFree: 84432 kB\n");
    t.write("proc/swaps", "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n/dev/zram0                              partition\t8089084\t\t8004652\t\t100\n");
    char exe[4096];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string binary = len > 0 ? std::string(exe, std::size_t(len)) : std::string("./ascore_tests");
    const std::string cache = binary.substr(0, binary.rfind('/')) + "/kinds-cache";
    fs::create_directories(cache);
    // Each question on its own; "" lets the catalogue choose the kind.
    auto ask_as = [&](const std::string& kind, const char* q) {
        as::Ask a;
        CHECK(a.open(dbp));
        a.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin", "vendor/unllm/data/banter.txt",
                        "vendor/unllm/data/rewrites.txt");
        a.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        a.load_facts({"facts/base.tsv"});
        CHECK(a.load_kinds("data/kinds", cache, model_dir));
        a.set_roots(t.roots());
        a.force_reading(kind);
        return a.ask(q, now);
    };
    auto has = [](const as::AskResult& r, const char* s) {
        const bool ok = r.answer.find(s) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  no \"%s\" in [%s] %s\n", s, r.branch.c_str(), r.answer.c_str());
        return ok;
    };
    auto lacks = [](const as::AskResult& r, const char* s) {
        const bool ok = r.answer.find(s) == std::string::npos;
        if (!ok) std::fprintf(stderr, "  \"%s\" in [%s] %s\n", s, r.branch.c_str(), r.answer.c_str());
        return ok;
    };

    // Q2: "the ram or the swap" are both memory: each is answered from the readings, and the one at its limit named.
    // Here both were: the swap at its cap while programs waited on memory.
    {
        as::AskResult r = ask_as("", "the laptop was crawling right before lunch yesterday, was it the ram or the swap thats the problem");
        CHECK(has(r, "Both") && has(r, "89.9%") && has(r, "16.6 GB") && has(r, "8.3 GB of 8.3 GB") && has(r, "14.9%"));
        CHECK(has(r, "10:53") && has(r, "ollama.service") && has(r, "llama-server"));
        CHECK(lacks(r, "main cause"));
        // The evening: the swap full, nobody waiting on memory, RAM at half: the swap.
        as::AskResult e = ask_as("", "was it the ram or the swap that was full yesterday evening");
        CHECK(has(e, "The swap") && lacks(e, "Both") && has(e, "50"));
        // Two things that are not memory measures keep their usual answer.
        CHECK(ask_as("", "was it the cpu or the gpu that got hot yesterday morning").branch.rfind("choice:memory", 0) != 0);
    }
    // Q10: the story in moments: the power button, the logout and the logins as one; the shell's restarts and the full
    // swap added; rows that recur nearly every day (resolv.conf, the updater in ~/.local/bin) left out.
    {
        as::AskResult r = ask_as("diagnose.list.window", "anything weird happen since lunch yesterday");
        CHECK(has(r, "ower button pressed, then") && has(r, "logged out and in") && lacks(r, "Logged in:") && lacks(r, "greeter"));
        CHECK(has(r, "kdeconnect@0.service and org.kde.kwalletd6@0.service failed"));
        CHECK(has(r, "plasmashell restarted 11 times") && lacks(r, "llama-server"));
        CHECK(has(r, "swap") && has(r, "full"));
        CHECK(lacks(r, "resolv.conf") && lacks(r, "claude"));
        CHECK(has(r, "ascore_tests crashed 3 times") && has(r, "ask crashed 4 times") && has(r, "ktorrent crashed"));
        CHECK(has(r, "KSplash@2-7.service failed (exit-code) 6 times") && has(r, "went to sleep"));
    }
    // Q47: what was added: the new files only, each with what it is from its place; a file replaced in place is not new;
    // the temporary files sed and the updater renamed onto other files are no additions at all.
    {
        as::AskResult r = ask_as("startup.list.window", "what new startup stuff got added yesterday");
        CHECK(has(r, "model-warm.service") && has(r, "08:04") && has(r, "08:57") && has(r, "background service"));
        CHECK(has(r, "unllm") && has(r, "08:44") && has(r, "~/.local/bin"));
        CHECK(has(r, "copy") && has(r, "model-warm.service.bak-2026-10-22"));
        CHECK(lacks(r, "sedS3jmuZ") && lacks(r, "sedrGLO0y") && lacks(r, "claude.tmp"));
        CHECK(has(r, "claude") && has(r, "three times"));
        CHECK(lacks(r, "startup changes were recorded") && lacks(r, "resolv.conf added"));
        CHECK(lacks(r, "old-sync.service (") && has(r, "old-sync.service once"));
        CHECK(ask_as("startup.list.window", "did anything new get added to startup yesterday").answer.rfind("Yes", 0) == 0);
        // Nothing was removed that day: the temporary files' rows are not removals.
        as::AskResult gone = ask_as("startup.list.window", "what startup stuff got removed yesterday");
        CHECK(lacks(gone, "sedS3jmuZ") && lacks(gone, "claude.tmp"));
    }
    fs::remove_all(dir);
}

// The new 50's catalogue faults, against rows shaped like the records copied on 2026-10-23 at 13:58: the session log
// (log-ins with the login screen's own sessions, the lid, the charger), the admin log (one sudo refused), first runs,
// first connections, process counts, a quiet moment in the night, Monday evening's fan in the hourly records only.
void test_catalogue_kinds() {
    std::puts("catalogue kinds: session, admin, first runs and connections, process counts, a moment, the fan, missing data");
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model_dir = em ? em : std::string(home ? home : "") + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model_dir + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: catalogue kind checks skipped");
        return;
    }
    const std::string dir = fs::temp_directory_path() / ("as-catkinds-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const std::string dbp = dir + "/h.db";
    auto at = [](int day, int h, int m, int s) {
        std::tm tm{};
        tm.tm_year = 2026 - 1900; tm.tm_mon = 9; tm.tm_mday = day; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s; tm.tm_isdst = -1;
        return int64_t(std::mktime(&tm));
    };
    const int64_t now = at(23, 13, 59, 8);  // the clock of the new 50
    { as::Store st(dbp); }
    {
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        auto exec = [&](const std::string& sql) {
            char* err = nullptr;
            const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
            if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
            CHECK(ok);
        };
        exec("BEGIN");
        auto row = [&](int64_t t, const char* cat, const std::string& title, const std::string& detail = "") {
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(db, "INSERT INTO log(t, category, title, detail) VALUES (?, ?, ?, ?)", -1, &q, nullptr);
            sqlite3_bind_int64(q, 1, t);
            sqlite3_bind_text(q, 2, cat, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 3, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 4, detail.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        };
        // Log-ins as logind reports them: at each boot the user's service manager, the login screen's own sessions,
        // then the person's desktop session (class 'user'); the login screen's sessions end a few seconds later.
        auto logged_in = [&](int64_t t, const std::string& who, const std::string& id, const std::string& cls, const std::string& type) {
            row(t, "session", "Logged in: '" + who + "' with class '" + cls + "' and type '" + type + "'.",
                "systemd-logind: New session '" + id + "' of user '" + who + "' with class '" + cls + "' and type '" + type + "'.");
        };
        auto logged_out = [&](int64_t t, const std::string& id) {
            row(t, "session", "Logged out (Removed session " + id + ".)", "systemd-logind: Removed session " + id + ".");
        };
        auto boot_login = [&](int day, int h, int m, int s) {
            const int64_t b = at(day, h, m, s);
            logged_in(b, "user", "1", "manager", "unspecified");
            logged_in(b + 3, "greeter", "c1", "greeter", "wayland");
            logged_in(b + 3, "greeter", "2", "manager-early", "unspecified");
            logged_in(b + 9, "user", "3", "user", "wayland");
            logged_out(b + 14, "c1");
            logged_out(b + 25, "2");
        };
        boot_login(16, 17, 34, 58);
        boot_login(19, 7, 21, 38);
        boot_login(19, 22, 47, 12);
        // Thursday 13:07: the power button ended the session; the login screen came up and the person logged in again.
        row(at(22, 13, 7, 11), "session", "Power button pressed", "systemd-logind: Power key pressed short.");
        logged_out(at(22, 13, 7, 21), "3");
        logged_in(at(22, 13, 7, 21), "greeter", "c2", "greeter", "wayland");
        logged_in(at(22, 13, 7, 21), "greeter", "4", "manager-early", "unspecified");
        logged_in(at(22, 13, 7, 27), "user", "5", "user", "wayland");
        logged_out(at(22, 13, 7, 32), "c2");
        logged_out(at(22, 13, 7, 43), "4");
        // The lid, each close followed by sleep: two last week, four this week (the last opened again a second later).
        auto lid = [&](int64_t closed, int64_t opened) {
            row(closed, "session", "Lid closed", "systemd-logind: Lid closed.");
            row(closed + 2, "session", "Went to sleep", "systemd-sleep: Performing sleep operation 'suspend'...");
            row(opened, "session", "Lid opened", "systemd-logind: Lid opened.");
            row(opened + 3, "session", "Woke from sleep", "systemd-sleep: System returned from sleep operation 'suspend'.");
        };
        lid(at(16, 22, 3, 56), at(17, 8, 59, 58));
        lid(at(17, 19, 9, 10), at(17, 21, 0, 0));
        lid(at(21, 0, 10, 11), at(21, 17, 18, 58));
        lid(at(21, 21, 36, 15), at(22, 6, 45, 25));
        lid(at(22, 11, 7, 40), at(22, 12, 26, 13));
        lid(at(22, 14, 54, 43), at(22, 14, 54, 44));
        // The charger, as LogWatch writes it.
        row(at(21, 20, 17, 9), "session", "Unplugged from power at 98% battery");
        row(at(21, 21, 16, 5), "session", "Plugged into power at 36% battery");
        row(at(22, 8, 37, 49), "session", "Unplugged from power at 98% battery");
        row(at(22, 9, 7, 16), "session", "Plugged into power at 75% battery");
        row(at(22, 23, 29, 59), "session", "Unplugged from power at 100% battery");
        row(at(22, 23, 46, 21), "session", "Plugged into power at 88% battery");
        row(at(23, 9, 11, 35), "session", "Unplugged from power at 100% battery");
        row(at(23, 10, 7, 38), "session", "Plugged into power at 56% battery");
        // sudo, as the logbook writes it; a refused one is titled like the others, its detail says why.
        auto sudo = [&](int64_t t, const std::string& cmd, const std::string& fields) {
            row(t, "admin", "Ran as administrator (sudo) by user: " + cmd, "sudo:  user : " + fields + " ; USER=root ; COMMAND=" + cmd);
        };
        sudo(at(16, 14, 21, 56), "/usr/bin/true", "PWD=/home/user");
        sudo(at(18, 17, 19, 40), "/usr/bin/true", "a password is required ; PWD=/home/user/projects/bench");
        sudo(at(19, 13, 26, 8), "/usr/bin/true", "a password is required ; PWD=/home/user/projects/bench");
        sudo(at(19, 13, 26, 34), "/usr/bin/sh -c 'echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo'", "PWD=/home/user/projects/bench");
        sudo(at(21, 20, 38, 4), "/usr/bin/pacman -S onnxruntime-cuda sentencepiece", "TTY=pts/1 ; PWD=/home/user");
        sudo(at(21, 20, 40, 40), "/usr/bin/pacman -Syy onnxruntime-cuda sentencepiece", "TTY=pts/1 ; PWD=/home/user");
        // Units that failed this week and one last week, as the logbook writes them, and a program's crash.
        auto failed = [&](int64_t t, const std::string& unit, const std::string& result) {
            row(t, "services", unit + " failed: Failed with result '" + result + "'.", "systemd: " + unit + ": Failed with result '" + result + "'.");
        };
        failed(at(16, 10, 0, 0), "oldjob.service", "exit-code");
        failed(at(22, 7, 23, 31), "ollama.service", "oom-kill");
        failed(at(23, 8, 13, 4), "backup.service", "exit-code");
        row(at(23, 10, 47, 10), "problems", "player crashed");
        // First runs of programs outside /usr, /opt and /snap: builds, new versions of a self-updating tool, tests.
        auto first = [&](int64_t t, const std::string& exe) {
            row(t, "programs", "First run of " + exe, "Command: " + exe + "\nUser: user\nStarted by: zsh");
        };
        first(at(16, 15, 0, 0), "/home/user/projects/tool/build/tool_old");
        first(at(19, 15, 18, 40), "/home/user/projects/tool/build/tool_meaning");
        first(at(19, 19, 42, 3), "/home/user/.local/share/app/versions/2.1.304");
        first(at(20, 17, 47, 16), "/home/user/.local/share/app/versions/2.1.305");
        first(at(21, 17, 21, 9), "/home/user/.local/share/app/versions/2.1.306");
        first(at(22, 12, 38, 37), "/home/user/projects/a/target/release/build/zstd-sys-1/build-script-build");
        first(at(23, 7, 41, 6), "/home/user/projects/b/target/release/build/zstd-sys-1/build-script-build");
        first(at(23, 7, 43, 54), "/home/user/projects/c/target/release/build/zstd-sys-1/build-script-build");
        first(at(23, 7, 43, 51), "/home/user/.cargo/bin/rustup");
        // Connections: one program new to the network today; two known ones reaching new places.
        auto conn = [&](const char* program, const char* exe, const char* remote, int64_t first_seen, int64_t last_seen) {
            exec(std::string("INSERT INTO connections(program, exe, remote, port, proto, first_seen, last_seen) VALUES ('") + program + "', '" + exe +
                 "', '" + remote + "', 443, 'tcp', " + std::to_string(first_seen) + ", " + std::to_string(last_seen) + ")");
        };
        conn("firefox", "/usr/lib/firefox/firefox", "198.51.100.7", at(10, 23, 9, 55), at(22, 21, 35, 0));
        conn("firefox", "/home/user/.cache/ms-playwright/firefox-1/firefox/firefox", "203.0.113.30", at(23, 8, 47, 54), at(23, 8, 48, 26));
        conn("curl", "/usr/bin/curl", "198.51.100.20", at(19, 10, 0, 0), at(19, 10, 0, 5));
        conn("curl", "/usr/bin/curl", "203.0.113.40", at(23, 7, 8, 42), at(23, 7, 8, 44));
        conn("openssl", "/usr/bin/openssl", "192.0.2.81", at(23, 6, 40, 41), at(23, 6, 40, 43));
        // Readings once a second. The small hours: idle at 1-2% but for a scan from 02:41 to 02:50. Lunch: 311 to
        // 350 processes counted, kernel threads included.
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO samples(t, cpu, mem_pct, temp, fan, procs) VALUES (?, ?, 42, 44, 45, ?)", -1, &st, nullptr);
        auto sample = [&](int64_t t, double cpu, int64_t procs) {
            sqlite3_bind_int64(st, 1, t);
            sqlite3_bind_double(st, 2, cpu);
            sqlite3_bind_int64(st, 3, procs);
            sqlite3_step(st);
            sqlite3_reset(st);
        };
        for (int64_t t = at(23, 2, 15, 0); t < at(23, 3, 45, 0); ++t)
            sample(t, t >= at(23, 2, 41, 0) && t < at(23, 2, 50, 0) ? 25 : 1 + double(t % 3) / 2, 313);
        for (int64_t t = at(23, 12, 0, 0); t < now; ++t) sample(t, 13, 311 + t % 40);
        sqlite3_finalize(st);
        exec("INSERT INTO events(id, start, \"end\", kinds, primary_kind, triggers, summary, peak_cpu) VALUES (7001, " + std::to_string(at(23, 2, 41, 0)) +
             ", " + std::to_string(at(23, 2, 50, 0)) + ", 1, 1, 'cpu', 'CPU busy: firefox', 33)");
        exec("INSERT INTO event_culprits(event_id, rank, kind, name, cmdline, pid, cpu_seconds, share) VALUES (7001, 1, 'process', 'firefox', "
             "'firefox --headless', 4100, 120, 0.8)");
        for (int m = 55; m < 65; ++m) {
            const std::string minute = std::to_string(at(23, 2, 0, 0) + m * 60);
            exec("INSERT INTO minute_top(minute, pid, start_ticks, name, cmdline, unit, cpu_seconds, peak_rss, io_bytes, exited) VALUES "
                 "(" + minute + ", 1536, 0, 'plasmashell', '/usr/bin/plasmashell', 'plasma-plasmashell.service', 2.0, 4e8, 0, 0),"
                 "(" + minute + ", 1490, 0, 'kwin_wayland', '/usr/bin/kwin_wayland', 'plasma-kwin_wayland.service', 1.0, 2e8, 0, 0),"
                 "(" + minute + ", 2210, 0, 'conky', 'conky -c conky.conf', 'conky.service', 0.4, 5e7, 0, 0)");
        }
        // The process records (no kernel threads): six running at 13:00 under five names, one finished long before.
        auto proc = [&](int64_t pid, int64_t started, int64_t ended, const char* name) {
            exec("INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss) VALUES (" +
                 std::to_string(pid) + "," + std::to_string(started) + "," + (ended ? std::to_string(ended) : std::string("NULL")) + ",'" + name +
                 "','/usr/bin/" + name + "','" + name + "','user','',1,'systemd',10,1e8)");
        };
        proc(1536, at(22, 13, 7, 29), 0, "plasmashell");
        proc(1490, at(22, 13, 7, 25), 0, "kwin_wayland");
        proc(880, at(19, 22, 47, 30), 0, "ollama");
        proc(5001, at(23, 12, 40, 0), at(23, 13, 20, 0), "ninja");
        proc(5002, at(23, 12, 59, 0), at(23, 13, 1, 0), "cc1plus");
        proc(5003, at(23, 12, 59, 30), at(23, 13, 0, 30), "cc1plus");
        proc(4000, at(23, 9, 0, 0), at(23, 10, 0, 0), "firefox");
        proc(1, at(16, 17, 34, 0), 0, "systemd");  // the service manager is among the programs on record
        // Monday evening's fan, before the per-second readings: the hourly records (median 80%, 85% at most, 100% in
        // the hour from 22:00) and the readings the heat event before the 22:44 cut kept, at 100% from 22:43:38.
        for (int h = 17; h < 24; ++h)
            exec("INSERT INTO samples_hourly(hour_ts, metric, n, med, p05, p95, max, min) VALUES (" + std::to_string(at(19, h, 0, 0)) +
                 ", 'fan', 3600, 80, 80, 85, " + (h == 22 ? "100" : "85") + ", 80)");
        exec("INSERT INTO events(id, start, \"end\", kinds, primary_kind, triggers, summary, peak_temp, peak_fan) VALUES (7002, " + std::to_string(at(19, 22, 40, 0)) +
             ", " + std::to_string(at(19, 22, 44, 30)) + ", 16, 16, 'temp', 'Hot: python', 98, 100)");
        for (int64_t t = at(19, 22, 40, 0); t <= at(19, 22, 44, 30); ++t)
            exec("INSERT INTO event_samples(event_id, t, cpu, temp, fan) VALUES (7002, " + std::to_string(t) + ", 90, 95, " +
                 (t >= at(19, 22, 43, 38) ? "100" : "85") + ")");
        exec("COMMIT");
        sqlite3_close(db);
    }
    // The live machine for "right now": two programs and two kernel threads (kthreadd and a worker under it).
    Tree t;
    t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
    t.write("proc/1/stat", stat_line(1, "systemd", 'S', 0, 100, 0, 10, 100));
    t.write("proc/2/stat", stat_line(2, "kthreadd", 'S', 0, 0, 0, 10, 0));
    t.write("proc/40/stat", stat_line(40, "kworker/0:1", 'I', 2, 0, 0, 20, 0));
    t.write("proc/900/stat", stat_line(900, "bash", 'S', 1, 10, 0, 500, 100));
    char exe[4096];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string binary = len > 0 ? std::string(exe, std::size_t(len)) : std::string("./ascore_tests");
    const std::string cache = binary.substr(0, binary.rfind('/')) + "/kinds-cache";
    fs::create_directories(cache);
    auto ask_as = [&](const std::string& kind, const char* q) {
        as::Ask a;
        CHECK(a.open(dbp));
        a.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin", "vendor/unllm/data/banter.txt",
                        "vendor/unllm/data/rewrites.txt");
        a.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        a.load_facts({"facts/base.tsv"});
        CHECK(a.load_kinds("data/kinds", cache, model_dir));
        a.set_roots(t.roots());
        a.force_reading(kind);
        return a.ask(q, now);
    };
    auto has = [](const as::AskResult& r, const char* s) {
        const bool ok = r.answer.find(s) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  no \"%s\" in [%s] %s\n", s, r.branch.c_str(), r.answer.c_str());
        return ok;
    };
    auto lacks = [](const as::AskResult& r, const char* s) {
        const bool ok = r.answer.find(s) == std::string::npos;
        if (!ok) std::fprintf(stderr, "  \"%s\" in [%s] %s\n", s, r.branch.c_str(), r.answer.c_str());
        return ok;
    };
    auto before = [](const as::AskResult& r, const char* a, const char* b) {
        const auto i = r.answer.find(a), j = r.answer.find(b);
        const bool ok = i != std::string::npos && (j == std::string::npos || i < j);
        if (!ok) std::fprintf(stderr, "  \"%s\" not before \"%s\" in [%s] %s\n", a, b, r.branch.c_str(), r.answer.c_str());
        return ok;
    };

    // Q9: the latest plug-in in all the records, its level, and that it stayed in; an unplug is told as such.
    {
        as::AskResult r = ask_as("", "when did i last plug the charger in");
        CHECK(has(r, "10:07") && has(r, "56%") && before(r, "10:07", "23:46") && has(r, "since"));
        as::AskResult u = ask_as("battery.charger.when.window", "when did i last unplug the laptop");
        CHECK(has(u, "09:11") && before(u, "09:11", "10:07"));
        CHECK(has(ask_as("", "how many times did i unplug the charger this week"), "four times"));
    }
    // Q16: the lid's closes this week, not last week's, and not sleeps or kills.
    {
        as::AskResult r = ask_as("", "how many times did i close the lid this week");
        CHECK(has(r, "four times") && has(r, "14:54") && lacks(r, "19:09") && lacks(r, "out-of-memory"));
        CHECK(has(ask_as("", "how often did i shut the lid yesterday"), "twice"));
        CHECK(has(ask_as("battery.lid.when.window", "when did i last shut the lid"), "14:54"));
    }
    // Q32: the person's log-ins, not the login screen's own sessions nor its service manager's; the latest first.
    {
        as::AskResult r = ask_as("", "when did i last log in");
        CHECK(has(r, "13:07") && before(r, "13:07", "22:47") && has(r, "ower button") && lacks(r, "greeter") && lacks(r, "It last started"));
        as::AskResult n = ask_as("startup.login.list.window", "how many times did i log in this week");
        CHECK(has(n, "three times") && has(n, "07:21") && lacks(n, "greeter") && lacks(n, "manager"));
        CHECK(has(ask_as("", "what time did i last log in"), "13:07"));
    }
    // Q14: sudo this week (since Monday), the refused one said so, last week's left out.
    {
        as::AskResult r = ask_as("", "what did i run with sudo this week");
        CHECK(has(r, "no_turbo") && has(r, "pacman -S onnxruntime-cuda") && has(r, "pacman -Syy") && has(r, "refused") &&
              has(r, "password is required") && lacks(r, "14:21") && lacks(r, "17:19") && lacks(r, "software"));
        CHECK(has(ask_as("", "which commands did i run as root since monday"), "no_turbo"));
        CHECK(ask_as("trust.admin.list.window", "did i use sudo yesterday").answer.rfind("No", 0) == 0);
    }
    // Q26: the log's first runs this week, grouped, not a live trust scan; last week's left out.
    {
        as::AskResult r = ask_as("", "what new programs did i run for the first time this week");
        CHECK(lacks(r, "weird") && has(r, "build-script-build") && has(r, "three") && has(r, "rustup") && has(r, "versions") &&
              lacks(r, "tool_old"));
        CHECK(has(ask_as("", "which programs ran for the first time yesterday"), "build-script-build"));
    }
    // Q17: programs new to the network today, apart from known programs reaching new places.
    {
        as::AskResult r = ask_as("", "which apps connected to the internet for the first time today");
        CHECK(has(r, "openssl") && has(r, "06:40") && before(r, "openssl", "firefox") && has(r, "before"));
        CHECK(has(ask_as("", "did any program connect out for the first time this week"), "openssl"));
        CHECK(ask_as("network.new.list.window", "did anything new start connecting out last night").answer.rfind("No", 0) == 0);
    }
    // Q36: how many were running: the recorder's count (kernel threads included) and the process records at lunch.
    {
        as::AskResult r = ask_as("", "how many programs were running at lunch time today");
        CHECK(has(r, "311") && has(r, "350") && has(r, "kernel") && has(r, "13:00") && has(r, "cc1plus") && lacks(r, "weird"));
        as::AskResult live = ask_as("", "how many processes are running right now");
        CHECK(has(live, "4 processes") && has(live, "2 kernel threads"));
    }
    // Q35: a named moment: what the machine was doing then, before the window's events.
    {
        as::AskResult r = ask_as("", "what was the laptop doing at 3am last night");
        CHECK(r.branch == "records:story" && has(r, "03:00") && has(r, "plasmashell") && has(r, "quiet") && before(r, "03:00", "slowdown"));
        CHECK(has(ask_as("", "what was my computer up to around 3am"), "plasmashell"));
    }
    // Q7: Monday evening's fan from the hourly highs and the heat event's own readings, not the hourly medians.
    {
        as::AskResult r = ask_as("heat.fan.value.window", "how loud did the fan get monday evening");
        CHECK(has(r, "100%") && has(r, "22:43") && lacks(r, "peaked at 80%"));
        CHECK(ask_as("", "how loud did the fan get monday evening").branch == "records:fan");
        CHECK(has(ask_as("", "how noisy did the fan get on monday night"), "100%"));
    }
    // Q45-Q47: what the records cannot hold gets the kind's own reply, saying what is recorded instead.
    {
        for (const char* q : {"how many tabs have i got open in firefox", "which websites did i visit yesterday"}) {
            as::AskResult r = ask_as("", q);
            CHECK(r.refused && r.branch == "unanswered:story.tabs.none");
        }
        for (const char* q : {"what was i typing when the laptop shut off on sunday", "what was on the screen when the laptop died"}) {
            as::AskResult r = ask_as("", q);
            CHECK(r.refused && r.branch == "unanswered:story.typing.none");
        }
        for (const char* q : {"which website used the most bandwidth yesterday", "how much data did youtube use today"}) {
            as::AskResult r = ask_as("", q);
            CHECK(r.refused && r.branch == "unanswered:network.site.none");
        }
    }
    // The traced 95 (T95): a program on record is no typo ("systemd" is not "system"); its crashes are the units that
    // failed this week, not last week's and not a program's crash.
    {
        as::AskResult r = ask_as("", "systemd crashes this week");
        CHECK(r.branch == "records:units-failed" && has(r, "ollama") && has(r, "backup") && lacks(r, "oldjob") && lacks(r, "player"));
        as::AskResult s = ask_as("", "any systemd crashes yesterday");
        CHECK(s.branch == "records:units-failed" && has(s, "ollama") && lacks(s, "backup"));
    }
    // T92: a question that asks no why ("fan noise last night") takes the nearly as near kind that is no cause: the fan's
    // level, not the heat's culprits; a why question keeps the cause.
    {
        CHECK(ask_as("", "fan noise last night").branch == "records:fan");
        CHECK(ask_as("", "loud fan last night").branch == "records:fan");
        CHECK(ask_as("", "why was the fan loud last night").branch != "records:fan");
    }
    // T18, T62, T81: what the records cannot hold, BIOS settings and screens, and a reminder (ask answers, it doesn't act).
    {
        for (const char* q : {"bios settings changed", "did someone mess with the uefi settings"}) {
            as::AskResult r = ask_as("", q);
            CHECK(r.refused && r.branch == "unanswered:identity.bios.settings.none");
        }
        for (const char* q : {"was the external monitor connected while i was gaming", "was the hdmi screen plugged in while i watched the film"}) {
            as::AskResult r = ask_as("", q);
            CHECK(r.refused && r.branch == "unanswered:devices.monitor.none");
        }
        for (const char* q : {"can you remind me tomorrow to clean the fans", "could you remind me next week to update the system"}) {
            as::AskResult r = ask_as("", q);
            CHECK(r.refused && r.branch == "unanswered:none.reminder");
        }
    }
    // T61: devices over a window are the USB, serial and drive records; a Bluetooth connection is not among them, said so.
    {
        as::AskResult r = ask_as("", "did a bluetooth device connect last night");
        CHECK(r.branch == "log:devices" && has(r, "Bluetooth"));
        CHECK(has(ask_as("", "did my bluetooth speaker connect this morning"), "Bluetooth"));
    }
    fs::remove_all(dir);
}

// Answers over a window from the records that cover it, not from the spike events alone, on rows shaped like the records
// copied on 2026-10-23 at 13:58 (generic names), at the clock of the second 50-question test: a program's CPU over days
// before the per-minute lists begin (its runs shared out), its memory and its runs, the peak with its moment and what
// ran then, a window told from its readings, the busiest hour, how often a temperature was reached, an interpreter's
// scripts, and parents whose per-minute figures hold their finished children's whole lives.
void test_window_answers() {
    std::puts("window answers: runs, readings, hours, scripts");
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model_dir = em ? em : std::string(home ? home : "") + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model_dir + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: window answer checks skipped");
        return;
    }
    const std::string dir = fs::temp_directory_path() / ("as-windows-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const std::string dbp = dir + "/h.db";
    auto at = [](int day, int h, int m, int s) {
        std::tm tm{};
        tm.tm_year = 2026 - 1900; tm.tm_mon = 9; tm.tm_mday = day; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s; tm.tm_isdst = -1;
        return int64_t(std::mktime(&tm));
    };
    const int64_t now = at(23, 13, 59, 8);  // Fri Oct 23 13:59:08, the clock of the second 50-question test
    { as::Store st(dbp); }
    {
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        auto exec = [&](const std::string& sql) {
            char* err = nullptr;
            const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
            if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
            CHECK(ok);
        };
        auto num = [](double v) { return std::to_string(v); };
        exec("BEGIN");
        // The hourly rollups from Saturday morning to Friday noon; asleep Wednesday until 17:00 and Thursday until 06:45.
        auto hour = [&](int64_t h, const char* metric, int n, double med, double p95, double max) {
            exec("INSERT OR REPLACE INTO samples_hourly(hour_ts, metric, n, med, p05, p95, max, dropouts, on_battery_frac) VALUES(" +
                 std::to_string(h) + ",'" + metric + "'," + std::to_string(n) + "," + num(med) + "," + num(med) + "," + num(p95) + "," +
                 num(max) + ",0,0)");
        };
        for (int64_t h = at(17, 6, 0, 0); h < at(23, 13, 0, 0); h += 3600) {
            if ((h >= at(21, 0, 0, 0) && h < at(21, 17, 0, 0)) || (h >= at(22, 0, 0, 0) && h < at(22, 6, 0, 0))) continue;
            const int n = h == at(22, 6, 0, 0) ? 900 : 3600;
            hour(h, "cpu", n, 3, 10, 30);
            hour(h, "temp", n, 50, 60, 70);
            hour(h, "fan", n, 45, 45, 50);
            hour(h, "gpu", n, 0, 5, 20);
        }
        hour(at(22, 6, 0, 0), "cpu", 900, 16, 27, 50);  // the quarter hour after waking: busier, but not an hour
        hour(at(22, 7, 0, 0), "cpu", 3600, 12, 29, 45);
        hour(at(22, 9, 0, 0), "cpu", 3600, 8.8, 21, 82);
        hour(at(22, 9, 0, 0), "temp", 3600, 70, 85, 95);
        hour(at(22, 10, 0, 0), "temp", 3600, 55, 80, 95);
        for (int h : {7, 8, 17, 18, 20}) hour(at(17, h, 0, 0), "fan", 3600, 80, 80, 85);
        hour(at(17, 18, 0, 0), "temp", 3600, 82, 90, 95);
        hour(at(17, 19, 0, 0), "fan", 3600, 80, 85, 100);
        hour(at(17, 19, 0, 0), "temp", 3600, 82, 95, 100);
        hour(at(19, 15, 0, 0), "temp", 3600, 60, 80, 96);
        hour(at(23, 7, 0, 0), "temp", 3600, 55, 70, 97);
        // Readings every 30 s from Thursday 13:30: calm, with the GPU's two spells last night, the afternoon's hour and
        // the busy spells this morning.
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO samples(t, cpu, temp, fan, gpu, mem_pct, cores) VALUES (?, ?, ?, ?, ?, 50, "
                               "'1,1,1,1,1,1,1,1,1,1,1,1')", -1, &st, nullptr);
        auto sample = [&](int64_t t, double cpu, double temp, double fan, double gpu) {
            sqlite3_bind_int64(st, 1, t);
            sqlite3_bind_double(st, 2, cpu);
            sqlite3_bind_double(st, 3, temp);
            sqlite3_bind_double(st, 4, fan);
            sqlite3_bind_double(st, 5, gpu);
            sqlite3_step(st);
            sqlite3_reset(st);
        };
        for (int64_t t = at(22, 13, 30, 0); t < now; t += 30) {
            const bool busy = (t >= at(23, 7, 51, 0) && t < at(23, 8, 8, 0)) || (t >= at(23, 10, 6, 0) && t < at(23, 10, 14, 0));
            sample(t, busy ? 50 : 3, 50, 45, 0);
        }
        for (int64_t t = at(22, 23, 33, 0); t <= at(22, 23, 37, 0); t += 30) sample(t, 20, 60, 60, 99);
        for (int64_t t = at(23, 0, 1, 0); t <= at(23, 0, 4, 0); t += 30) sample(t, 20, 60, 60, 86);
        sample(at(23, 0, 8, 0), 3, 50, 45, 5);
        sample(at(23, 7, 57, 0), 50, 96, 85, 0);
        sample(at(23, 7, 57, 30), 50, 97, 85, 0);
        sample(at(23, 12, 30, 0), 3, 58, 45, 0);
        sample(at(23, 13, 17, 0), 3, 46, 45, 0);
        sample(at(23, 13, 29, 30), 3, 65, 45, 0);
        sample(at(23, 13, 42, 0), 3, 61, 45, 0);
        sqlite3_finalize(st);
        // Events, each with its culprits.
        auto event = [&](int id, int64_t start, int64_t end, int kinds, double cpu, double temp, double fan, double gpu, double margin) {
            exec("INSERT INTO events(id, start, end, kinds, summary, peak_cpu, peak_temp, peak_fan, peak_gpu, min_margin, total_cpu_seconds) VALUES(" +
                 std::to_string(id) + "," + num(double(start)) + "," + num(double(end)) + "," + std::to_string(kinds) + ",'event'," + num(cpu) +
                 "," + num(temp) + "," + num(fan) + "," + num(gpu) + "," + num(margin) + ",1000)");
        };
        auto culprit = [&](int id, const char* kind, const char* name, const char* cmdline, int pid, double cpu, double share, double vram) {
            exec("INSERT INTO event_culprits(event_id, kind, name, cmdline, pid, cpu_seconds, share, gpu_mem) VALUES(" + std::to_string(id) +
                 ",'" + kind + "','" + name + "','" + cmdline + "'," + std::to_string(pid) + "," + num(cpu) + "," + num(share) + "," + num(vram) + ")");
        };
        auto esample = [&](int id, int64_t t, double temp) {
            exec("INSERT INTO event_samples(event_id, t, cpu, temp, fan) VALUES(" + std::to_string(id) + "," + num(double(t)) + ",60," + num(temp) + ",80)");
        };
        // Thursday morning: 95 C twice, the second while the server and a worker script ran.
        event(10, at(22, 9, 19, 36), at(22, 9, 23, 2), 31, 67, 95, 85, 20, 5);
        for (auto [t, c] : std::vector<std::pair<int64_t, double>>{{at(22, 9, 19, 40), 80}, {at(22, 9, 21, 54), 95}, {at(22, 9, 21, 55), 95},
                                                                   {at(22, 9, 22, 30), 85}})
            esample(10, t, c);
        culprit(10, "process", "plasmashell", "/usr/bin/plasmashell", 1536, 144, 0.3, 0);
        culprit(10, "process", "exe", "unllm --serve", 1372, 74, 0.15, 0);
        culprit(10, "unseen", "ollama", "", 1200, 40, 0.08, 0);
        event(11, at(22, 9, 59, 8), at(22, 10, 11, 51), 31, 100, 95, 85, 30, 5);
        for (auto [t, c] : std::vector<std::pair<int64_t, double>>{{at(22, 10, 0, 0), 80}, {at(22, 10, 8, 43), 95}, {at(22, 10, 8, 44), 95},
                                                                   {at(22, 10, 9, 0), 95}, {at(22, 10, 10, 0), 85}})
            esample(11, t, c);
        culprit(11, "unseen", "systemd", "", 774, 11600, 1.0, 0);
        culprit(11, "process", "exe", "unllm --serve", 1372, 363, 0.21, 0);
        culprit(11, "process", "python", "python worker.py", 1400, 127, 0.07, 0);
        culprit(11, "process", "llama-server", "/usr/local/lib/ollama/llama-server --model m", 3012, 121, 0.07, 0);
        event(12, at(22, 9, 35, 5), at(22, 9, 43, 56), 16, 35, 93, 80, 0, 7);
        culprit(12, "process", "exe", "unllm --serve", 1372, 50, 0.4, 0);
        // Saturday: the benchmark in the morning and the evening, the fan at 100% from 19:15.
        const std::vector<std::tuple<int, int64_t, int64_t, double, double, double>> sat = {
            {20, at(17, 7, 29, 41), at(17, 7, 59, 42), 85, 92, 7},  {21, at(17, 7, 59, 39), at(17, 8, 23, 36), 85, 93, 6},
            {22, at(17, 8, 24, 14), at(17, 8, 50, 46), 85, 93, 6},  {23, at(17, 17, 38, 21), at(17, 18, 8, 16), 85, 95, 5},
            {24, at(17, 18, 8, 20), at(17, 18, 14, 2), 85, 94, 8},  {25, at(17, 18, 15, 7), at(17, 18, 45, 7), 85, 95, 6},
            {26, at(17, 18, 45, 4), at(17, 19, 15, 5), 85, 93, 8},  {27, at(17, 19, 15, 2), at(17, 19, 44, 59), 100, 100, 3},
            {28, at(17, 19, 45, 16), at(17, 20, 10, 9), 100, 97, 4}};
        for (const auto& [id, a, b, fan, temp, margin] : sat) {
            event(id, a, b, 31, 100, temp, fan, 80, margin);
            culprit(id, "group", "unllm", "unllm --branch --brief", 0, 600, 0.5, 0);
            culprit(id, "process", "python", "python reader.py", 1500, 240, 0.2, 0);
            culprit(id, "process", "llama-server", "/usr/local/lib/ollama/llama-server --model m", 1600, 120, 0.1, 6.4e9);
        }
        event(30, at(19, 15, 10, 0), at(19, 15, 20, 0), 16, 90, 96, 85, 0, 4);
        culprit(30, "process", "firefox", "/usr/lib/firefox/firefox", 2001, 200, 0.6, 0);
        // The GPU last night: two short spells of the model runner.
        event(40, at(22, 23, 33, 0), at(22, 23, 37, 0), 8, 20, 60, 60, 99, -1);
        culprit(40, "process", "llama-server", "/usr/local/lib/ollama/llama-server --model m", 3030, 30, 0.5, 3.4e9);
        event(41, at(23, 0, 1, 0), at(23, 0, 4, 0), 8, 20, 60, 60, 86, -1);
        culprit(41, "process", "llama-server", "/usr/local/lib/ollama/llama-server --model m", 3031, 30, 0.5, 3.0e9);
        // Monday night: the browser session cut off by the crash, charged in the spikes before it.
        event(50, at(19, 22, 2, 0), at(19, 22, 30, 0), 1, 90, 70, 60, 0, -1);
        culprit(50, "process", "firefox", "/usr/lib/firefox/firefox", 2101, 2000, 0.7, 0);
        event(51, at(19, 22, 30, 5), at(19, 22, 44, 0), 1, 90, 70, 60, 0, -1);
        culprit(51, "process", "firefox", "/usr/lib/firefox/firefox", 2101, 1000, 0.7, 0);
        // This morning's build and tests.
        event(60, at(23, 10, 7, 0), at(23, 10, 13, 30), 1, 100, 80, 80, 0, -1);
        culprit(60, "process", "ascore_tests", "./build/ascore_tests", 605, 300, 0.4, 0);
        culprit(60, "group", "cc1plus", "cc1plus -quiet", 0, 180, 0.3, 0);
        // Processes.
        auto proc = [&](int pid, int64_t started, int64_t ended, const char* name, const char* exe, const char* cmdline, const char* unit,
                        int ppid, double cpu, double rss, int estimated = 0) {
            exec("INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss, end_estimated) VALUES(" +
                 std::to_string(pid) + "," + num(double(started)) + "," + (ended ? num(double(ended)) : std::string("NULL")) + ",'" + name + "','" +
                 exe + "','" + cmdline + "','user','" + unit + "'," + std::to_string(ppid) + ",''," + num(cpu) + "," + num(rss) + "," +
                 std::to_string(estimated) + ")");
        };
        const char* ff = "/usr/lib/firefox/firefox";
        proc(2001, at(19, 9, 13, 0), at(19, 11, 38, 0), "firefox", ff, ff, "app-firefox@1.service", 1, 1000, 4e8);
        proc(2002, at(19, 9, 14, 0), at(19, 11, 38, 0), "Isolated Web Co", ff, "/usr/lib/firefox/firefox -contentproc", "app-firefox@1.service",
             2001, 500, 4e8);
        proc(2003, at(19, 12, 36, 0), at(19, 13, 41, 0), "firefox", ff, ff, "app-firefox@2.service", 1, 546, 4e8);
        proc(2101, at(19, 18, 59, 35), at(19, 22, 44, 30), "firefox", ff, ff, "app-firefox@3.service", 1, 1.4, 4e8, 1);  // cut off: stale
        proc(2201, at(19, 23, 0, 0), at(20, 7, 0, 0), "firefox", ff, ff, "app-firefox@4.service", 1, 8000, 4e8);  // an eighth on Monday
        proc(2301, at(18, 10, 0, 0), at(18, 12, 0, 0), "firefox", ff, ff, "app-firefox@5.service", 1, 700, 4e8);  // Sunday
        const char* ol = "/usr/local/bin/ollama";
        const char* ls = "/usr/local/lib/ollama/llama-server";
        const char* lsm = "/usr/local/lib/ollama/llama-server --model m";
        proc(3001, at(17, 6, 0, 0), at(18, 2, 0, 0), "ollama", ol, "/usr/local/bin/ollama serve", "ollama.service", 1, 9000, 6e8);
        for (int i = 0; i < 4; ++i)
            proc(3002 + i, at(17, 8, 15 * i, 0), at(17, 8, 15 * i + 10, 0), "llama-server", ls, lsm, "ollama.service", 3001, 500, 3e9);
        proc(2999, at(21, 21, 30, 0), at(22, 7, 23, 31), "ollama", ol, "/usr/local/bin/ollama serve", "ollama.service", 1, 0, 6e8);
        proc(3010, at(21, 21, 35, 0), at(22, 7, 23, 31), "llama-server", ls, lsm, "ollama.service", 2999, 0, 6.0e9);
        proc(3009, at(22, 7, 24, 0), at(22, 10, 53, 53), "ollama", ol, "/usr/local/bin/ollama serve", "ollama.service", 1, 0, 6e8);
        proc(3011, at(22, 7, 44, 0), at(22, 7, 49, 0), "llama-server", ls, lsm, "ollama.service", 3009, 0, 5.65e9);
        for (int i = 0; i < 10; ++i)
            proc(3020 + i, at(22, 8, 4 * i, 0), at(22, 8, 4 * i + 2, 0), "llama-server", ls, lsm, "ollama.service", 3009, 0, 0.9e9);
        proc(3012, at(22, 9, 59, 59), at(22, 10, 16, 52), "llama-server", ls, lsm, "ollama.service", 3009, 0, 7.52e9);
        proc(3013, at(22, 10, 42, 41), at(22, 10, 53, 55), "llama-server", ls, lsm, "ollama.service", 3009, 0, 7.05e9);
        proc(3000, at(22, 10, 53, 58), 0, "ollama", ol, "/usr/local/bin/ollama serve", "ollama.service", 1, 0.5, 6e8);
        proc(4001, at(21, 17, 42, 19), at(21, 18, 4, 25), "blender", "/usr/bin/blender", "blender -b -P render.py", "vte-spawn-2.scope", 4000,
             1907.9, 2e9);
        proc(4002, at(21, 19, 25, 37), at(21, 20, 20, 37), "blender", "/usr/bin/blender", "blender -b -P render.py", "vte-spawn-2.scope", 4000,
             93, 2e9);
        proc(4100, at(20, 13, 0, 0), 0, "blender-mcp", "/usr/bin/python3.14", "python3 /home/user/.local/bin/blender-mcp", "vte-spawn-1.scope",
             500, 3, 5e7);
        const char* py = "/usr/bin/python3.14";
        proc(5001, at(19, 22, 47, 14), 0, "python3", py, "python3 /home/user/shop/app.py", "app-shop.service", 1, 20, 9e7);
        proc(5002, at(19, 22, 47, 14), 0, "python3", py, "python3 /home/user/mailer/app.py", "app-mailer.service", 1, 20, 9e7);
        proc(5003, at(22, 13, 7, 29), 0, "python3", py, "python3 /home/user/tools/progress_tray.py", "app-tray.service", 1, 5, 6e7);
        proc(5004, at(23, 7, 51, 13), at(23, 8, 7, 30), "python3", py, "python3 -I askend.py phase1", "vte-spawn-1.scope", 500, 650, 2e8);
        proc(5005, at(23, 8, 42, 0), at(23, 8, 52, 21), "python3", py, "python3 -m scan", "run-scan.scope", 1, 60, 1e8);
        for (int i = 0; i < 5; ++i)
            proc(5010 + i, at(23, 11, 5 * i, 0), at(23, 11, 5 * i, 30), "python3", py, "python3 tool.py", "vte-spawn-1.scope", 500, 1, 3e7);
        // The terminal's assistant and the build it ran this morning: a shell, the build wrapper, cmake, ninja, the compiler
        // driver and the compiler, and the test suite.
        const char* tab = "vte-spawn-1.scope";
        proc(500, at(22, 13, 7, 40), 0, "claude", "/home/user/.local/share/claude/versions/2.1.308", "claude --resume", tab, 400, 2000, 7e8);
        proc(601, at(23, 10, 6, 0), at(23, 10, 13, 10), "zsh", "/usr/bin/zsh", "/usr/bin/zsh -c make", tab, 500, 0.2, 1e7);
        proc(602, at(23, 10, 6, 1), at(23, 10, 12, 55), "heatwatch.sh", "/usr/bin/bash", "/bin/bash heatwatch.sh", tab, 601, 0, 1e7);
        proc(603, at(23, 10, 6, 2), at(23, 10, 12, 54), "cmake", "/usr/bin/cmake", "cmake --build build", tab, 602, 0, 1e7);
        proc(604, at(23, 10, 6, 10), at(23, 10, 12, 50), "ninja", "/usr/bin/ninja", "/usr/bin/ninja -j 4", tab, 603, 5, 1e7);
        proc(605, at(23, 10, 10, 0), at(23, 10, 13, 0), "ascore_tests", "/home/user/as/build/ascore_tests", "./build/ascore_tests", tab, 601, 300, 5e8);
        std::map<int64_t, int> ended_in;  // the compilers' drivers ended in each minute
        for (int i = 0; i < 30; ++i) {
            const int64_t s = at(23, 10, 6, 30) + 12 * i;
            proc(610 + i, s, s + 10, "c++", "/usr/bin/c++", "c++ -c x.cpp", tab, 604, 0, 1e7);
            proc(650 + i, s + 1, s + 9, "cc1plus", "/usr/lib/gcc/cc1plus", "cc1plus -quiet x.cpp", tab, 610 + i, 6, 3e8);
            ++ended_in[(s + 10) / 60 * 60];
        }
        // Per-minute lists from Thursday 13:33. The parents' minutes hold their children's whole lives at the minute they
        // ended, as the recorder wrote them before 2026-10-23 11:22.
        auto minutes = [&](int64_t from, int64_t to, int pid, const char* name, const char* cmdline, const char* unit, double cpu) {
            for (int64_t m = from; m < to; m += 60)
                exec("INSERT INTO minute_top(minute, pid, start_ticks, name, cmdline, unit, cpu_seconds, peak_rss, io_bytes, exited) VALUES(" +
                     num(double(m)) + "," + std::to_string(pid) + ",0,'" + name + "','" + cmdline + "','" + unit + "'," + num(cpu) + ",1e8,0,0)");
        };
        minutes(at(22, 13, 33, 0), at(22, 13, 34, 0), 1497, "kwin_wayland", "/usr/bin/kwin_wayland", "plasma-kwin_wayland.service", 0.5);
        minutes(at(22, 14, 0, 0), at(22, 15, 0, 0), 3000, "ollama", "/usr/local/bin/ollama serve", "ollama.service", 2);
        minutes(at(23, 7, 51, 0), at(23, 8, 8, 0), 5004, "python3", "python3 -I askend.py phase1", tab, 40);
        minutes(at(23, 0, 0, 0), at(23, 13, 58, 0), 5003, "python3", "python3 /home/user/tools/progress_tray.py", "app-tray.service", 0.6);
        minutes(at(23, 9, 15, 0), at(23, 10, 45, 0), 500, "claude", "claude --resume", tab, 1);
        exec("UPDATE minute_top SET cpu_seconds = 1 + 485.2 WHERE pid = 500 AND minute = " + num(double(at(23, 10, 13, 0))));
        minutes(at(23, 10, 12, 0), at(23, 10, 13, 0), 603, "cmake", "cmake --build build", tab, 185);
        minutes(at(23, 10, 12, 0), at(23, 10, 13, 0), 602, "heatwatch.sh", "/bin/bash heatwatch.sh", tab, 185);
        minutes(at(23, 10, 12, 0), at(23, 10, 13, 0), 601, "zsh", "/usr/bin/zsh -c make", tab, 185);
        minutes(at(23, 10, 13, 0), at(23, 10, 14, 0), 601, "zsh", "/usr/bin/zsh -c make", tab, 300);
        for (int64_t m = at(23, 10, 6, 0); m <= at(23, 10, 12, 0); m += 60)
            minutes(m, m + 60, 604, "ninja", "/usr/bin/ninja -j 4", tab, 0.7 + 6.0 * ended_in[m]);
        for (int i = 0; i < 30; ++i) {
            const int64_t s = at(23, 10, 6, 31) + 12 * i;
            minutes(s / 60 * 60, s / 60 * 60 + 60, 650 + i, "cc1plus", "cc1plus -quiet x.cpp", tab, 6);
        }
        minutes(at(23, 10, 10, 0), at(23, 10, 13, 0), 605, "ascore_tests", "./build/ascore_tests", tab, 100);
        exec("INSERT INTO log(t, category, title, detail) VALUES(" + num(double(at(21, 0, 10, 0))) + ",'session','Went to sleep',''),(" +
             num(double(at(21, 17, 19, 0))) + ",'session','Woke from sleep','')");
        exec("COMMIT");
        sqlite3_close(db);
    }
    Tree t;
    t.write("proc/stat", "cpu 0 0 0 0 0 0 0 0\nbtime 1000\n");
    char exe[4096];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string bin = len > 0 ? std::string(exe, std::size_t(len)) : std::string("./ascore_tests");
    const std::string cache = bin.substr(0, bin.rfind('/')) + "/kinds-cache";
    fs::create_directories(cache);
    auto asked = [&](const char* q) {
        as::Ask ask;
        CHECK(ask.open(dbp));
        ask.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin", "vendor/unllm/data/banter.txt",
                          "vendor/unllm/data/rewrites.txt");
        ask.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        ask.load_facts({"facts/base.tsv"});
        CHECK(ask.load_kinds("data/kinds", cache, model_dir));
        ask.set_roots(t.roots());
        as::AskResult r = ask.ask(q, now);
        std::fprintf(stderr, "  [%s] %s -> %s (%s)\n", r.branch.c_str(), q, r.answer.c_str(), r.span.c_str());
        return r.answer;
    };
    auto has = [](const std::string& a, const char* s) { return a.find(s) != std::string::npos; };
    auto before = [](const std::string& a, const char* first, const char* second) {
        return a.find(first) != std::string::npos && a.find(first) < a.find(second);
    };

    // Q19: a program's CPU on a day before the per-minute lists begin: its runs, the one across midnight shared out by the
    // time the machine was awake, the one cut off by the crash counted from the spikes that charged it.
    for (const char* q : {"how much cpu did firefox use on monday", "how much processor time did firefox use on monday"}) {
        const std::string a = asked(q);
        CHECK(has(a, "6,046") && !has(a, "main cause"));
    }
    // Q31: since a day, the server with the runners it starts: its runs before the lists, the lists after.
    CHECK(has(asked("how much processor time has ollama used since saturday"), "11,120"));
    // Q37: a program's memory over a window: its runs' peaks, the highest with its run's times.
    for (const char* q : {"how much memory was llama-server using yesterday morning", "how much ram did llama-server take up yesterday morning"}) {
        const std::string a = asked(q);
        CHECK(has(a, "7.5 GB") && has(a, "09:59") && has(a, "10:16") && !has(a, "events"));
    }
    // Q20: whether a program ran and for how long: its runs with their times, not the helper named like it.
    {
        const std::string a = asked("was blender running on wednesday, and for how long");
        CHECK(a.rfind("Yes", 0) == 0 && has(a, "17:42") && has(a, "19:25") && has(a, "one hour and 17 minutes") && !has(a, "mcp"));
        const std::string b = asked("how long was blender running on wednesday");
        CHECK(has(b, "17:42") && has(b, "one hour and 17 minutes"));
    }
    // Q6: the peak, its moments and what was working then.
    {
        const std::string a = asked("what was the hottest it got yesterday and what was running then");
        CHECK(has(a, "95 C") && has(a, "09:21") && has(a, "10:08") && has(a, "unllm --serve") && !has(a, "ystemd"));
        CHECK(has(asked("when was it hottest yesterday"), "09:21"));
    }
    // Q44: a window told from its readings: the range with the peak's moment and the typical level, not the events.
    for (const char* q : {"how has the cpu temperature been over the past 2 hours", "what was the cpu temperature like between 1 and 2 pm today"}) {
        const std::string a = asked(q);
        CHECK(has(a, "46 C") && has(a, "65 C") && has(a, "13:29") && has(a, "50 C") && !has(a, "events"));
    }
    // Q24: the GPU over a night: its spells above the recorder's busy level and who held it, not a count of events.
    for (const char* q : {"what was the gpu doing last night", "how busy was the gpu last night"}) {
        const std::string a = asked(q);
        CHECK(has(a, "23:33") && has(a, "00:01") && has(a, "99%") && has(a, "lama-server") && !has(a, "during"));
    }
    // Q28: why the fan roared: the cause, and when it was worst with the fan's level.
    for (const char* q : {"why was the fan going crazy on saturday", "why was the fan so loud on saturday evening"}) {
        const std::string a = asked(q);
        CHECK(has(a, "nllm") && has(a, "19:15") && has(a, "fan") && has(a, "100%"));
    }
    // Q29: the busiest hour is an hour (the whole ones), not a program.
    {
        const std::string a = asked("what was the busiest hour on the laptop yesterday");
        CHECK(has(a, "07:00") && has(a, "08:00") && has(a, "12%") && !has(a, "processor time"));
        CHECK(has(asked("which hour was the hottest yesterday"), "09:00"));
    }
    // Q39: how often a temperature was reached: the hours and days at or over it, and the events that peaked there.
    for (const char* q : {"how often does the laptop hit 95 degrees", "how many times did it reach 95 degrees"}) {
        const std::string a = asked(q);
        CHECK(has(a, "six of the") && has(a, "four of the") && has(a, "seven events") && !has(a, "100 C during"));
    }
    // Q42: an interpreter's scripts by how long they ran, by name, never the busiest program of all.
    {
        const std::string a = asked("which python scripts ran the longest today");
        CHECK(has(a, "app.py") && has(a, "askend.py") && has(a, "07:51") && !has(a, "laude"));
        CHECK(has(asked("which python scripts used the most cpu today"), "askend.py"));
    }
    // Q4: a parent's minute that holds its finished children's whole lives is not its own work: the builds and the
    // tests it ran rank, not the assistant, the shell or the build tools that waited on them.
    for (const char* q : {"what was using the cpu so much around 10 this morning",
                          "which programs were busiest on the cpu between 10 and 10:30 this morning"}) {
        const std::string a = asked(q);
        CHECK(has(a, "score_tests") && has(a, "cc1plus") && (!has(a, "laude") || before(a, "score_tests", "laude")) && !has(a, "zsh"));
    }
    fs::remove_all(dir);
}

void test_notable() {
    std::puts("routine or notable");
    as::Event e;
    e.culprits.push_back({});
    e.culprits.back().unit = "app-org.kde.konsole-1234.scope";

    e.kinds = as::KindCpu | as::KindHeat;
    CHECK(!as::event_notable(e));  // your own app working hard is routine
    e.culprits.back().unit = "vte-spawn-abc123.scope";
    CHECK(!as::event_notable(e));  // a command you typed in a terminal, too
    e.min_margin = 4;
    CHECK(as::event_notable(e));  // nearly shut down: always notable
    e.min_margin = -1;
    e.culprits.back().unit = "ollama.service";
    CHECK(as::event_notable(e));  // a background service is notable
    e.culprits.back().unit = "app-x.scope";
    e.triggers = {"Swapping back in at 5 MB/s"};
    CHECK(as::event_notable(e));  // the machine struggling is notable
    e.triggers.clear();
    e.dead_fans = {"CPU fan"};
    CHECK(as::event_notable(e));
    as::Event none;
    CHECK(as::event_notable(none));  // no culprit to blame: show it
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
    {
        // A hand-built 32-bit ELF: the lib32 family must not be invisible.
        std::vector<unsigned char> e(0x200, 0);
        e[0] = 0x7f; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 1;
        auto put16 = [&](std::size_t o, uint16_t v) { e[o] = v & 0xff; e[o + 1] = uint8_t(v >> 8); };
        auto put32 = [&](std::size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) e[o + i] = uint8_t(v >> (8 * i)); };
        put32(0x20, 0x100); put16(0x2E, 40); put16(0x30, 3);
        const char* nm = "libtest.so.1";
        std::memcpy(&e[0x41], nm, std::strlen(nm) + 1);          // .dynstr at 0x40, index 1
        put32(0x60, 1); put32(0x64, 1);                           // DT_NEEDED -> 1
        std::size_t s1 = 0x100 + 40, s2 = 0x100 + 80;
        put32(s1 + 4, 6); put32(s1 + 16, 0x60); put32(s1 + 20, 16); put32(s1 + 24, 2);
        put32(s2 + 4, 3); put32(s2 + 16, 0x40); put32(s2 + 20, 0x10);
        std::ofstream f(root + "/elf32", std::ios::binary);
        f.write(reinterpret_cast<const char*>(e.data()), std::streamsize(e.size()));
        f.close();
        auto n32 = as::elf_needed(root + "/elf32");
        CHECK(n32.size() == 1 && n32[0] == "libtest.so.1");
    }
    // A command that never finishes is stopped, not waited on forever.
    auto slow = as::run_command({"sleep", "30"}, 1);
    CHECK(slow.out.find("did not finish") != std::string::npos);
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

// The event for a boot that stopped without a clean shutdown: the journal's last entry can come before the
// recorder's own last reading (Sun Oct 18: the log's last entry at 22:05:39, the readings on to 22:06:32). The stop is
// the later of the two, and its last two minutes are the readings before it, not the ones before the log's line.
void test_shutdown_event() {
    std::puts("shutdown event");
    Tree t;
    as::Store st(t.root + "/h.db");
    const double base = 1'800'000'000;
    for (int i = 0; i <= 60; ++i) {
        auto s = std::make_shared<as::Snapshot>();
        s->wall = base + i;
        s->cpu.total = i < 58 ? 90 : 1;
        s->cpu.temp_c = as::Reading<double>::of(i < 58 ? 95 : 74);
        s->guard.raw = as::Reading<double>::of(i == 40 ? 96 : i < 58 ? 92 : 74);
        s->guard.limit = as::Reading<double>::of(100);
        st.add_sample(*s);
    }
    st.flush();
    as::MinuteTop m;
    m.minute = int64_t(base);
    as::Culprit c;
    c.name = "python";
    c.cmdline = "/home/user/.cache/venv/bin/python -I vectors.py build";
    c.cpu_seconds = 50;
    m.top.push_back(c);
    st.add_minute(m);
    as::BootEndInfo sudden;
    // The log's last entry 55 s before the recorder's last reading; the next boot five minutes later.
    as::Event e = as::shutdown_event(st, sudden, base + 5, base + 300);
    CHECK(near(e.start, base + 60) && near(e.end, base + 60));
    CHECK(e.summary.find("-") == std::string::npos || e.summary.find("s before") == std::string::npos);
    CHECK(e.summary.find(as::clock_time(base + 60)) != std::string::npos);
    CHECK(near(e.min_margin, 4) && near(e.peak_temp, 95));  // the hottest second of its last two minutes is kept
    CHECK(e.summary.find("96") != std::string::npos);        // and said, not only the calm last reading
    CHECK(!e.culprits.empty());
    // A log that runs past the recorder (the recorder had stopped a minute before): the log's line is the stop.
    as::Event late = as::shutdown_event(st, sudden, base + 120, base + 300);
    CHECK(near(late.start, base + 120));
    CHECK(late.summary.find("60 s before") != std::string::npos);
    // Readings from the next boot are not this boot's.
    as::Event next = as::shutdown_event(st, sudden, base - 100, base - 50);
    CHECK(near(next.start, base - 100));
    CHECK(next.summary.find("not recording") != std::string::npos);
}

// The answers of the new 50 (asked at Fri Oct 23 13:59:08) that the records area fixes, on rows shaped like this
// laptop's records: shutdowns whose last minute only the hourly record and the process rows hold, sleeps, failed
// units, two days of heat where the per-second readings start halfway through the first, a week of the charger's
// moments in the log, disk traffic per program, and the live machine (a fake /proc).
void test_records_n50() {
    std::puts("records answers of the new 50");
    namespace fs = std::filesystem;
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model = em ? em : home + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: skipped");
        return;
    }
    const std::string dir = fs::temp_directory_path() / ("as-n50-" + std::to_string(getpid()));
    fs::create_directories(dir);
    auto at = [](int mday, int h, int m, int s) {
        std::tm tmv{};
        tmv.tm_year = 2026 - 1900; tmv.tm_mon = 9; tmv.tm_mday = mday; tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = s; tmv.tm_isdst = -1;
        return int64_t(std::mktime(&tmv));
    };
    const int64_t now = at(23, 13, 59, 8);
    const std::string dbp = dir + "/h.db";
    { as::Store st(dbp); }  // the schema
    {
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        auto exec = [&](const std::string& sql) {
            char* err = nullptr;
            const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
            if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
            return ok;
        };
        auto n = [](double v) { return std::to_string(v); };
        auto i = [](int64_t v) { return std::to_string(v); };
        CHECK(exec("BEGIN"));
        auto row = [&](int64_t t, const char* cat, const std::string& title, const std::string& detail) {
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(db, "INSERT INTO log(t, category, title, detail) VALUES (?, ?, ?, ?)", -1, &q, nullptr);
            sqlite3_bind_int64(q, 1, t);
            sqlite3_bind_text(q, 2, cat, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 3, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 4, detail.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        };
        auto failed = [&](int64_t t, const std::string& unit, const std::string& result) {
            row(t, "services", unit + " failed: Failed with result '" + result + "'.", "systemd: " + unit + ": Failed with result '" + result + "'.");
        };
        // Process rows; estimated: the end the recorder gave a process cut off by a stop (its own last reading).
        auto proc = [&](int64_t pid, int64_t started, int64_t ended, bool estimated, const std::string& name, const std::string& cmdline,
                        const std::string& unit) {
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(db, "INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss, end_estimated) "
                                   "VALUES (?, ?, ?, ?, '', ?, 'user', ?, 1, 'systemd', 10, 1e8, ?)", -1, &q, nullptr);
            sqlite3_bind_int64(q, 1, pid);
            sqlite3_bind_int64(q, 2, started);
            sqlite3_bind_int64(q, 3, ended);
            sqlite3_bind_text(q, 4, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 5, cmdline.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 6, unit.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(q, 7, estimated ? 1 : 0);
            sqlite3_step(q);
            sqlite3_finalize(q);
        };
        const std::string lead = "Nothing was written to the system log first, which is what happens when the laptop''s own firmware cuts the power, "
                                 "the battery dies, or the system freezes.";
        // Fri Oct 16 17:33:38: the recorder's last reading two seconds before the log's last entry.
        const int64_t fri = at(16, 17, 33, 38);
        exec("INSERT INTO events(id, start, \"end\", kinds, primary_kind, summary, peak_cpu, peak_temp, peak_fan, min_margin, guard_limit) VALUES "
             "(595, " + i(fri) + ", " + i(fri) + ", 32, 32, '17:33:38 to 17:33:38 (1 s). The machine stopped without warning on Fri Oct 16 at 17:33:38. " +
             lead + " The last reading, 1 s before, was 89 °C on the shutdown sensor (11 °C below its 100 °C limit), with the CPU 16% busy. "
             "The busiest program in its last two minutes was llama-server (34% of the CPU time).', 23.5, 97, 100, 3, 100)");
        exec("INSERT INTO event_samples(event_id, t, cpu, mem_pct, pressure, disk_bps, gpu, temp, fan, guard) VALUES "
             "(595, " + i(fri - 118) + ", 23.5, 60, 0, 0, -1, 96, 100, 97), (595, " + i(fri - 1) + ", 16, 60, 0, 0, -1, 91, 85, 89)");
        exec("INSERT INTO event_culprits(event_id, rank, kind, name, cmdline, unit, cpu_seconds, share) VALUES "
             "(595, 0, 'process', 'llama-server', '/usr/local/lib/ollama/llama-server --model /usr/share/ollama/.ollama/models/blobs/sha256-0000', 'ollama.service', 67.1, 0.338),"
             "(595, 1, 'process', 'ollama', '/usr/local/bin/ollama serve', 'ollama.service', 50.9, 0.257),"
             "(595, 2, 'process', 'python', '/home/user/.cache/venv/bin/python -I /home/user/src/compare_model.py --model m9', 'app.scope', 29.7, 0.15)");
        proc(2001, at(16, 17, 24, 16), fri - 2, true, "llama-server", "/usr/local/lib/ollama/llama-server --model x", "ollama.service");
        // That evening the lid closed and it went to sleep: no shut-off.
        row(at(16, 22, 3, 56), "session", "Lid closed", "");
        row(at(16, 22, 3, 58), "session", "Went to sleep", "");
        row(at(17, 6, 53, 0), "session", "Woke from sleep", "");
        // Sun Oct 18 22:05:39: the recorder read on to 22:06:32; the readings kept with the event end calm at 22:05:40.
        const int64_t sun = at(18, 22, 5, 39);
        exec("INSERT INTO events(id, start, \"end\", kinds, primary_kind, summary, peak_cpu, peak_temp, peak_fan, min_margin, guard_limit) VALUES "
             "(1083, " + i(at(18, 21, 57, 27)) + ", " + i(at(18, 22, 3, 49)) + ", 31, 1, 'CPU peaked at 80%, the CPU reached 95 C, mostly python', 80, 95, 85, 8, 100),"
             "(1084, " + i(sun) + ", " + i(sun) + ", 32, 32, '22:05:39 to 22:05:39 (1 s). The machine stopped without warning on Sun Oct 18 at 22:05:39. " +
             lead + " The last reading, -1 s before, was 74 °C on the shutdown sensor (26 °C below its 100 °C limit), with the CPU 1% busy. "
             "The busiest program in its last two minutes was python (93% of the CPU time).', 74.7, 95, 85, 6, 100)");
        exec("INSERT INTO event_samples(event_id, t, cpu, mem_pct, pressure, disk_bps, gpu, temp, fan, guard) VALUES "
             "(1084, " + i(sun - 120) + ", 60, 50, 0, 0, -1, 92, 85, 90), (1084, " + i(sun - 69) + ", 74.7, 50, 0, 0, -1, 95, 85, 94),"
             "(1084, " + i(sun) + ", 2, 50, 0, 0, -1, 76, 85, 75), (1084, " + i(sun + 1) + ", 1, 50, 0, 0, -1, 74, 85, 74)");
        exec("INSERT INTO event_culprits(event_id, rank, kind, name, cmdline, unit, cpu_seconds, share) VALUES "
             "(1084, 0, 'process', 'python', '/home/user/.cache/venv/bin/python -I vectors.py build', 'vectors.service', 317, 0.931),"
             "(1084, 1, 'process', 'plasmashell', '/usr/bin/plasmashell --no-respawn', 'plasma-plasmashell.service', 5.1, 0.015)");
        proc(3001, at(18, 21, 57, 38), at(18, 22, 6, 32), true, "python", "/home/user/.cache/venv/bin/python -I vectors.py build", "vectors.service");
        proc(3002, at(18, 19, 0, 0), at(18, 22, 6, 32), true, "plasmashell", "/usr/bin/plasmashell --no-respawn", "plasma-plasmashell.service");
        proc(3003, at(18, 20, 0, 0), at(18, 21, 0, 0), false, "firefox", "/usr/lib/firefox/firefox", "app-firefox.scope");
        const int64_t h22 = at(18, 22, 0, 0);
        exec("INSERT INTO samples_hourly(hour_ts, metric, n, med, p05, p95, max, min, on_battery_frac) VALUES "
             "(" + i(h22) + ", 'cpu', 393, 19.6, 1, 76.8, 100, 0, 0), (" + i(h22) + ", 'temp', 393, 75, 60, 95, 96, 55, 0),"
             "(" + i(h22) + ", 'fan', 393, 80, 45, 85, 100, 45, 0)");
        // Mon Oct 19 22:44:30: the recorder read on to 22:44:45.
        const int64_t mon = at(19, 22, 44, 30);
        exec("INSERT INTO events(id, start, \"end\", kinds, primary_kind, summary, peak_cpu, peak_temp, peak_fan, min_margin, guard_limit) VALUES "
             "(1153, " + i(mon) + ", " + i(mon) + ", 32, 32, '22:44:30 to 22:44:30 (1 s). The machine stopped without warning on Mon Oct 19 at 22:44:30. " +
             lead + " The last reading, -1 s before, was 96 °C on the shutdown sensor (4 °C below its 100 °C limit), with the CPU 37% busy. "
             "The busiest program in its last two minutes was python (25% of the CPU time).', 59.3, 98, 100, 3, 100)");
        exec("INSERT INTO event_samples(event_id, t, cpu, mem_pct, pressure, disk_bps, gpu, temp, fan, guard) VALUES "
             "(1153, " + i(mon - 1) + ", 28, 60, 0, 0, -1, 91, 100, 93), (1153, " + i(mon + 1) + ", 37.4, 60, 0, 0, -1, 95, 100, 96)");
        exec("INSERT INTO event_culprits(event_id, rank, kind, name, cmdline, unit, cpu_seconds, share) VALUES "
             "(1153, 0, 'process', 'python', '/home/user/.cache/venv/bin/python -I rerank.py', 'app.scope', 133.5, 0.25),"
             "(1153, 1, 'process', 'firefox', '/usr/lib/firefox/firefox', 'app-firefox.scope', 108, 0.20)");
        proc(3101, at(19, 20, 0, 0), mon + 15, true, "firefox", "/usr/lib/firefox/firefox", "app-firefox.scope");
        // A program whose name is an everyday word ("it cut out" reads "cut").
        proc(3102, at(19, 12, 0, 0), at(19, 12, 0, 1), false, "cut", "cut -d: -f1", "app.scope");
        // Q15: Wednesday night's sleep into Thursday, then two on Thursday.
        row(at(21, 21, 36, 15), "session", "Lid closed", "");
        row(at(21, 21, 36, 17), "session", "Went to sleep", "");
        row(at(22, 6, 45, 28), "session", "Woke from sleep", "");
        row(at(22, 11, 7, 40), "session", "Lid closed", "");
        row(at(22, 11, 7, 41), "session", "Went to sleep", "");
        row(at(22, 12, 26, 13), "session", "Lid opened", "");
        row(at(22, 12, 26, 15), "session", "Woke from sleep", "");
        row(at(22, 13, 7, 11), "session", "Power button pressed", "");
        row(at(22, 14, 54, 43), "session", "Lid closed", "");
        row(at(22, 14, 54, 45), "session", "Went to sleep", "");
        row(at(22, 14, 54, 58), "session", "Woke from sleep", "");
        // Q22: seven units failed in the last two days.
        failed(at(21, 20, 20, 47), "vte-spawn-0000aaaa-0000-0000-0000-000000000001.scope", "oom-kill");
        failed(at(21, 20, 25, 27), "run-p1000-i2000.scope", "oom-kill");
        failed(at(22, 7, 23, 31), "ollama.service", "oom-kill");
        failed(at(22, 10, 11, 49), "vte-spawn-0000aaaa-0000-0000-0000-000000000002.scope", "oom-kill");
        failed(at(22, 10, 53, 53), "ollama.service", "oom-kill");
        failed(at(22, 13, 7, 20), "app-chat-sync@0.service", "exit-code");
        failed(at(22, 13, 7, 20), "app-wallet@0.service", "exit-code");
        for (int k = 0; k < 6; ++k) failed(at(22, 15, 49, 40) + k * 1200, "app-splash@" + std::to_string(k + 2) + ".service", "exit-code");
        failed(at(23, 8, 13, 4), "cpu-burn-test.service", "exit-code");
        failed(at(23, 9, 9, 32), "app-splash@8.service", "exit-code");
        // Q21: a program that crashes on day after day, against one-day bursts of more crashes.
        for (const auto& [t, prog] : std::vector<std::pair<int64_t, const char*>>{
                 {at(17, 19, 46, 26), "ktorrent"}, {at(19, 18, 3, 14), "ktorrent"}, {at(21, 20, 26, 6), "ktorrent"}, {at(22, 22, 56, 30), "ktorrent"},
                 {at(22, 14, 35, 12), "ask"}, {at(22, 14, 38, 27), "ask"}, {at(22, 14, 38, 30), "ask"}, {at(22, 14, 40, 30), "ask"},
                 {at(23, 7, 57, 19), "sim"}, {at(23, 7, 57, 25), "sim"}, {at(23, 7, 57, 30), "sim"}})
            row(t, "problems", std::string(prog) + " crashed", std::string("systemd-coredump: Process 1 (") + prog + ") of user 1000 dumped core.");
        // Q43: the charger's moments over the week, as LogWatch writes them; the level at a plug-in is the low point
        // of the spell on battery before it.
        for (const auto& [t, title] : std::vector<std::pair<int64_t, const char*>>{
                 {at(16, 15, 13, 58), "Unplugged from power at 68% battery"}, {at(16, 15, 57, 55), "Plugged into power at 10% battery"},
                 {at(16, 19, 43, 1), "Unplugged from power at 57% battery"}, {at(16, 20, 20, 26), "Plugged into power at 7% battery"},
                 {at(17, 7, 1, 56), "Unplugged from power at 99% battery"}, {at(17, 7, 48, 31), "Plugged into power at 19% battery"},
                 {at(18, 19, 7, 44), "Unplugged from power at 100% battery"}, {at(18, 19, 58, 17), "Plugged into power at 18% battery"},
                 {at(19, 8, 20, 12), "Unplugged from power at 100% battery"}, {at(19, 9, 2, 46), "Plugged into power at 37% battery"},
                 {at(19, 9, 11, 10), "Unplugged from power at 45% battery"}, {at(19, 9, 15, 45), "Plugged into power at 38% battery"},
                 {at(21, 20, 17, 9), "Unplugged from power at 98% battery"}, {at(21, 21, 16, 5), "Plugged into power at 36% battery"},
                 {at(22, 8, 37, 49), "Unplugged from power at 98% battery"}, {at(22, 9, 7, 16), "Plugged into power at 75% battery"},
                 {at(23, 9, 11, 35), "Unplugged from power at 100% battery"}, {at(23, 10, 7, 38), "Plugged into power at 56% battery"}})
            row(t, "session", title, "");
        // Q25 and Q43: per-second readings (one every 10 s here) from Thu 13:32: Thursday afternoon at most 88 C,
        // today up to 97 C at 07:58; the battery on mains but for today's 09:11 to 10:07 spell, down to 56%.
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO samples(t, cpu, temp, fan, battery_pct, on_ac, disk_r, disk_w) VALUES (?, ?, ?, ?, ?, ?, ?, ?)", -1, &st, nullptr);
        for (int64_t t = at(22, 13, 32, 20); t < now - 30; t += 10) {
            const bool today = t >= at(23, 0, 0, 0);
            double temp = today ? 52 : 48, fan = today ? 50 : 45;
            if (t >= at(22, 15, 0, 0) && t < at(22, 15, 1, 0)) temp = 88, fan = 80;
            if (t >= at(23, 7, 58, 0) && t < at(23, 7, 58, 10)) temp = 97, fan = 100;
            const int64_t off = at(23, 9, 11, 35), on = at(23, 10, 7, 38);
            const double pct = t < off ? 100 : t < on ? 100 - 44.0 * double(t - off) / double(on - off) : std::min(100.0, 56 + double(t - on) / 60.0);
            const double v[] = {double(t), 5, temp, fan, pct, t >= off && t < on ? 0.0 : 1.0, today ? 4e6 : 1e5, today ? 3e5 : 1e4};
            for (int k = 0; k < 8; ++k) sqlite3_bind_double(st, k + 1, v[k]);
            sqlite3_step(st);
            sqlite3_reset(st);
        }
        sqlite3_finalize(st);
        // Thursday before the per-second readings: the hourly records, asleep until 06:45 and 11:07 to 12:26; at
        // 95 C at most in the 09:00 hour.
        std::string hourly = "INSERT INTO samples_hourly(hour_ts, metric, n, med, p05, p95, max, min, on_battery_frac) VALUES ";
        for (int h = 6; h <= 13; ++h) {
            if (h == 11) continue;
            const double peak = h == 9 ? 95 : 70, med = h == 9 || h == 10 ? 66 : 55;
            hourly += std::string(h == 6 ? "" : ", ") + "(" + i(at(22, h, 0, 0)) + ", 'temp', 3000, " + n(med) + ", 45, 80, " + n(peak) + ", 44, 0), (" +
                      i(at(22, h, 0, 0)) + ", 'fan', 3000, 45, 45, 80, 85, 45, 0)";
        }
        CHECK(exec(hourly));
        // Q38: disk traffic today, per program reads and writes together (a parent's with its finished children's).
        exec("INSERT INTO events(id, start, \"end\", kinds, primary_kind, summary, peak_disk) VALUES "
             "(2000, " + i(at(23, 9, 5, 27)) + ", " + i(at(23, 9, 25, 24)) + ", 4, 4, 'Disk traffic peaked at 1.7 GB/s', 1.7e9)");
        exec("INSERT INTO event_culprits(event_id, rank, kind, name, cmdline, unit, cpu_seconds, share, io_bytes) VALUES "
             "(2000, 0, 'process', 'claude', 'claude --resume', 'app.scope', 30, 0.1, 27.1e9),"
             "(2000, 1, 'process', 'python3', 'python3 -I runner.py', 'app.scope', 20, 0.1, 16.3e9)");
        CHECK(exec("COMMIT"));
        sqlite3_close(db);
    }
    // The live machine: memory half used, four programs; the system's own low-battery level. (ask's own process is
    // left out of the holders by the process reader's self flag, which only the real /proc sets.)
    Tree t;
    t.write("proc/stat", "cpu 100 0 100 800 0 0 0 0\nbtime 1000\n");
    t.write("proc/meminfo", "MemTotal: 16000000 kB\nMemAvailable: 8000000 kB\nSwapTotal: 8000000 kB\nSwapFree: 400000 kB\n");
    t.write("proc/swaps", "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n/dev/zram0                              partition\t8000000\t\t7600000\t\t100\n");
    auto live = [&](int pid, const char* name, uint64_t pages) {
        t.write("proc/" + std::to_string(pid) + "/stat", stat_line(pid, name, 'S', 1, 100, 0, 500, pages));
        t.write("proc/" + std::to_string(pid) + "/cmdline", std::string(name) + std::string(1, '\0'));
    };
    live(4101, "claude", 131000);
    live(4102, "plasmashell", 87000);
    live(4103, "activityscope", 48000);
    live(4104, "kwin_wayland", 33000);
    t.write("etc/UPower/UPower.conf", "[UPower]\nUsePercentageForPolicy=true\nPercentageLow=20.0\nPercentageCritical=5.0\n");
    char exe[4096];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string bin = len > 0 ? std::string(exe, std::size_t(len)) : std::string("./ascore_tests");
    const std::string cache = bin.substr(0, bin.rfind('/')) + "/kinds-cache";
    fs::create_directories(cache);
    auto ask_as = [&](const std::string& kind, const char* q) {
        as::Ask a;
        a.open(dbp);
        a.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                        "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
        a.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        a.load_facts({"facts/base.tsv"});
        a.load_kinds("data/kinds", cache, model);
        a.set_roots(t.roots());
        a.force_reading(kind);
        return a.ask(q, now);
    };
    auto has = [](const as::AskResult& r, const char* text) {
        const bool ok = r.answer.find(text) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  no \"%s\" in [%s] %s\n", text, r.branch.c_str(), r.answer.c_str());
        return ok;
    };
    auto lacks = [](const as::AskResult& r, const char* text) {
        const bool ok = r.answer.find(text) == std::string::npos;
        if (!ok) std::fprintf(stderr, "  \"%s\" in [%s] %s\n", text, r.branch.c_str(), r.answer.c_str());
        return ok;
    };

    // Q1: a why gets no "Yes"; the stop is placed at the recorder's last reading; the conditions before it: the
    // power source, the last two minutes kept and the hour's highs past them; the busiest program by its script.
    as::AskResult why = ask_as("crashes.yesno.window.system", "why did my laptop just turn off by itself sunday night");
    CHECK(lacks(why, "Yes") && lacks(why, "-1 s"));
    CHECK(has(why, "22:06:32") && has(why, "vectors.py") && has(why, "mains"));
    CHECK(has(why, "96") && has(why, "6 C below"));
    as::AskResult what = ask_as("crashes.yesno.window.system", "what made the laptop power off on sunday night");
    CHECK(lacks(what, "Yes") && has(what, "vectors.py"));
    // A yes/no question keeps its "Yes"; a stop the recorder did not outlast keeps the log's time.
    CHECK(has(ask_as("crashes.yesno.window.system", "did the laptop shut off by itself on sunday night"), "Yes"));
    as::AskResult last = ask_as("crashes.yesno.window.system", "did the laptop shut off by itself last friday");
    CHECK(has(last, "17:33:38") && lacks(last, "read on") && has(last, "compare_model.py"));
    // Q2: a count, this week: once, Monday's, its last reading 15 s after the log's line.
    as::AskResult week = ask_as("crashes.yesno.window.system", "how many times has the laptop shut down without warning this week");
    CHECK(has(week, "once") && has(week, "Mon 22:44") && lacks(week, "Yes") && has(week, "22:44:45"));

    // Q33: "when the machine shut off" places the question at the stop on the day asked, though it came just before
    // the part of the day named; what ran then is told from that moment's records.
    as::AskResult off = ask_as("slow.which.window", "what was running when the machine shut off last friday evening");
    CHECK(has(off, "17:33") && has(off, "llama-server") && has(off, "compare_model.py") && lacks(off, "quiet"));
    as::AskResult off2 = ask_as("slow.which.window", "which programs were open when the laptop cut out on october 16 in the evening");
    CHECK(has(off2, "17:33") && has(off2, "llama-server"));

    // The first 50's Q34: "before it cut out" is the stop, though "cut" is also a program's name.
    as::AskResult before = ask_as("diagnose.list.window", "what was the laptop doing right before it cut out on monday night");
    CHECK(has(before, "Right before Mon 22:44"));

    // Q41: a story of a window tells its stops without a clean shutdown, with their times.
    as::AskResult wk = ask_as("diagnose.list.window", "what did the laptop get up to since saturday");
    CHECK(has(wk, "Sun 22:05") && has(wk, "Mon 22:44") && has(wk, "without a clean shutdown"));
    as::AskResult sund = ask_as("diagnose.list.window", "what happened on sunday");
    CHECK(has(sund, "Sun 22:05") && has(sund, "without a clean shutdown"));

    // Q21: the program that crashed on the most separate days is singled out, beside the count.
    as::AskResult keeps = ask_as("crashes.which.window", "what keeps crashing on this machine since last friday");
    CHECK(has(keeps, "ktorrent") && has(keeps, "four separate days"));
    as::AskResult over = ask_as("crashes.which.window", "which program crashes over and over this week");
    CHECK(has(over, "ktorrent") && has(over, "three separate days"));
    // One day: nothing is singled out by days.
    CHECK(lacks(ask_as("crashes.which.window", "what crashed the most yesterday"), "separate days"));

    // Q15: every sleep in the day asked, not only the latest; with no day named, the latest.
    as::AskResult sl = ask_as("battery.sleep.when.window", "when did the laptop go to sleep yesterday and how long for");
    CHECK(has(sl, "11:07") && has(sl, "12:26") && has(sl, "14:54") && has(sl, "one hour and 18 minutes"));
    as::AskResult sl2 = ask_as("battery.sleep.when.window", "at what times did the laptop sleep on thursday");
    CHECK(has(sl2, "11:07") && has(sl2, "14:54"));
    as::AskResult latest = ask_as("battery.sleep.when.window", "when did the laptop last go to sleep");
    CHECK(has(latest, "14:54") && lacks(latest, "11:07"));

    // Q22: one or two left past six are named, not "and 1 more".
    as::AskResult sv = ask_as("startup.which.window", "which services failed in the last two days");
    CHECK(has(sv, "cpu-burn-test.service") && lacks(sv, "1 more"));
    as::AskResult sv2 = ask_as("startup.which.window", "what units have failed since wednesday afternoon");
    CHECK(has(sv2, "cpu-burn-test.service") && lacks(sv2, "1 more"));

    // Q25: Thursday before 13:32 from the hourly records: its peak was 95 C at 09:00, not the afternoon's 88 C.
    as::AskResult hot = ask_as("heat.compare.window", "was it hotter today or yesterday");
    CHECK(has(hot, "97") && has(hot, "95") && lacks(hot, "88") && has(hot, "hourly"));
    as::AskResult hot2 = ask_as("heat.compare.window", "did the laptop run hotter yesterday or today");
    CHECK(has(hot2, "97") && has(hot2, "95") && lacks(hot2, "88"));

    // Q43: the lowest level over the whole week, from the log where the readings don't reach, with a yes or no
    // against the level the system itself calls low.
    as::AskResult bat = ask_as("battery.value.window", "did the battery get low at all this week");
    CHECK(has(bat, "No") && has(bat, "36%") && has(bat, "Wed 21:16") && has(bat, "20%"));
    as::AskResult bat7 = ask_as("battery.value.window", "did the battery get low at all in the last 7 days");
    CHECK(has(bat7, "Yes") && has(bat7, "7%") && has(bat7, "Oct 16 20:20") && has(bat7, "19%"));
    as::AskResult bat2 = ask_as("battery.value.window", "how low did my battery go this week");
    CHECK(has(bat2, "lowest 36%") && lacks(bat2, "lowest 56%"));

    // Q38: per program the records hold reads and writes together: said, not a top writer.
    as::AskResult wr = ask_as("disk.which.window", "which program wrote the most to disk today");
    CHECK(has(wr, "reads and writes together"));
    as::AskResult wr2 = ask_as("disk.which.window", "what was writing to the disk the most this morning");
    CHECK(has(wr2, "reads and writes together"));

    // Q10: the top few holders of memory, not one.
    as::AskResult mem = ask_as("memory.which.now", "whats eating my memory right now");
    CHECK(has(mem, "Claude holds the most") && has(mem, "plasmashell") && has(mem, "activityscope") && lacks(mem, "kwin_wayland"));
    as::AskResult mem2 = ask_as("memory.which.now", "what is using all my ram at the moment");
    CHECK(has(mem2, "Claude") && has(mem2, "plasmashell"));

    // Q30: about now: the live figures and a verdict on them first, then what the records hold.
    as::AskResult ok = ask_as("diagnose.cause.now", "is there anything wrong with the laptop at the moment");
    CHECK(has(ok, "Right now") && has(ok, "memory 50% used") && has(ok, "swap 95% full"));
    as::AskResult ok2 = ask_as("diagnose.cause.now", "is my laptop ok right now");
    CHECK(has(ok2, "Right now") && has(ok2, "memory 50% used"));
    fs::remove_all(dir);
}

// The traced 95's live area: time at or over a level, traffic sent, a program with the runners it starts, the spans of a
// "while it ran", memory pressure, the GPU's temperature asked of the past, the disk at a moment with no event, the fan
// over a night; and live: which program works the disk, which programs are online, whether the CPU is hot, and two
// questions in one.
void test_live_t95() {
    std::puts("live answers and measures of the traced 95");
    namespace fs = std::filesystem;
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model = em ? em : home + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: skipped");
        return;
    }
    const std::string dir = fs::temp_directory_path() / ("as-t95live-" + std::to_string(getpid()));
    fs::create_directories(dir);
    auto at = [](int mday, int h, int m, int s) {
        std::tm tmv{};
        tmv.tm_year = 2026 - 1900; tmv.tm_mon = 9; tmv.tm_mday = mday; tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = s; tmv.tm_isdst = -1;
        return int64_t(std::mktime(&tmv));
    };
    const int64_t now = at(23, 13, 59, 8);
    const int64_t first = at(22, 13, 32, 20), last = now - 30;  // the per-second readings, one a second
    const std::string dbp = dir + "/h.db";
    { as::Store st(dbp); }
    {
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        auto exec = [&](const std::string& sql) {
            char* err = nullptr;
            const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
            if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
            CHECK(ok);
        };
        auto n = [](double v) { return std::to_string(v); };
        auto i = [](int64_t v) { return std::to_string(v); };
        exec("BEGIN");
        // The readings: the CPU over 80% three times this morning; the fan loud twice last night; memory pressure
        // 6.8% at 23:35; the disks around 2am (mandb at 01:29, snapper's cleanup from 01:40, a script at 02:41); an
        // upload of 1 GB at 21:00; the battery on its own from 09:11 to 10:07 today.
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO samples(t, cpu, temp, fan, gpu, mem_pct, pressure, disk_r, disk_w, net_rx, net_tx, battery_pct, on_ac, cores) "
                               "VALUES (?, ?, ?, ?, 0, 40, ?, ?, ?, ?, ?, ?, ?, '1,1,1,1,1,1,1,1,1,1,1,1')", -1, &st, nullptr);
        const int64_t off = at(23, 9, 11, 35), on = at(23, 10, 7, 38);
        for (int64_t t = first; t < last; ++t) {
            double cpu = 5, fan = 45, pressure = 0, rd = 1e4, wr = 2e4, tx = 1000;
            if (t >= at(23, 7, 38, 0) && t < at(23, 7, 39, 0)) cpu = 100;
            if (t >= at(23, 9, 8, 0) && t < at(23, 9, 11, 0)) cpu = t == at(23, 9, 9, 30) ? 100 : 95;
            if (t >= at(23, 11, 18, 0) && t < at(23, 11, 18, 10)) cpu = 90;
            if ((t >= at(22, 23, 32, 30) && t < at(22, 23, 37, 48)) || (t >= at(23, 0, 2, 0) && t < at(23, 0, 6, 51))) fan = 80;
            if (t >= at(22, 19, 41, 31) && t < at(22, 19, 41, 54)) fan = 72;
            if (t >= at(22, 23, 35, 7) && t < at(22, 23, 35, 14)) pressure = 6.8;
            if (t >= at(22, 14, 19, 50) && t < at(22, 14, 20, 0)) pressure = 3;
            if (t >= at(22, 20, 0, 0) && t < at(22, 20, 10, 0) && (t - at(22, 20, 0, 0)) % 30 == 0) pressure = 2.5;  // blips, 30 s apart
            if (t == at(23, 1, 29, 36)) rd = 15.5e6;
            if (t >= at(23, 1, 40, 0) && t < at(23, 2, 41, 0)) wr = 1e5;
            if (t == at(23, 2, 41, 8)) rd = 37.6e6, wr = 8e6;
            if (t >= at(22, 21, 0, 0) && t < at(22, 21, 1, 40)) tx = 1e7;
            if (t >= at(22, 15, 0, 0) && t < at(22, 16, 0, 0)) cpu = 30;  // an unknown worker's steady hour
            const double pct = t < off ? 100 : t < on ? 100 - 44.0 * double(t - off) / double(on - off) : std::min(100.0, 56 + double(t - on) / 60.0);
            const double v[] = {double(t), cpu, 46, fan, pressure, rd, wr, 2000, tx, pct, t >= off && t < on ? 0.0 : 1.0};
            for (int k = 0; k < 11; ++k) sqlite3_bind_double(st, k + 1, v[k]);
            sqlite3_step(st);
            sqlite3_reset(st);
        }
        sqlite3_finalize(st);
        // Thursday before the per-second readings (awake from 06:45, asleep 11:07 to 12:26): the CPU busiest and memory
        // pressure highest in the 10:00 hour. Sunday: the GPU busy in the evening.
        std::string hourly = "INSERT INTO samples_hourly(hour_ts, metric, n, med, p05, p95, max, min, on_battery_frac) VALUES ";
        for (int h = 6; h <= 13; ++h) {
            if (h == 11) continue;
            const std::string t = i(at(22, h, 0, 0));
            hourly += std::string(h == 6 ? "" : ", ") + "(" + t + ", 'cpu', 3000, 5, 1, " + (h == 10 ? "85, 95" : "20, 40") + ", 1, 0), (" + t +
                      ", 'pressure', 3000, 0, 0, " + (h == 10 ? "5, 14.9" : h == 12 ? "2, 11" : "1, 5.3") + ", 0, 0), (" + t +
                      ", 'mem_pct', 3000, 45, 40, 60, " + (h == 7 || h == 10 ? "90" : "70") + ", 40, 0)";
        }
        for (int h = 0; h <= 22; ++h)
            hourly += ", (" + i(at(18, h, 0, 0)) + ", 'gpu', 3600, " + (h == 19 ? "94" : "0") + ", 0, " + (h >= 17 && h <= 20 ? "100" : "20") + ", " +
                      (h == 19 ? "100" : "60") + ", 0, 0)";
        exec(hourly);
        // Memory reached 90% twice yesterday: the model runner ollama starts held the most.
        exec("INSERT INTO events(id, start, \"end\", kinds, primary_kind, summary, peak_mem, peak_cpu) VALUES "
             "(100, " + i(at(22, 7, 13, 30)) + ", " + i(at(22, 7, 27, 39)) + ", 2, 2, 'Memory use reached 90%', 90, 40), "
             "(101, " + i(at(22, 10, 42, 17)) + ", " + i(at(22, 10, 56, 31)) + ", 2, 2, 'Memory use reached 90%', 90, 39)");
        const std::string runner = "/usr/local/lib/ollama/llama-server --model m";
        exec("INSERT INTO event_culprits(event_id, rank, kind, name, cmdline, unit, cpu_seconds, share, peak_rss) VALUES "
             "(100, 0, 'process', 'llama-server', '" + runner + "', 'ollama.service', 30, 0.2, 6.0e9),"
             "(100, 1, 'process', 'unllm', 'unllm --branch', 'run-x.scope', 20, 0.1, 2.4e9),"
             "(100, 2, 'process', 'ollama', '/usr/local/bin/ollama serve', 'ollama.service', 5, 0.05, 3.8e8),"
             "(101, 0, 'process', 'llama-server', '" + runner + "', 'ollama.service', 30, 0.2, 7.52e9),"
             "(101, 1, 'process', 'exe', 'unllm --serve', 'app-unllm.service', 25, 0.15, 4.76e9),"
             "(101, 2, 'process', 'ollama', '/usr/local/bin/ollama serve', 'ollama.service', 5, 0.05, 3.8e8)");
        auto proc = [&](int pid, int64_t started, int64_t ended, const char* name, const char* cmdline, const char* user, const char* unit, int ppid,
                        double cpu, double rss) {
            exec("INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss, end_estimated) VALUES(" +
                 i(pid) + "," + i(started) + "," + (ended ? i(ended) : std::string("NULL")) + ",'" + name + "','','" + cmdline + "','" + user + "','" +
                 unit + "'," + i(ppid) + ",''," + n(cpu) + "," + n(rss) + ",0)");
        };
        // ollama's service with its runners, and two ollama commands typed in a terminal while it ran.
        proc(2990, at(16, 17, 35, 0), at(18, 22, 6, 32), "ollama", "/usr/local/bin/ollama serve", "ollama", "ollama.service", 1, 900, 4e8);
        proc(3000, at(19, 22, 47, 0), 0, "ollama", "/usr/local/bin/ollama serve", "ollama", "ollama.service", 1, 800, 3.8e8);
        proc(3100, at(22, 7, 10, 0), at(22, 7, 30, 0), "llama-server", runner.c_str(), "ollama", "ollama.service", 3000, 300, 6e9);
        proc(3101, at(22, 10, 40, 0), at(22, 10, 57, 0), "llama-server", runner.c_str(), "ollama", "ollama.service", 3000, 300, 7.52e9);
        proc(500, at(22, 13, 0, 0), 0, "claude", "claude --resume", "user", "vte-spawn-1.scope", 400, 900, 7e8);
        proc(3200, at(22, 15, 0, 0), at(22, 15, 10, 0), "ollama", "ollama ps", "user", "vte-spawn-1.scope", 500, 1, 3e7);
        proc(3201, at(22, 16, 0, 0), at(22, 16, 10, 0), "ollama", "ollama run m", "user", "vte-spawn-1.scope", 500, 1, 3e7);
        proc(1400, at(22, 10, 30, 0), at(22, 11, 0, 0), "exe", "unllm --serve", "user", "app-unllm.service", 1, 300, 4.76e9);
        // Around 2am: mandb, snapper's cleanup and a script; root's sleeps between them.
        proc(6003, at(23, 1, 29, 36), at(23, 1, 29, 39), "mandb", "/usr/bin/mandb --quiet", "root", "man-db.service", 1, 0.6, 3e7);
        proc(6001, at(23, 1, 40, 6), at(23, 2, 41, 15), "snapperd", "/usr/sbin/snapperd", "root", "snapperd.service", 1, 6.3, 3e7);
        proc(6002, at(23, 1, 40, 6), at(23, 2, 40, 15), "systemd-helper", "/usr/lib/snapper/systemd-helper --cleanup", "root",
             "snapper-cleanup.service", 1, 3.5, 3e7);
        for (int k = 0; k < 5; ++k) proc(7001 + k, at(23, 2, 0, 3 * k), at(23, 2, 0, 3 * k + 2), "sleep", "sleep 1", "root", "fanwatch.service", 1, 0, 1e6);
        proc(5005, at(23, 2, 41, 7), at(23, 2, 50, 37), "python", "python -m scan scan", "user", "run-scan.scope", 1, 20.9, 1e8);
        // The build and the tests that ran while the fan was loud.
        proc(7100, at(22, 23, 32, 0), at(22, 23, 38, 0), "cc1plus", "cc1plus -quiet x.cpp", "user", "vte-spawn-1.scope", 500, 150, 3e8);
        proc(7200, at(23, 0, 1, 0), at(23, 0, 7, 0), "python3", "python3 -I run_tests.py", "user", "vte-spawn-1.scope", 500, 200, 2e8);
        // Thursday afternoon: a program never seen before holds a quarter of the machine for an hour.
        proc(7300, at(22, 15, 0, 0), at(22, 16, 0, 0), "worker-x", "/home/user/.cache/w/worker-x --threads 4", "user", "run-w.scope", 1, 12000, 2e8);
        auto minutes = [&](int64_t from, int64_t to, int pid, const char* name, const char* cmdline, const char* unit, double cpu, double io) {
            for (int64_t m = from; m < to; m += 60)
                exec("INSERT INTO minute_top(minute, pid, start_ticks, name, cmdline, unit, cpu_seconds, peak_rss, io_bytes, exited) VALUES(" +
                     i(m) + "," + i(pid) + ",0,'" + name + "','" + cmdline + "','" + unit + "'," + n(cpu) + ",1e8," + n(io) + ",0)");
        };
        minutes(at(22, 13, 33, 0), at(23, 13, 58, 0), 500, "claude", "claude --resume", "vte-spawn-1.scope", 1, 1e4);
        minutes(at(22, 14, 0, 0), at(22, 15, 0, 0), 3000, "ollama", "/usr/local/bin/ollama serve", "ollama.service", 2, 0);
        minutes(at(22, 23, 32, 0), at(22, 23, 38, 0), 7100, "cc1plus", "cc1plus -quiet x.cpp", "vte-spawn-1.scope", 30, 0);
        minutes(at(23, 0, 1, 0), at(23, 0, 7, 0), 7200, "python3", "python3 -I run_tests.py", "vte-spawn-1.scope", 40, 0);
        minutes(at(22, 15, 0, 0), at(22, 16, 0, 0), 7300, "worker-x", "/home/user/.cache/w/worker-x --threads 4", "run-w.scope", 200, 0);
        minutes(at(23, 1, 40, 0), at(23, 2, 42, 0), 6001, "snapperd", "/usr/sbin/snapperd", "snapperd.service", 0.1, 0);
        minutes(at(23, 2, 41, 0), at(23, 2, 43, 0), 5005, "python", "python -m scan scan", "run-scan.scope", 10, 3.85e6);
        // Who is connected now: the browser to three places, the assistant to one.
        exec("INSERT INTO connections(program, exe, remote, port, proto, first_seen, last_seen) VALUES "
             "('firefox', '/usr/lib/firefox/firefox', '203.0.113.1', 443, 'tcp', " + i(now - 3600) + ", " + i(now - 30) + "),"
             "('firefox', '/usr/lib/firefox/firefox', '203.0.113.2', 443, 'tcp', " + i(now - 3600) + ", " + i(now - 30) + "),"
             "('firefox', '/usr/lib/firefox/firefox', '203.0.113.3', 443, 'tcp', " + i(now - 600) + ", " + i(now - 20) + "),"
             "('claude', '/usr/bin/claude', '198.51.100.7', 443, 'tcp', " + i(now - 7200) + ", " + i(now - 10) + ")");
        exec("INSERT INTO log(t, category, title, detail) VALUES (" + i(off) + ", 'session', 'Unplugged from power at 100% battery', ''), (" +
             i(on) + ", 'session', 'Plugged into power at 56% battery', '')");
        exec("COMMIT");
        sqlite3_close(db);
    }
    // The live machine: the CPU at 57 C, the battery full on the charger and worn to 61%, online through Wi-Fi, a disk.
    Tree t;
    t.write("proc/stat", "cpu 100 0 100 800 0 0 0 0\nbtime 1000\n");
    t.write("proc/meminfo", "MemTotal: 16000000 kB\nMemAvailable: 8000000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
    t.write("proc/net/dev", "Inter-|   Receive |  Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets\n"
                            "    lo: 100 1 0 0 0 0 0 0 100 1 0 0 0 0 0 0\n  wlan0: 5000000 100 0 0 0 0 0 0 2000000 100 0 0 0 0 0 0\n");
    t.write("sys/class/net/wlan0/wireless/.keep", "");
    t.write("sys/class/net/wlan0/operstate", "up\n");
    t.write("sys/class/hwmon/hwmon0/name", "coretemp\n");
    t.write("sys/class/hwmon/hwmon0/temp1_label", "Package id 0\n");
    t.write("sys/class/hwmon/hwmon0/temp1_input", "57000\n");
    t.write("sys/class/power_supply/AC/type", "Mains\n");
    t.write("sys/class/power_supply/AC/online", "1\n");
    for (const auto& [f, v] : std::vector<std::pair<const char*, const char*>>{{"type", "Battery"}, {"status", "Full"}, {"capacity", "100"},
                                                                              {"charge_full", "3827000"}, {"charge_full_design", "6250000"},
                                                                              {"charge_now", "3827000"}, {"cycle_count", "0"}})
        t.write(std::string("sys/class/power_supply/BAT0/") + f, std::string(v) + "\n");
    t.write("sys/block/nvme0n1/size", "1000000000\n");
    // Two programs of mine: a backup tool writing 5 MB/s and reading 1 MB/s, an editor writing a little. A writer
    // thread moves their counters and the disk's on, so the live rates are rates.
    for (const auto& [pid, name] : std::vector<std::pair<int, const char*>>{{4201, "backup-tool"}, {4202, "editor"}}) {
        t.write("proc/" + std::to_string(pid) + "/stat", stat_line(pid, name, 'S', 1, 100, 0, 500, 1000));
        t.write("proc/" + std::to_string(pid) + "/cmdline", std::string(name) + std::string(1, '\0'));
    }
    std::atomic<bool> moving{true};
    auto put = [&](const std::string& rel, const std::string& text) {
        const std::string path = t.root + "/" + rel;
        std::ofstream(path + ".new") << text;
        std::rename((path + ".new").c_str(), path.c_str());
    };
    auto counters = [&](double secs) {
        const uint64_t rd = uint64_t(1e6 * secs), wr = uint64_t(5e6 * secs), small = uint64_t(2e4 * secs);
        put("proc/4201/io", "rchar: 0\nwchar: 0\nread_bytes: " + std::to_string(rd) + "\nwrite_bytes: " + std::to_string(wr) + "\n");
        put("proc/4202/io", "rchar: 0\nwchar: 0\nread_bytes: 0\nwrite_bytes: " + std::to_string(small) + "\n");
        put("proc/diskstats", " 259       0 nvme0n1 100 0 " + std::to_string((rd + 1000000) / 512) + " 10 100 0 " +
                                  std::to_string((wr + small + 1000000) / 512) + " 10 0 " + std::to_string(uint64_t(secs * 500)) + " 20 0 0 0 0\n");
    };
    counters(0);
    std::thread writer([&] {
        const auto t0 = std::chrono::steady_clock::now();
        while (moving) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            counters(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
    });
    char exe[4096];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string bin = len > 0 ? std::string(exe, std::size_t(len)) : std::string("./ascore_tests");
    const std::string cache = bin.substr(0, bin.rfind('/')) + "/kinds-cache";
    fs::create_directories(cache);
    auto ask_as = [&](const std::string& kind, const char* q) {
        as::Ask a;
        a.open(dbp);
        a.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                        "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
        a.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        a.load_facts({"facts/base.tsv"});
        a.load_kinds("data/kinds", cache, model);
        a.set_roots(t.roots());
        if (!kind.empty()) a.force_reading(kind);
        as::AskResult r = a.ask(q, now);
        std::fprintf(stderr, "  [%s] %s -> %s\n", r.branch.c_str(), q, r.answer.c_str());
        return r;
    };
    auto has = [](const as::AskResult& r, const char* text) { return r.answer.find(text) != std::string::npos; };

    // T70: how long the CPU was over a level, from the per-second readings, not the events' span.
    for (const char* kind : {"slow.above.value.window", "slow.value.window"}) {
        as::AskResult a = ask_as(kind, "how long was the cpu above 80% today");
        CHECK(has(a, "four minutes") && has(a, "three spells") && has(a, "09:08") && !has(a, "event"));
    }
    CHECK(has(ask_as("slow.above.value.window", "for how much time was the processor over 90% this morning"), "four minutes"));
    // Before the per-second readings: the hours the hourly records show at the level.
    as::AskResult yest = ask_as("slow.above.value.window", "how long was the cpu above 80% yesterday");
    CHECK(has(yest, "10:00") && has(yest, "hourly"));
    // The 13:00 hour overlaps the readings (from 13:32): it is not "before" them.
    CHECK(has(yest, "of the six hours on record"));

    // T31 (passed before; kept): the traffic totals of a day.
    CHECK(has(ask_as("network.value.window", "bandwidth usage today"), "uploaded"));
    // T80: data sent is the upload first, and where it went is not measured; a which-kind gives the totals it can.
    for (const char* q : {"data sent to cloud", "how much did this laptop upload in the last day"}) {
        as::AskResult a = ask_as("sharing.value.window", q);
        CHECK(has(a, "about 1.1 GB was sent") && a.answer.find("1.1 GB") < a.answer.find("173 MB") && has(a, "not how much went to each"));
    }
    as::AskResult sent = ask_as("sharing.which.now", "data sent to cloud");
    CHECK(has(sent, "firefox") && has(sent, "1.1 GB"));

    // T40: the runners a program starts in its own service are its: ollama with llama-server was the main cause.
    for (const char* q : {"did ollama eat all my memory yesterday", "did ollama hog the memory yesterday"}) {
        as::AskResult a = ask_as("memory.which.window", q);
        CHECK(has(a, "ollama, with the llama-server") && has(a, "main cause") && !has(a, "main cause was llama-server"));
    }

    // T11: a "while it ran" over runs that overlap: joined, each reading counted once, the spans said plainly.
    as::AskResult ran = ask_as("slow.value.window", "cpu usage while ollama ran");
    CHECK(has(ran, "since Mon 22:47") && !has(ran, " to Oct") && !has(ran, "more"));
    CHECK(!ran.receipts.empty() && ran.receipts[0].rfind(std::to_string(last - first) + " per-second readings", 0) == 0);
    CHECK(has(ask_as("slow.value.window", "how busy was the cpu while ollama was running"), "since Mon 22:47"));

    // T68: a word naming a reading the records keep ("pressure") is that reading; before the per-second readings,
    // the hourly records.
    for (const char* q : {"memory pressure yesterday", "how bad was the memory pressure yesterday"}) {
        as::AskResult a = ask_as("memory.value.window", q);
        CHECK(has(a, "pressure") && has(a, "6.8%") && has(a, "23:35") && has(a, "14.9%") && has(a, "10:00"));
        // The time at or over the level is the readings' own (37 of them), not the span of blips joined a minute apart.
        CHECK(has(a, "under a minute in all"));
    }

    // T10: the GPU's temperature asked of the past: not kept, said first; the load it does keep follows.
    for (const char* q : {"gpu temp while gaming sunday", "how hot was the graphics card on sunday"}) {
        as::AskResult a = ask_as("gpu.value.now.temp", q);
        CHECK(a.answer.rfind("I don't keep the GPU's temperature", 0) == 0 && has(a, "GPU load"));
    }

    // T85: the disks at a moment with no disk event: the readings, my programs' share, the system's unreadable.
    for (const char* q : {"what was reading and writing the ssd at 2am", "what was using the disk at 2am"}) {
        as::AskResult a = ask_as("disk.which.window", q);
        CHECK(has(a, "02:41") && has(a, "python read and wrote") && has(a, "snapperd") && !has(a, "quiet in that window") && !has(a, "sleep"));
    }

    // T92: the fan over a night: how loud and when, and what was working then.
    for (const char* q : {"fan noise last night", "how loud was the fan last night"}) {
        as::AskResult a = ask_as("heat.fan.value.window", q);
        CHECK(has(a, "23:32") && has(a, "00:02") && has(a, "45%") && has(a, "cc1plus"));
    }

    // T56: a yes or no about an unnamed program's work on the CPU that no peak decides ("mine crypto"): whether anything
    // held the machine steadily for long, and whether the heaviest programs are ones known or seen before.
    for (const char* q : {"did something mine crypto on my laptop today", "did anything use my laptop to mine bitcoin today"}) {
        as::AskResult a = ask_as("slow.which.window", q);
        CHECK(a.answer.rfind("No sign of it", 0) == 0 && has(a, "laude") && !has(a, "can't say yes or no"));
    }
    as::AskResult mined = ask_as("slow.which.window", "did something mine crypto on my laptop yesterday");
    CHECK(has(mined, "worker-x") && has(mined, "one hour") && has(mined, "isn't a program I know") && !has(mined, "No sign"));

    // T13: which program works the disk right now, by its own live counters.
    for (const char* q : {"what's hammering the disk right now?", "which program is using the disk right now"})
        CHECK(has(ask_as("", q), "ackup-tool"));

    // T54: which program uses the network now: not measured, said; who is connected and the machine's rate.
    for (const char* q : {"what's using my bandwidth atm", "what is eating my internet right now"}) {
        as::AskResult a = ask_as("", q);
        CHECK(has(a, "firefox") && has(a, "isn't measured") && has(a, "B/s"));
    }

    // T60: the temperature now with whether that is hot: against the recorder's level and the usual.
    for (const char* q : {"What's my CPU temp right now, is it running hot?", "how hot is my cpu at the moment"}) {
        as::AskResult a = ask_as("", q);
        CHECK(has(a, "57 C") && has(a, "not hot") && has(a, "85 C") && has(a, "46 C"));
    }

    // T67: two questions in one, each answered: the battery's wear and how fast it drains (on the charger now: the
    // last time it ran on its own).
    for (const char* q : {"what's the battery health and how fast is it draining", "what is my battery health, and how quickly is it draining"}) {
        as::AskResult a = ask_as("", q);
        CHECK(has(a, "of its design capacity") && has(a, "47%") && a.branch.find(" + ") != std::string::npos);
    }
    moving = false;
    writer.join();
    fs::remove_all(dir);
}

// A command's output.
std::string run_cmd(const std::string& cmd) {
    std::string out;
    if (FILE* f = popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (std::size_t n = std::fread(buf, 1, sizeof buf, f)) out.append(buf, n);
        pclose(f);
    }
    return out;
}

// The log and identity answers of the traced 95 (records area), against rows shaped like the records copied on
// 2026-10-23 at 13:58: config snapshots, the software and admin log, Wi-Fi drops and sleeps, log-ins, the lid and
// the charger, programs opened by hand, the free space over the week, the device log.
void test_records_t95() {
    std::puts("records answers of the traced 95");
    namespace fs = std::filesystem;
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const char* em = std::getenv("ASK_EMBED_MODEL");
    const std::string model = em ? em : home + "/.local/share/unllm/models/bge-small-en-v1.5";
    if (access((model + "/model.onnx").c_str(), R_OK) != 0) {
        std::puts("  no embedding model: skipped");
        return;
    }
    const std::string dir = fs::temp_directory_path() / ("as-t95-" + std::to_string(getpid()));
    fs::create_directories(dir);
    auto at = [](int mon, int mday, int h, int m, int s) {
        std::tm tmv{};
        tmv.tm_year = 2026 - 1900; tmv.tm_mon = mon - 1; tmv.tm_mday = mday; tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = s; tmv.tm_isdst = -1;
        return int64_t(std::mktime(&tmv));
    };
    const int64_t now = at(10, 23, 13, 59, 8);
    const std::string dbp = dir + "/h.db";
    { as::Store st(dbp); }  // the schema
    {
        sqlite3* db = nullptr;
        sqlite3_open(dbp.c_str(), &db);
        auto exec = [&](const std::string& sql) {
            char* err = nullptr;
            const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
            if (err) std::fprintf(stderr, "  %s\n", err), sqlite3_free(err);
            return ok;
        };
        auto i = [](int64_t v) { return std::to_string(v); };
        CHECK(exec("BEGIN"));
        auto row = [&](int64_t t, const char* cat, const std::string& title, const std::string& detail) {
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(db, "INSERT INTO log(t, category, title, detail) VALUES (?, ?, ?, ?)", -1, &q, nullptr);
            sqlite3_bind_int64(q, 1, t);
            sqlite3_bind_text(q, 2, cat, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 3, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 4, detail.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        };
        auto proc = [&](int64_t pid, int64_t started, int64_t ended, const std::string& name, const std::string& cmdline, const std::string& unit) {
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(db, "INSERT INTO procs(pid, started, ended, name, exe, cmdline, user, unit, ppid, parent, cpu_seconds, peak_rss, end_estimated) "
                                   "VALUES (?, ?, ?, ?, '', ?, 'user', ?, 1, 'systemd', 1, 1e8, 0)", -1, &q, nullptr);
            sqlite3_bind_int64(q, 1, pid);
            sqlite3_bind_int64(q, 2, started);
            sqlite3_bind_int64(q, 3, ended);
            sqlite3_bind_text(q, 4, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 5, cmdline.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 6, unit.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        };
        // Packages: the August upgrade that moved the kernel and the firmware, the networking update before
        // Saturday's drop, and Wednesday's install, which the user started with sudo ten seconds before.
        row(at(8, 30, 18, 43, 30), "software", "Installed 21 packages; upgraded 824 packages",
            "Command: pacman -Syu\nUpgraded:\n  intel-ucode (20260512-1 -> 20260812-1)\n  linux-cachyos (7.1.3-2 -> 7.2.2-1)\n"
            "  linux-cachyos-lts (6.18.38-2 -> 6.18.42-1)\n  linux-firmware (1:20260622-1 -> 1:20260810-2)\n"
            "  linux-firmware-intel (1:20260622-1 -> 1:20260810-2)\n");
        row(at(10, 17, 14, 30, 0), "software", "Upgraded networkmanager", "Upgraded:\n  networkmanager (1.54.0-1 -> 1.54.1-1)\n");
        row(at(10, 21, 20, 40, 40), "admin", "Ran as administrator (sudo) by user: /usr/bin/pacman -Syy onnxruntime-cuda sentencepiece",
            "COMMAND=/usr/bin/pacman -Syy onnxruntime-cuda sentencepiece");
        row(at(10, 21, 20, 40, 50), "software", "Installed 6 packages; removed onnxruntime-cpu",
            "Installed:\n  cudnn (9.27.0.42-1.1)\n  cudnn-frontend (1.31.0-1.1)\n  nccl (2.32.3-1.1)\n  onednn (3.11.3-1)\n"
            "  onnxruntime-cuda (1.29.0-3)\n  sentencepiece (0.2.2-2)\nRemoved:\n  onnxruntime-cpu (1.29.0-3)\n");
        // The Wi-Fi: Saturday's drop while awake; this week every disconnect came with going to sleep.
        row(at(10, 17, 15, 0, 0), "network", "Disconnected: wlan0", "");
        row(at(10, 17, 15, 2, 0), "network", "Connected to Home network on wlan0", "");
        auto nap = [&](int64_t a, int64_t b, bool lid) {
            row(a - 2, "network", "Disconnected: wlan0", "");
            if (lid) row(a - 2, "session", "Lid closed", "");
            row(a, "session", "Went to sleep", "");
            row(b, "session", "Woke from sleep", "");
            if (lid) row(b, "session", "Lid opened", "");
            row(b + 9, "network", "Connected to Home network on wlan0", "");
        };
        nap(at(10, 21, 0, 10, 13), at(10, 21, 17, 19, 0), true);
        nap(at(10, 21, 21, 36, 17), at(10, 22, 6, 45, 28), true);
        nap(at(10, 22, 11, 7, 41), at(10, 22, 12, 26, 15), true);
        nap(at(10, 22, 14, 54, 45), at(10, 22, 14, 54, 58), true);
        // Log-ins: the desktop's own, the greeter's and the user's at Thursday 13:07; the charger, twice in the night.
        row(at(10, 22, 13, 7, 21), "session", "Logged in: 'plasmalogin' with class 'greeter' and type 'wayland'.", "");
        row(at(10, 22, 13, 7, 27), "session", "Logged in: 'user' with class 'user' and type 'wayland'.", "");
        row(at(10, 22, 23, 29, 59), "session", "Unplugged from power at 100% battery", "");
        row(at(10, 22, 23, 46, 21), "session", "Plugged into power at 88% battery", "");
        row(at(10, 23, 9, 11, 35), "session", "Unplugged from power at 100% battery", "");
        row(at(10, 23, 10, 7, 38), "session", "Plugged into power at 56% battery", "");
        // Unit failures this week.
        row(at(10, 22, 7, 23, 31), "services", "ollama.service failed: Failed with result 'oom-kill'.", "");
        row(at(10, 23, 8, 13, 4), "services", "cpu-burn-test.service failed: Failed with result 'exit-code'.", "");
        // Programs opened by hand: desktop apps (app-*.service) and terminal tabs (vte-spawn-*.scope); a backup on
        // Tuesday evening; a background service all night.
        proc(5001, at(10, 22, 14, 18, 39), at(10, 22, 15, 0, 0), "okular", "/usr/bin/okular paper.pdf", "app-org.kde.okular@a1.service");
        proc(5002, at(10, 22, 14, 26, 20), at(10, 22, 18, 0, 0), "fish", "/usr/bin/fish", "vte-spawn-0001.scope");
        proc(5003, at(10, 22, 16, 35, 18), at(10, 22, 17, 0, 0), "firefox", "/usr/lib/firefox/firefox", "app-firefox@b2.service");
        proc(5004, at(10, 22, 21, 37, 17), at(10, 22, 22, 30, 0), "haruna", "/usr/bin/haruna", "app-org.kde.haruna@c3.service");
        proc(5005, at(10, 23, 9, 7, 5), at(10, 23, 9, 30, 0), "fish", "/usr/bin/fish", "vte-spawn-0002.scope");
        proc(5006, at(10, 23, 13, 22, 6), at(10, 23, 13, 40, 0), "dolphin", "/usr/bin/dolphin", "app-org.kde.dolphin@d4.service");
        proc(5007, at(10, 22, 13, 0, 0), at(10, 23, 13, 50, 0), "ollama", "/usr/local/bin/ollama serve", "ollama.service");
        proc(5008, at(10, 20, 18, 0, 0), at(10, 20, 18, 20, 0), "rsync", "rsync -a /home/user /mnt/backup", "app.scope");
        // Free space on / over the week, hourly: Monday afternoon freed some, Wednesday evening filled the most.
        std::string hourly = "INSERT INTO samples_hourly(hour_ts, metric, n, med, p05, p95, max, min, on_battery_frac) VALUES ";
        const std::vector<std::tuple<int, int, double>> free = {
            {19, 7, 25.70}, {19, 10, 25.62}, {19, 13, 25.53}, {19, 14, 29.96}, {19, 23, 29.32}, {20, 6, 29.27}, {20, 8, 27.75},
            {20, 18, 27.70}, {20, 19, 27.69}, {21, 18, 27.50}, {21, 19, 26.59}, {21, 20, 26.25}, {21, 21, 25.39}, {22, 6, 25.30},
            {22, 12, 25.25}, {23, 12, 24.29}};
        for (std::size_t k = 0; k < free.size(); ++k) {
            const auto& [d, h, v] = free[k];
            hourly += std::string(k ? "," : "") + "(" + i(at(10, d, h, 0, 0)) + ", 'disk_free_pct', 3600, " + std::to_string(v) + ", " +
                      std::to_string(v) + ", " + std::to_string(v) + ", " + std::to_string(v) + ", " + std::to_string(v) + ", 0)";
        }
        CHECK(exec(hourly));
        CHECK(exec("COMMIT"));
        sqlite3_close(db);
    }
    char exe[4096];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const std::string bin = len > 0 ? std::string(exe, std::size_t(len)) : std::string("./ascore_tests");
    const std::string cache = bin.substr(0, bin.rfind('/')) + "/kinds-cache";
    fs::create_directories(cache);
    // The live machine's own /proc and /sys: the identity answers read the hardware.
    auto ask_as = [&](const std::string& kind, const char* q) {
        as::Ask a;
        a.open(dbp);
        a.load_language("vendor/unllm/data/synonyms.txt", "vendor/unllm/data/vectors.bin",
                        "vendor/unllm/data/banter.txt", "vendor/unllm/data/rewrites.txt");
        a.load_conversation("vendor/unllm/data/followups.txt", "vendor/unllm/data/chatspeak.txt");
        a.load_facts({"facts/base.tsv"});
        a.load_kinds("data/kinds", cache, model);
        a.force_reading(kind);
        return a.ask(q, now);
    };
    auto has = [](const as::AskResult& r, const char* text) {
        const bool ok = r.answer.find(text) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  no \"%s\" in [%s] %s\n", text, r.branch.c_str(), r.answer.c_str());
        return ok;
    };
    auto lacks = [](const as::AskResult& r, const char* text) {
        const bool ok = r.answer.find(text) == std::string::npos;
        if (!ok) std::fprintf(stderr, "  \"%s\" in [%s] %s\n", text, r.branch.c_str(), r.answer.c_str());
        return ok;
    };

    // T28, T83: "any" and "file" are how the question is put, not files' names: every change since Monday, and a
    // yes to a yes-or-no question. A word that names a file still narrows it.
    {
        const std::string fake = dir + "/home", repo = fake + "/.local/share/hindsight/repo";
        fs::create_directories(repo + "/etc");
        fs::create_directories(repo + "/home/config");
        std::system(("git init -q '" + repo + "'").c_str());
        auto snap = [&](const char* date) {
            std::system(("cd '" + repo + "' && export GIT_AUTHOR_DATE='" + date + "' GIT_COMMITTER_DATE='" + date +
                         "' && git -c user.name=t -c user.email=t@t add -A && git -c user.name=t -c user.email=t@t commit -qm snapshot >/dev/null 2>&1")
                            .c_str());
        };
        std::ofstream(repo + "/etc/hostname") << "laptop\n";
        std::ofstream(repo + "/etc/fstab") << "# fstab\n";
        std::ofstream(repo + "/home/config/kwinrc") << "[a]\n";
        std::ofstream(repo + "/home/config/baloo_file.desktop") << "[Desktop Entry]\n";
        snap("2026-10-18T00:10:02");
        std::ofstream(repo + "/home/config/kwinrc") << "[b]\n";
        snap("2026-10-20T00:04:25");
        std::ofstream(repo + "/etc/hostname") << "laptop2\n";
        std::ofstream(repo + "/home/config/gtkrc") << "gtk\n";
        snap("2026-10-22T06:45:26");
        std::ofstream(repo + "/home/config/kwinrc") << "[c]\n";
        snap("2026-10-23T15:00:00");  // after the clock
        setenv("HOME", fake.c_str(), 1);
        as::AskResult any = ask_as("config.list.window", "did any config files change since monday");
        CHECK(has(any, "Yes") && has(any, "3 config files changed") && has(any, "~/.config/kwinrc") && lacks(any, "No any"));
        as::AskResult which = ask_as("config.list.window", "what config files changed since monday");
        CHECK(has(which, "~/.config/kwinrc") && has(which, "/etc/hostname") && has(which, "~/.config/gtkrc") && lacks(which, "file file"));
        CHECK(lacks(ask_as("config.list.window", "were all my config files edited this week or what"), "No all"));
        CHECK(has(ask_as("config.list.window", "did fstab change since monday"), "No fstab file changed"));
        CHECK(has(ask_as("config.list.window", "did sysctl change since monday"), "No sysctl file changed"));
        setenv("HOME", home.c_str(), 1);
    }

    // T72: the week's one install was started by the user's own sudo: said, and the packages named, not a bare "Yes".
    as::AskResult inst = ask_as("updates.list.window", "did anything weird install itself this week");
    CHECK(lacks(inst, "Yes") && has(inst, "sudo") && has(inst, "pacman -Syy onnxruntime-cuda sentencepiece") && has(inst, "cudnn"));
    as::AskResult inst2 = ask_as("updates.list.window", "did something get installed on its own since monday");
    CHECK(lacks(inst2, "Yes") && has(inst2, "sudo") && has(inst2, "nccl"));

    // T23: the Wi-Fi never dropped this week while the laptop was awake: said, with the week's change; Saturday's drop
    // came after the networking update.
    as::AskResult wifi = ask_as("updates.cause.window", "what changed on my system this week that broke wifi");
    CHECK(has(wifi, "Wi-Fi") && has(wifi, "sleep") && has(wifi, "cudnn") && lacks(wifi, "networkmanager"));
    as::AskResult sat = ask_as("updates.cause.window", "did an update break my wifi on saturday");
    CHECK(has(sat, "Sat 15:00") && has(sat, "networkmanager") && has(sat, "1.54.1-1"));

    // T65: the kernel's own package history from the software log, beside the version running now; a plain "what
    // kernel" gets no history.
    {
        std::ifstream rel("/proc/sys/kernel/osrelease");
        std::string running;
        std::getline(rel, running);
        as::AskResult ker = ask_as("identity.kernel", "kernel version change history");
        const bool cachy = run_cmd("pacman -Qqo /usr/lib/modules/" + running + "/vmlinuz 2>/dev/null") == "linux-cachyos\n";
        CHECK(has(ker, running.c_str()));
        if (cachy) CHECK(has(ker, "7.1.3-2") && has(ker, "7.2.2-1") && has(ker, "August 30"));
        as::AskResult upd = ask_as("identity.kernel", "when was the kernel last updated");
        if (cachy) CHECK(has(upd, "7.2.2-1"));
        CHECK(lacks(ask_as("identity.kernel", "what kernel am i running"), "7.1.3-2"));
    }

    // T75: the whole hardware list, not a summary; T43: the memory's sticks and speed, or why they can't be read.
    as::AskResult hw = ask_as("identity.list", "exact hardware list");
    CHECK(has(hw, "CPU") && has(hw, "memory") && has(hw, "kernel") && (has(hw, "disk") || has(hw, "drive")));
    CHECK(lacks(ask_as("identity.list", "full specs of this machine"), "has been up for"));
    as::AskResult ram = ask_as("identity.ram", "how much ram is in this laptop and what speed");
    CHECK(ram.answer.find("MT/s") != std::string::npos || has(ram, "root"));
    CHECK(has(ask_as("identity.ram", "what memory sticks does it have"), "GB"));

    // T71: which folder grew is not kept: said, with the week's change in free space and where it fell; the folders
    // are those of now.
    {
        const std::string fake = dir + "/small";
        fs::create_directories(fake + "/Downloads");
        std::ofstream(fake + "/Downloads/a.bin") << std::string(4096, 'x');
        setenv("HOME", fake.c_str(), 1);
        as::AskResult fill = ask_as("disk.space.which.now", "what filled up my drive this week");
        CHECK(has(fill, "folder sizes") && has(fill, "from 26%") && has(fill, "Wed 18:00") && has(fill, "Downloads"));
        as::AskResult ate = ask_as("disk.space.which.now", "what ate my disk space since monday");
        CHECK(has(ate, "folder sizes") && has(ate, "Mon 13:00"));
        CHECK(lacks(ask_as("disk.space.which.now", "what is taking up space on my drive"), "folder sizes"));
        setenv("HOME", home.c_str(), 1);
    }

    // T78: no backup on Monday in the records: said, with Monday's free space; Tuesday's backup is placed.
    as::AskResult bk = ask_as("disk.space.trend.window", "disk space before monday backup");
    CHECK(has(bk, "backup") && has(bk, "no program") && has(bk, "26%"));
    as::AskResult tue = ask_as("disk.space.trend.window", "how much free space was there before the backup on tuesday");
    CHECK(has(tue, "rsync") && has(tue, "Tue 18:00") && lacks(tue, "no program"));
    // The kind's own wording ("lose", as its phrasings ask it) and a program the question is about are no moment.
    as::AskResult lose = ask_as("disk.space.trend.window", "how much disk space did i lose this week");
    CHECK(lacks(lose, "in the records") && has(lose, "Free space on /"));
    CHECK(lacks(ask_as("disk.space.trend.window", "how much space did ollama eat this week"), "in the records"));

    // T4: who used it can't be said; the log-ins, the lid, the charger and the programs opened by hand can.
    as::AskResult used = ask_as("story.used.window", "did my roommate use my laptop while i was out");
    CHECK(has(used, "can't say who") && has(used, "13:07") && has(used, "Thu 23:29") && has(used, "Thu 21:37") && has(used, "Fri 09:07"));
    as::AskResult used2 = ask_as("story.used.window", "was anyone on my computer last night");
    CHECK(has(used2, "can't say who") && has(used2, "haruna") && lacks(used2, "okular"));

    // T79: no device record then: said, and the sleep at 14:54 a minute before.
    as::AskResult mouse = ask_as("devices.when.window", "my mouse disconnected at 3pm, why");
    CHECK(has(mouse, "No device") && has(mouse, "14:54") && has(mouse, "sleep"));
    as::AskResult kb = ask_as("devices.when.window", "why did my keyboard stop working around noon yesterday");
    CHECK(has(kb, "No device") && has(kb, "12:26"));

    // T8: no firmware flash on record, and the BIOS by its version and date.
    as::AskResult fw = ask_as("flash.list.window", "firmware update history check");
    CHECK(has(fw, "No firmware flash") && has(fw, "BIOS"));
    CHECK(lacks(ask_as("flash.list.window", "list my esp32 flashes this week"), "BIOS"));

    // T95: "systemd crashes" are failed units.
    as::AskResult sd = ask_as("startup.which.window", "systemd crashes this week");
    CHECK(has(sd, "unit failures") && has(sd, "ollama.service") && has(sd, "cpu-burn-test.service"));

    // T53: a "while ..." that names a state the session log keeps (the lid closed, on battery) is checked against its
    // rows: the lid was never closed last night (its last close Thu 14:54), said before the drain; a state that held
    // is told with its times; a clause that names no state changes nothing.
    as::AskResult lid = ask_as("battery.value.window", "whats draining batt while lid closed last night");
    CHECK(has(lid, "lid wasn't closed") && has(lid, "Thu 14:54"));
    as::AskResult unpl = ask_as("battery.value.window", "how fast did the battery drain while it was unplugged last night");
    CHECK(has(unpl, "on battery from Thu 23:29 to Thu 23:46"));
    as::AskResult shut = ask_as("battery.value.window", "did the battery drain while the lid was shut on wednesday");
    CHECK(has(shut, "lid was closed from Wed 00:10"));
    as::AskResult game = ask_as("heat.value.window", "how hot did the cpu get while i was compiling yesterday");
    CHECK(lacks(game, "wasn't") && lacks(game, "lid was"));
    fs::remove_all(dir);
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
    test_child_cpu_once();
    test_child_cpu_events();
    test_blips_dropped();
    test_store();
    test_rollup();
    test_swap_column();
    test_swap_battery_history();
    test_startup_lifetime();
    test_facts();
    test_judge();
    test_diagnose();
    test_answer_parts();
    test_askjudge();
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
    test_askcore();
    test_kinds();
    test_names();
    test_records_data();
    test_records_log();
    test_shutdown_event();
    test_records_n50();
    test_live_t95();
    test_records_t95();
    test_event_answers();
    test_answer_shapes();
    test_catalogue_kinds();
    test_window_answers();
    test_notable();
    test_updates();
    test_upgradecheck();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
