#include <chrono>
#include <cstdio>
#include "kinds.h"
int main() {
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    as::Kinds k;
    if (!k.load("data/kinds", "/tmp", "/nonexistent")) { std::puts("load failed"); return 1; }
    auto t1 = clk::now();
    const char* qs[] = {"why did the fan scream last night", "what crashed this week", "is firefox eating my ram rn",
                        "how hot did the cpu get while i was asleep", "which apps connected out for the first time today"};
    int n = 0; auto t2 = clk::now();
    for (int r = 0; r < 200; ++r) for (const char* q : qs) { auto m = k.match(q); n += !m.id.empty(); }
    auto t3 = clk::now();
    std::printf("load %.0f ms; match %.1f us each (%d)\n", std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::micro>(t3 - t2).count() / n, n);
    for (const char* q : qs) { auto m = k.match(q); std::printf("  %.2f %-28s %s\n", m.score, m.id.c_str(), q); }
}
