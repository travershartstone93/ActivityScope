// ask: plain questions against this machine's recorded history.
// Deterministic end to end; the heavy thinking happened at record time.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <unistd.h>

#include "askcore.h"

namespace {

std::string home_path(const char* tail) {
    const char* h = getenv("HOME");
    return std::string(h ? h : "") + tail;
}

std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
    return p.substr(0, p.rfind('/'));
}

std::string first_existing(const std::vector<std::string>& paths) {
    for (const auto& p : paths)
        if (access(p.c_str(), R_OK) == 0) return p;
    return paths.empty() ? "" : paths.front();
}

}  // namespace

int main(int argc, char** argv) {
    std::string question;
    for (int i = 1; i < argc; ++i) question += std::string(i > 1 ? " " : "") + argv[i];
    if (question.empty() || question == "--help" || question == "-h") {
        std::puts("ask: a plain question about this machine, answered from its records.\n"
                  "  ask why did the fan scream last night\n"
                  "  ask what crashed yesterday\n"
                  "  ask what did blender do this morning\n"
                  "The history comes from ActivityScope; answers carry the records they\n"
                  "stand on. No model, no network: table lookups and arithmetic.");
        return 0;
    }
    const char* dbenv = getenv("ASK_DB");
    std::string db = dbenv ? dbenv : home_path("/.local/share/activityscope/history.db");
    as::Ask ask;
    if (!ask.open(db)) {
        std::printf("ask: no history at %s (is ActivityScope installed and recording?)\n", db.c_str());
        return 1;
    }
    std::string dir = exe_dir();
    ask.load_language(
        first_existing({dir + "/../share/activityscope/unllm/synonyms.txt", dir + "/../vendor/unllm/data/synonyms.txt"}),
        first_existing({dir + "/../share/activityscope/unllm/vectors.bin", dir + "/../vendor/unllm/data/vectors.bin"}));
    as::AskResult r = ask.ask(question, time(nullptr));
    std::printf("%s\n", r.answer.c_str());
    if (!r.receipts.empty()) {
        std::printf("\n");
        for (const auto& line : r.receipts) std::printf("  %s\n", line.c_str());
    }
    return 0;
}
