// ask: plain questions against this machine's recorded history.
// Deterministic end to end; the heavy thinking happened at record time.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "askcore.h"
#include "kinds.h"
#include <sys/stat.h>

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
    std::string question, script;
    int64_t fixed_now = 0;
    std::string force_kind, offer_kind;
    bool show_branch = false, show_trace = false, kind_mode = false, rebuild_kinds = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--script" && i + 1 < argc) { script = argv[++i]; continue; }
        if (a == "--now" && i + 1 < argc) { fixed_now = std::atoll(argv[++i]); continue; }
        if (a == "--branch") { show_branch = true; continue; }
        if (a == "--trace") { show_branch = show_trace = true; continue; }
        if (a == "--as" && i + 1 < argc) { force_kind = argv[++i]; continue; }
        if (a == "--offer" && i + 1 < argc) { offer_kind = argv[++i]; continue; }
        if (a == "--kind") { kind_mode = true; continue; }          // measuring: the kind a question maps to
        if (a == "--build-kinds") { kind_mode = rebuild_kinds = true; continue; }
        question += std::string(question.empty() ? "" : " ") + a;
    }
    if (kind_mode) {
        // The catalogue matcher alone (src/core/kinds.h): shipped kinds and
        // phrasings, vectors cached under the user's data folder, the
        // embedding model from unllm's install (ASK_EMBED_MODEL overrides).
        std::string dir = exe_dir();
        const char* xdg = getenv("XDG_DATA_HOME");
        const std::string data = (xdg ? std::string(xdg) : home_path("/.local/share")) + "/activityscope";
        const char* em = getenv("ASK_EMBED_MODEL");
        const std::string kinds_dir = first_existing({dir + "/../share/activityscope/kinds/kinds.tsv", dir + "/../data/kinds/kinds.tsv"});
        mkdir((data + "/kinds").c_str(), 0755);
        as::Kinds kinds;
        if (!kinds.load(kinds_dir.substr(0, kinds_dir.rfind('/')), data + "/kinds",
                        em ? em : home_path("/.local/share/unllm/models/bge-small-en-v1.5"), rebuild_kinds)) {
            std::printf("ask: the kinds catalogue or the embedding model could not be loaded\n");
            return 1;
        }
        auto show = [&](const std::string& q) {
            as::KindMatch m = kinds.match(q);
            std::printf("{\"kind\": \"%s\", \"score\": %.4f, \"via\": \"%s\", \"runners\": [", m.id.c_str(), double(m.score), m.phrasing.c_str());
            for (std::size_t i = 0; i < m.runners.size(); ++i)
                std::printf("%s[\"%s\", %.4f]", i ? ", " : "", m.runners[i].first.c_str(), double(m.runners[i].second));
            // each kind's nearest phrasing, in the runners' order
            std::printf("], \"nearest\": [");
            for (std::size_t i = 0; i < m.runner_phrasings.size(); ++i) {
                std::string p;
                for (char c : m.runner_phrasings[i]) p += c == '"' || c == '\\' ? std::string("\\") + c : std::string(1, c);
                std::printf("%s\"%s\"", i ? ", " : "", p.c_str());
            }
            std::printf("]}\n");
        };
        if (!question.empty()) show(question);
        else for (std::string line; std::getline(std::cin, line);) if (!line.empty()) show(line);
        return 0;
    }
    if (question == "--help" || question == "-h") {
        std::puts("ask: a plain question about this machine, answered from its records.\n"
                  "  ask why did the fan scream last night\n"
                  "  ask what crashed yesterday\n"
                  "  ask                      (no question: a conversation; follow-ups work)\n"
                  "  ask --script FILE        replay a conversation, one question per line\n"
                  "  ask --now EPOCH ...      answer as if the clock read EPOCH (tests)\n"
                  "  ask --branch ...         also print which handler answered\n"
                  "  ask --trace ...          --branch plus the time read and the nearest kinds\n"
                  "  ask --as KIND ...        answer as that catalogue kind (measuring)\n"
                  "  ask --offer KIND ...     show the check that kind first (measuring)\n"
                  "The history comes from ActivityScope; answers carry the records they\n"
                  "stand on: table lookups and arithmetic. A small local model (Ollama,\n"
                  "qwen3:4b) checks each answer fits the question and never writes one;\n"
                  "ASK_CHECK=off turns the check off. Nothing leaves this machine.");
        return 0;
    }
    if (question == "-i") question.clear();
    auto now = [&] { return fixed_now ? fixed_now : int64_t(time(nullptr)); };
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
        first_existing({dir + "/../share/activityscope/unllm/vectors.bin", dir + "/../vendor/unllm/data/vectors.bin"}),
        first_existing({dir + "/../share/activityscope/unllm/banter.txt", dir + "/../vendor/unllm/data/banter.txt"}),
        first_existing({dir + "/../share/activityscope/unllm/rewrites.txt", dir + "/../vendor/unllm/data/rewrites.txt"}));
    ask.load_conversation(
        first_existing({dir + "/../share/activityscope/unllm/followups.txt", dir + "/../vendor/unllm/data/followups.txt"}),
        first_existing({dir + "/../share/activityscope/unllm/chatspeak.txt", dir + "/../vendor/unllm/data/chatspeak.txt"}));
    // Refused questions land in a local file, so the vocabulary can grow
    // from real misses. Never sent anywhere; delete it any time.
    const char* xdg = getenv("XDG_DATA_HOME");
    const std::string data = (xdg ? std::string(xdg) : home_path("/.local/share")) + "/activityscope";
    ask.set_miss_log(data + "/ask-misses.log");
    // What things are. Later files win: the machine's harvest (factharvest)
    // covers the long tail, the shipped hand-written notes outrank it, and
    // the user's corrections outrank everything.
    ask.load_facts({data + "/facts/local.tsv",
                    first_existing({dir + "/../share/activityscope/facts/base.tsv", dir + "/../facts/base.tsv"}),
                    data + "/facts/custom.tsv"});
    // The question kinds and their matcher (kinds.h): shipped data, vectors cached
    // under the data folder, the embedding model from unllm's install.
    {
        const char* em = getenv("ASK_EMBED_MODEL");
        const std::string kf = first_existing({dir + "/../share/activityscope/kinds/kinds.tsv", dir + "/../data/kinds/kinds.tsv"});
        mkdir((data + "/kinds").c_str(), 0755);
        ask.load_kinds(kf.substr(0, kf.rfind('/')), data + "/kinds", em ? em : home_path("/.local/share/unllm/models/bge-small-en-v1.5"));
    }
    // The final check (askcore.h): the local 4B reads each answer; ASK_CHECK names
    // another model or "off". Without Ollama the answers stand as they are.
    const char* check = getenv("ASK_CHECK");
    ask.set_checker(check ? check : "qwen3:4b");
    ask.force_reading(force_kind);
    ask.offer_reading(offer_kind);
    auto answer = [&](const std::string& q) {
        as::AskResult r = ask.ask(q, now());
        std::printf("%s\n", r.answer.c_str());
        if (show_branch) std::printf("[branch: %s]\n", r.branch.empty() ? "-" : r.branch.c_str());
        if (show_branch) std::printf("[topic: %s %s]\n", r.topic.empty() ? "-" : r.topic.c_str(), r.when.c_str());
        if (show_branch && !r.check.empty()) std::printf("[check: %s]\n", r.check.c_str());
        if (show_trace) {
            std::printf("[window: %s]\n", r.span.c_str());
            std::string all;
            for (const auto& k : r.readings) all += (all.empty() ? "" : " ") + k;
            std::printf("[readings: %s]\n", all.c_str());
            std::string progs;
            for (const auto& p : r.programs) progs += (progs.empty() ? "" : ", ") + p;
            std::printf("[programs: %s]\n", progs.empty() ? "-" : progs.c_str());
        }
        if (!r.receipts.empty()) {
            std::printf("\n");
            for (const auto& line : r.receipts) std::printf("  %s\n", line.c_str());
        }
    };
    if (!script.empty()) {
        // A replayed conversation: one session, each turn echoed and each
        // answer closed by a "---" line so a driver can split them.
        std::ifstream f(script);
        if (!f) {
            std::printf("ask: cannot read %s\n", script.c_str());
            return 1;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::printf("> %s\n", line.c_str());
            answer(line);
            std::printf("---\n");
        }
        return 0;
    }
    if (!question.empty()) {
        answer(question);
        return 0;
    }
    // No question: a conversation. One Ask instance holds the thread, so
    // "and firefox?" or "how many times?" patch the previous question.
    const bool tty = isatty(0);
    if (tty) std::puts("ask: a conversation with the records. Blank line or quit ends it.");
    std::string line;
    while (true) {
        if (tty) {
            std::printf("> ");
            std::fflush(stdout);
        }
        if (!std::getline(std::cin, line)) break;
        if (line.empty() || line == "quit" || line == "exit") break;
        answer(line);
        if (tty) std::printf("\n");
    }
    return 0;
}
