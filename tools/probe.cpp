// Development helper: samples the live machine; reports cost and triggers.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

#include "events.h"
#include "sampler.h"

int main(int argc, char** argv) {
    int n = argc > 1 ? std::atoi(argv[1]) : 3;
    bool verbose = argc > 2;
    as::Sampler sampler;
    as::EventEngine engine;
    std::map<std::string, double> total;
    for (int i = 0; i < n; ++i) {
        auto s = sampler.sample();
        for (auto& [w, ms] : s->timings) total[w] += ms;
        if (s->procs_reused) total["(reused scans)"] += 1;
        double t0 = as::thread_cpu_seconds();
        for (auto& e : engine.feed(s)) {
            std::printf("EVENT CLOSED: %s\n", e.summary.c_str());
            for (auto& c : e.culprits)
                std::printf("   %-7s %-22s pid %-7d parent %-12s cpu %6.2f s share %4.1f%% child %6.2f exited %d count %d\n", c.kind.c_str(),
                            c.name.c_str(), c.pid, c.parent.c_str(), c.cpu_seconds, c.share * 100, c.child_cpu_seconds, c.exited, c.count);
        }
        total["Events"] += (as::thread_cpu_seconds() - t0) * 1000;
        if (verbose && (i % 5 == 0 || !engine.active_now().empty())) {
            std::printf("t=%d cpu %.0f%% open=%d active:", i, s->cpu.total, engine.has_open());
            for (auto& a : engine.active_now()) std::printf(" [%s]", a.c_str());
            std::printf("\n");
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    double sum = 0;
    for (auto& [w, ms] : total) {
        sum += ms;
        std::printf("  %-16s %7.1f ms per minute (per part)\n", w.c_str(), ms * 60.0 / n);
    }
    std::printf("TOTAL %.1f ms per minute\n", sum * 60.0 / n);
}
