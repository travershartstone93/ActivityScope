#pragma once
// Everyday words to the programs in the records: "the movie player" is haruna,
// "that fan control thing" is the root script fan-control.sh, "that node thing"
// is node-MainThread running realm-companion.js. The candidates are the programs
// recorded over a window (procs and minute_top) and those running now. Each is
// described by its own names (the process name, the executable's file name, the
// script an interpreter or shell runs), what its unit says it is, its desktop
// entry and its note in the fact table. A word matches a name exactly first;
// otherwise a run of words matches a description by meaning, through the
// sentence model the kinds matcher already loaded (bge-small). Every description
// comes from this machine or the fact files: no alias tables.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "unllm/unllm.h"

struct sqlite3;
namespace unllm {
class Meaning;
}

namespace as {

class Facts;
struct ProcessInfo;

// One program as the records hold it.
struct Program {
    std::string name;    // the records' name (procs.name, minute_top.name): what the program filters compare
    std::string exe;     // the executable's file name ("node", "python3.14"), from the path or else the command line
    std::string script;  // what an interpreter or shell runs ("realm-companion.js", "fan-control.sh"), "" for none
    std::string unit;    // its unit, random parts folded to * ("vte-spawn-*.scope"), "" for none
    std::string about;   // what that unit says it is ("MSI Cooler Boost Fan Control"), "" when it says nothing more
    int64_t runs = 0;    // processes in the window
    double cpu = 0;      // their CPU seconds in the window
    bool live = false;   // running now
};

// A program some words of a question name.
struct NameMatch {
    Program program;
    float score = 0;     // 1 for a name said exactly, else the cosine of the nearest description or name
    std::string words;   // the words that named it ("movie player")
    std::string via;     // the name or description they matched ("Media Player")
    bool by_unit = false;  // matched through what the program's unit is ("the terminal": a terminal tab), so the
                           // words name the unit rather than this one program in it
};

class Names {
public:
    Names();
    ~Names();
    // The records (read-only, not owned).
    void open(sqlite3* db);
    // The sentence model (not owned; null leaves exact names only), the fact table (not owned) and the analyzer
    // whose own words (topics, time, generic words) never name a program by themselves (not owned).
    void set_model(const unllm::Meaning* model);
    void set_facts(const Facts* facts);
    void set_analyzer(const unllm::Analyzer* an);
    // Description vectors are kept in this file between runs; "" keeps them in memory only.
    void set_cache(const std::string& path);
    // For tests: unit descriptions by unit or folded unit instead of asking systemd, and the folders that hold
    // desktop entries (by default /usr/share/applications and the user's own).
    void set_unit_descriptions(std::map<std::string, std::string> descriptions);
    void set_desktop_dirs(std::vector<std::string> dirs);

    // The candidates: every program recorded over [from, to) (the whole records when to is 0), with its runs and
    // CPU there, plus the live processes given.
    void gather(int64_t from, int64_t to, const std::vector<ProcessInfo>& live = {});
    const std::vector<Program>& candidates() const;
    // The programs some words of a question name, best first; empty when none does. Gathers the reading's window
    // first, then the whole records when nothing there matches. live: the processes running now, for a question
    // about now; it is called only when some words may name a program.
    std::vector<NameMatch> find(const std::string& question, const unllm::Reading& reading, int64_t now,
                                const std::function<std::vector<ProcessInfo>()>& live = {});
    // One phrase ("the movie player") against the gathered candidates, best first; empty when it names none.
    std::vector<NameMatch> resolve(const std::string& phrase, int64_t now);
    // The same with the phrase taken whole, its known words too, against the candidates' descriptions: what a clause
    // is about ("movie" in "while i watched the movie"), which resolve() would skip as a word the analyzer knows.
    std::vector<NameMatch> resolve_whole(const std::string& phrase, int64_t now);
    // A program installed but never seen in the records: an executable on the PATH whose file name is two or three of
    // the question's words, one the analyzer does not know, joined as file names join words ("the arduino cli thing":
    // arduino-cli). Its path, or "".
    std::string installed(const std::string& question, const unllm::Reading& reading, int64_t now);

    // How close a description must be to count as named (set from tests, see notes): a phrase of two or three
    // words, and a single word, which must be nearer since one word inside a description ("version" in "the version
    // control tool") is weaker evidence than a phrase.
    static constexpr float kMeaning = 0.79f;
    static constexpr float kMeaningWord = 0.85f;
    // How much better than its topic word alone a phrase holding one must match ("fan control" against "fan").
    static constexpr float kLift = 0.05f;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether a process is the program a word names: its name, its executable's file name, or the program or first
// file its command line names, compared without case, version or extension ("node" is node-MainThread running
// /usr/bin/node; "fan-control" is /bin/bash /usr/local/sbin/fan-control.sh; "python" is python3.14).
bool is_program(const std::string& word, const std::string& name, const std::string& exe, const std::string& cmdline);
// The same test as an SQL function, is_program(word, name, exe, cmdline), so a filter reads
// "AND is_program(?, name, exe, cmdline)" in place of "AND name = ?".
void add_program_function(sqlite3* db);
// A unit with its random parts folded to * ("vte-spawn-6a3f...-39196307af6b.scope" -> "vte-spawn-*.scope") and
// systemd's escapes undone ("app-realm\x2dos@..." -> "app-realm-os@*.service").
std::string fold_unit(const std::string& unit);

}  // namespace as
