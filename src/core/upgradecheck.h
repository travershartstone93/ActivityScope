#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "updates.h"

namespace as {

// ---- building blocks (each tested on its own) ----

struct CmdResult {
    int code = -1;
    std::string out;  // stdout and stderr together
};
using Runner = std::function<CmdResult(const std::vector<std::string>& argv)>;
// Runs a program with LC_ALL=C (so pacman's messages are in a fixed form).
CmdResult run_command(const std::vector<std::string>& argv);

// "pacman -Qi/-Si" output: one block per package, keyed by Name. When a
// package is in several repositories, the first (the one pacman uses) wins.
using PkgInfo = std::map<std::string, std::string>;
std::map<std::string, PkgInfo> parse_pacman_info(const std::string& text);
std::vector<std::string> info_list(const PkgInfo& info, const std::string& field);  // "None" -> empty

// pacman's version ordering (epoch:version-release).
int vercmp(const std::string& a, const std::string& b);
struct DepSpec { std::string name, op, version; };
DepSpec parse_dep(const std::string& dep);  // "audit=4.1.4", "gcc<15", "libfoo.so=76-64"
bool satisfies(const std::string& version, const DepSpec& dep);

// The reasons pacman gives for refusing a transaction.
struct Refusal {
    enum Kind { BreaksDep, Unsatisfied, Conflict, NotFound, Other } kind = Other;
    std::string package, version;  // the package being installed (BreaksDep, Conflict)
    std::string dep;               // the requirement involved
    std::string by;                // the package that requires it
    std::string raw;
};
std::vector<Refusal> parse_refusals(const std::string& pacman_output);

// Shared libraries a program file needs (DT_NEEDED), read straight from the ELF file.
std::vector<std::string> elf_needed(const std::string& path);
// "libicuuc.so=76-64" -> "libicuuc.so.76"; empty if it is not a library provide.
std::string soname_of_provide(const std::string& provide);
double parse_size(const std::string& text);  // "12.34 MiB" -> bytes

// ---- the check ----

struct Fix {
    std::string kind;                  // remove, hold, rebuild
    std::vector<std::string> targets;  // what the button acts on
    std::vector<std::string> removes;  // everything a removal takes out (from pacman's preview)
    std::string command;
    std::string label;                 // button text
};

struct Issue {
    std::string severity;  // fail (the upgrade will stop), warn (something will break), info
    std::string kind;      // refusal, aur-library, kernel, held, python, disk
    std::string title, explanation;
    std::vector<std::string> facts;
    std::vector<Fix> fixes;
    std::string raw;       // pacman's own words, when there are any
    bool tested = false;   // the fix was applied to a copy and the upgrade re-checked
};

struct CheckInput {
    std::string sync_db;    // folder holding the freshly downloaded package lists (checkupdates' "sync")
    std::string local_db = "/var/lib/pacman/local";
    std::string scratch;    // a folder the check may fill with its copy
    std::set<std::string> held;
    std::vector<PendingUpdate> updates;
    std::string home;
    std::string cache_dir = "/var/cache/pacman/pkg";
    std::string root = "/";
    std::function<void(int percent, const std::string& what)> progress;  // called from the checking thread
};

struct CheckResult {
    std::vector<Issue> issues;
    bool installs_cleanly = false;     // with every fix applied, the dry run succeeds
    bool needed_fixes = false;
    int planned = 0;                   // packages the final dry run would install
    double download_bytes = 0, extra_bytes = 0, free_bytes = 0;
};

CheckResult check_upgrade(const CheckInput& in, const Runner& run = run_command);

}  // namespace as
