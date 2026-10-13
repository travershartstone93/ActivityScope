#include "upgradecheck.h"

#include <poll.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>

#include "events.h"
#include "fs.h"

namespace as {

namespace fs = std::filesystem;

namespace {
double monotonic_now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}
}  // namespace

CmdResult run_command(const std::vector<std::string>& argv, int timeout_s) {
    CmdResult r;
    int pipefd[2];
    if (argv.empty() || pipe(pipefd) != 0) return r;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return r;
    }
    if (pid == 0) {
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
        setenv("LC_ALL", "C", 1);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(pipefd[1]);
    // A wedged child must not hang the page's analysis thread forever.
    char buf[65536];
    double deadline = monotonic_now() + timeout_s;
    bool killed = false;
    for (;;) {
        pollfd pf{pipefd[0], POLLIN, 0};
        int pr = poll(&pf, 1, 1000);
        if (pr > 0) {
            ssize_t n = read(pipefd[0], buf, sizeof buf);
            if (n <= 0) break;
            r.out.append(buf, std::size_t(n));
        }
        if (monotonic_now() > deadline) {
            kill(pid, SIGKILL);
            killed = true;
            r.out += "\n(ActivityScope: the command did not finish in time and was stopped)";
            break;
        }
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (killed) return r;
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

std::map<std::string, PkgInfo> parse_pacman_info(const std::string& text) {
    std::map<std::string, PkgInfo> out;
    PkgInfo cur;
    std::string last;
    auto flush = [&] {
        auto n = cur.find("Name");
        if (n != cur.end() && !out.count(n->second)) out[n->second] = cur;
        cur.clear();
        last.clear();
    };
    for (const auto& line : split(text, '\n')) {
        if (trim(line).empty()) {
            flush();
            continue;
        }
        auto colon = line.find(" : ");
        if (colon != std::string::npos && line[0] != ' ') {
            last = trim(line.substr(0, colon));
            cur[last] = trim(line.substr(colon + 3));
        } else if (!last.empty()) {
            cur[last] += " " + trim(line);  // wrapped continuation lines
        }
    }
    flush();
    return out;
}

std::vector<std::string> info_list(const PkgInfo& info, const std::string& field) {
    std::vector<std::string> out;
    auto it = info.find(field);
    if (it == info.end() || it->second == "None") return out;
    for (auto v : split_ws(it->second)) out.emplace_back(v);
    return out;
}

namespace {

// rpmvercmp, as pacman uses it: compare alternating runs of digits and letters.
int rpmvercmp(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    std::size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        while (i < a.size() && !std::isalnum(static_cast<unsigned char>(a[i]))) ++i;
        while (j < b.size() && !std::isalnum(static_cast<unsigned char>(b[j]))) ++j;
        if (i >= a.size() || j >= b.size()) break;
        // pacman also counts separators: one with more separator runs left is newer
        bool num = std::isdigit(static_cast<unsigned char>(a[i]));
        std::size_t si = i, sj = j;
        if (num) {
            while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
            while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
        } else {
            while (i < a.size() && std::isalpha(static_cast<unsigned char>(a[i]))) ++i;
            while (j < b.size() && std::isalpha(static_cast<unsigned char>(b[j]))) ++j;
        }
        std::string x = a.substr(si, i - si), y = b.substr(sj, j - sj);
        if (y.empty()) return num ? 1 : -1;  // numeric beats alpha
        if (num) {
            x.erase(0, std::min(x.find_first_not_of('0'), x.size()));
            y.erase(0, std::min(y.find_first_not_of('0'), y.size()));
            if (x.size() != y.size()) return x.size() > y.size() ? 1 : -1;
        }
        int c = x.compare(y);
        if (c != 0) return c > 0 ? 1 : -1;
    }
    bool a_left = i < a.size(), b_left = j < b.size();
    if (!a_left && !b_left) return 0;
    // the one with something left is newer, unless what is left starts with a letter (alpha < nothing)
    if (a_left) return std::isalpha(static_cast<unsigned char>(a[i])) ? -1 : 1;
    return std::isalpha(static_cast<unsigned char>(b[j])) ? 1 : -1;
}

void split_evr(const std::string& v, std::string& e, std::string& ver, std::string& rel) {
    std::string s = v;
    auto colon = s.find(':');
    e = colon == std::string::npos ? "0" : s.substr(0, colon);
    if (colon != std::string::npos) s = s.substr(colon + 1);
    auto dash = s.rfind('-');
    ver = dash == std::string::npos ? s : s.substr(0, dash);
    rel = dash == std::string::npos ? "" : s.substr(dash + 1);
}

}  // namespace

int vercmp(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    std::string ea, va, ra, eb, vb, rb;
    split_evr(a, ea, va, ra);
    split_evr(b, eb, vb, rb);
    if (int c = rpmvercmp(ea, eb)) return c;
    if (int c = rpmvercmp(va, vb)) return c;
    if (!ra.empty() && !rb.empty()) return rpmvercmp(ra, rb);
    return 0;
}

DepSpec parse_dep(const std::string& dep) {
    DepSpec d;
    auto p = dep.find_first_of("<>=");
    if (p == std::string::npos) {
        d.name = dep;
        return d;
    }
    d.name = dep.substr(0, p);
    std::size_t q = p;
    while (q < dep.size() && (dep[q] == '<' || dep[q] == '>' || dep[q] == '=')) ++q;
    d.op = dep.substr(p, q - p);
    d.version = dep.substr(q);
    return d;
}

bool satisfies(const std::string& version, const DepSpec& d) {
    if (d.op.empty()) return true;
    // A requirement without a release ("4.1.4") matches any release of that version.
    std::string v = version;
    if (d.version.find('-') == std::string::npos) {
        auto dash = v.rfind('-');
        if (dash != std::string::npos) v = v.substr(0, dash);
    }
    int c = vercmp(v, d.version);
    if (d.op == "=") return c == 0;
    if (d.op == ">=") return c >= 0;
    if (d.op == "<=") return c <= 0;
    if (d.op == ">") return c > 0;
    if (d.op == "<") return c < 0;
    return true;
}

std::vector<Refusal> parse_refusals(const std::string& out) {
    std::vector<Refusal> r;
    static const std::regex breaks(R"(:: installing (\S+) \((\S+)\) breaks dependency '([^']+)' required by (\S+))");
    static const std::regex removing(R"(:: removing (\S+) breaks dependency '([^']+)' required by (\S+))");
    static const std::regex unsat(R"(:: unable to satisfy dependency '([^']+)' required by (\S+))");
    static const std::regex conflict(R"(:: (\S+) and (\S+) are in conflict)");
    static const std::regex notfound(R"(error: target not found: (\S+))");
    for (const auto& line : split(out, '\n')) {
        std::smatch m;
        Refusal x;
        x.raw = trim(line);
        if (std::regex_search(line, m, breaks)) x = {Refusal::BreaksDep, m[1], m[2], m[3], m[4], x.raw};
        else if (std::regex_search(line, m, removing)) x = {Refusal::BreaksDep, m[1], "", m[2], m[3], x.raw};
        else if (std::regex_search(line, m, unsat)) x = {Refusal::Unsatisfied, "", "", m[1], m[2], x.raw};
        else if (std::regex_search(line, m, conflict)) x = {Refusal::Conflict, m[1], "", m[2], "", x.raw};
        else if (std::regex_search(line, m, notfound)) x = {Refusal::NotFound, m[1], "", "", "", x.raw};
        else continue;
        r.push_back(x);
    }
    return r;
}

std::vector<std::string> elf_needed(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    unsigned char ident[16];
    if (!f.read(reinterpret_cast<char*>(ident), 16) || std::memcmp(ident, "\x7f" "ELF", 4) != 0) return out;
    // Both ELF classes: 32-bit matters here, because the lib32 family is
    // exactly where upgrades strand programs.
    const bool is64 = ident[4] == 2;
    if ((ident[4] != 1 && ident[4] != 2) || ident[5] != 1) return out;  // little-endian only
    auto rd = [&](uint64_t off, void* p, std::size_t n) {
        f.clear();
        f.seekg(std::streamoff(off));
        return bool(f.read(static_cast<char*>(p), std::streamsize(n)));
    };
    auto rdv = [&](uint64_t off) -> uint64_t {  // one pointer-sized field
        uint64_t v64 = 0;
        uint32_t v32 = 0;
        if (is64) return rd(off, &v64, 8) ? v64 : 0;
        return rd(off, &v32, 4) ? v32 : 0;
    };
    uint64_t shoff = rdv(is64 ? 0x28 : 0x20);
    uint16_t shentsize = 0, shnum = 0;
    rd(is64 ? 0x3A : 0x2E, &shentsize, 2);
    rd(is64 ? 0x3C : 0x30, &shnum, 2);
    if (shoff == 0 || shentsize < (is64 ? 64 : 40) || shnum == 0 || shnum > 4096) return out;
    struct Sec { uint32_t type, link; uint64_t offset, size; };
    std::vector<Sec> secs(shnum);
    for (uint16_t i = 0; i < shnum; ++i) {
        uint64_t base = shoff + uint64_t(i) * shentsize;
        rd(base + 4, &secs[i].type, 4);
        secs[i].offset = rdv(base + (is64 ? 0x18 : 16));
        secs[i].size = rdv(base + (is64 ? 0x20 : 20));
        rd(base + (is64 ? 0x28 : 24), &secs[i].link, 4);
    }
    const uint64_t stride = is64 ? 16 : 8;
    for (const auto& s : secs) {
        if (s.type != 6 /* SHT_DYNAMIC */ || s.link >= secs.size() || s.size > (1 << 20)) continue;
        const Sec& str = secs[s.link];
        if (str.size > (16 << 20)) continue;
        std::string strtab(std::size_t(str.size), '\0');
        if (!rd(str.offset, strtab.data(), strtab.size())) continue;
        for (uint64_t off = 0; off + stride <= s.size; off += stride) {
            uint64_t tag = rdv(s.offset + off);
            uint64_t val = rdv(s.offset + off + stride / 2);
            if (tag == 0) break;
            if (tag == 1 /* DT_NEEDED */ && val < strtab.size()) out.emplace_back(strtab.c_str() + val);
        }
    }
    return out;
}

std::string soname_of_provide(const std::string& provide) {
    // Arch writes library provides as "libfoo.so=VERSION-64".
    auto d = parse_dep(provide);
    if (d.op != "=" || d.name.find(".so") == std::string::npos) return {};
    auto dash = d.version.rfind('-');
    std::string ver = dash == std::string::npos ? d.version : d.version.substr(0, dash);
    return d.name + "." + ver;
}

double parse_size(const std::string& text) {
    auto f = split_ws(text);
    if (f.empty()) return 0;
    double v = std::atof(std::string(f[0]).c_str());
    std::string u = f.size() > 1 ? std::string(f[1]) : "B";
    if (u == "KiB") v *= 1024;
    else if (u == "MiB") v *= 1024.0 * 1024;
    else if (u == "GiB") v *= 1024.0 * 1024 * 1024;
    return v;
}

namespace {

bool official_packager(const PkgInfo& i) {
    auto p = i.find("Packager");
    if (p == i.end()) return false;
    return p->second.find("archlinux.org") != std::string::npos || p->second.find("cachyos.org") != std::string::npos;
}

std::string join(const std::vector<std::string>& v, const std::string& sep = ", ", const std::string& last = " and ") {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i == 0 ? "" : i + 1 == v.size() ? last : sep) + v[i];
    return s;
}

std::string family_note(const std::vector<std::string>& names) {
    bool lib32 = std::any_of(names.begin(), names.end(), [](const std::string& n) { return starts_with(n, "lib32-"); });
    bool dkms = std::any_of(names.begin(), names.end(), [](const std::string& n) { return n.find("-dkms") != std::string::npos; });
    if (lib32) return " Packages starting with lib32- are 32-bit copies of libraries, kept for older 32-bit programs such as Steam games and Wine.";
    if (dkms) return " Packages ending in -dkms are drivers that are rebuilt for each kernel.";
    return {};
}

struct Work {
    const CheckInput& in;
    const Runner& run;
    std::string dir;   // scratch database: local (copy) + sync (link)
    std::set<std::string> ignore;
    std::map<std::string, PkgInfo> local, sync;

    std::string ignore_arg() const {
        std::string s;
        for (const auto& i : ignore) s += (s.empty() ? "" : ",") + i;
        return s;
    }
    CmdResult dry_run() const {
        std::vector<std::string> argv = {"pacman", "-Sup", "--dbpath", dir, "--logfile", "/dev/null", "--noconfirm", "--print-format", "%n %v"};
        if (!ignore.empty()) argv.push_back("--ignore"), argv.push_back(ignore_arg());
        return run(argv);
    }
    void load_local(const std::vector<std::string>& names) {
        std::vector<std::string> want;
        for (const auto& n : names)
            if (!local.count(n)) want.push_back(n);
        if (want.empty()) return;
        std::vector<std::string> argv = {"pacman", "-Qi", "--dbpath", dir};
        argv.insert(argv.end(), want.begin(), want.end());
        for (auto& [k, v] : parse_pacman_info(run(argv).out)) local[k] = v;
    }
    void load_sync(const std::vector<std::string>& names) {
        std::vector<std::string> want;
        for (const auto& n : names)
            if (!sync.count(n)) want.push_back(n);
        if (want.empty()) return;
        std::vector<std::string> argv = {"pacman", "-Si", "--dbpath", dir};
        argv.insert(argv.end(), want.begin(), want.end());
        auto got = parse_pacman_info(run(argv).out);
        for (const auto& n : want) sync[n] = got.count(n) ? got[n] : PkgInfo{};
    }
    bool in_repos(const std::string& n) {
        load_sync({n});
        return !sync[n].empty();
    }
    // Remove packages from the scratch copy (never from the real system).
    void drop_local(const std::vector<std::string>& names) {
        for (const auto& n : names) {
            load_local({n});
            auto v = local[n].find("Version");
            if (v == local[n].end()) continue;
            std::error_code ec;
            fs::remove_all(dir + "/local/" + n + "-" + v->second, ec);
        }
    }
};

// Everything that (directly or through others) requires `pkg`, with pkg itself.
std::vector<std::string> requirers_closure(Work& w, const std::string& pkg) {
    std::vector<std::string> order = {pkg}, queue = {pkg};
    std::set<std::string> seen = {pkg};
    while (!queue.empty() && order.size() < 40) {
        std::string p = queue.back();
        queue.pop_back();
        w.load_local({p});
        for (const auto& r : info_list(w.local[p], "Required By"))
            if (seen.insert(r).second) order.push_back(r), queue.push_back(r);
    }
    return order;
}

void diagnose(Work& w, const Refusal& ref, CheckResult& res, bool& progressed) {
    Issue is;
    is.severity = "fail";
    is.kind = "refusal";
    is.raw = ref.raw;
    progressed = false;
    std::string blocker = ref.by;
    DepSpec need = parse_dep(ref.dep);

    if (ref.kind == Refusal::BreaksDep || ref.kind == Refusal::Unsatisfied) {
        w.load_local({blocker});
        const PkgInfo& bl = w.local[blocker];
        std::string newer;
        if (!ref.package.empty()) newer = ref.package + " " + ref.version;

        // A held package (cuda) is involved: the fix is to hold what needs it too.
        bool held_involved = w.in.held.count(need.name) || w.in.held.count(ref.package);
        if (held_involved && !blocker.empty() && !w.in.held.count(blocker)) {
            w.load_local({need.name});
            std::string have = w.local[need.name].count("Version") ? w.local[need.name]["Version"] : "?";
            is.title = blocker + " needs a newer " + need.name + ", which you keep at its current version";
            is.explanation = "The update for " + blocker + " requires " + ref.dep + ", but " + need.name + " is held at " + have +
                             ". Updating everything else is fine; " + blocker + " has to wait as well.";
            is.fixes.push_back({"hold", {blocker}, {}, "", "Hold " + blocker + " too"});
            w.ignore.insert(blocker);
            progressed = true;
            res.issues.push_back(is);
            return;
        }

        bool gone = !blocker.empty() && !w.in_repos(blocker);
        if (gone && official_packager(bl)) {
            // The package that pins the old version has been dropped from the repositories.
            auto chain = requirers_closure(w, blocker);
            bool all_gone = true;
            std::vector<std::string> tops;
            for (const auto& p : chain) {
                if (w.in_repos(p)) all_gone = false;
                w.load_local({p});
                if (info_list(w.local[p], "Required By").empty()) tops.push_back(p);
            }
            if (all_gone && !tops.empty()) {
                std::vector<std::string> argv = {"pacman", "-Rsp", "--dbpath", w.dir, "--logfile", "/dev/null", "--print-format", "%n"};
                argv.insert(argv.end(), tops.begin(), tops.end());
                auto prev = w.run(argv);
                std::vector<std::string> removes;
                for (const auto& l : split(prev.out, '\n'))
                    if (!trim(l).empty() && trim(l).find(' ') == std::string::npos) removes.push_back(trim(l));
                bool explicit_top = false;
                for (const auto& t : tops)
                    if (w.local[t]["Install Reason"].find("Explicitly") != std::string::npos) explicit_top = true;
                is.title = "Update all will stop: " + (ref.package.empty() ? need.name : ref.package) + " is blocked by " + blocker +
                           ", which is no longer in the repositories";
                is.explanation = (ref.kind == Refusal::BreaksDep
                                      ? "Installing " + newer + " breaks a requirement of " + blocker + ": it needs exactly " + ref.dep + ". "
                                      : blocker + " needs " + ref.dep + ", which is no longer available. ") +
                                 join(chain) + (chain.size() == 1 ? " is" : " are") +
                                 " no longer in the repositories, so no newer version will come to fix this. " +
                                 (explicit_top ? "You installed " + join(tops) + " yourself, so check you no longer need " +
                                                     (tops.size() == 1 ? "it" : "them") + "."
                                               : "Nothing else on this system uses " + std::string(chain.size() == 1 ? "it" : "them") +
                                                     " (" + (chain.size() == 1 ? "it was" : "they were") + " installed as dependencies).") +
                                 family_note(chain);
                for (const auto& p : chain) {
                    w.load_local({p});
                    is.facts.push_back(p + " " + w.local[p]["Version"] + ": " + w.local[p]["Description"]);
                }
                Fix fx{"remove", tops, removes, "sudo pacman -Rns " + join(tops, " ", " "),
                       removes.size() > 1 ? "Remove broken packages" : "Remove broken package"};
                is.fixes.push_back(fx);
                w.drop_local(removes.empty() ? chain : removes);
                progressed = true;
                res.issues.push_back(is);
                return;
            }
        }
        if (gone) {
            // Built locally or from the AUR: it pins the old version until rebuilt.
            is.title = blocker + " (not from the repositories) holds " + need.name + " back";
            is.explanation = blocker + " was built on this machine (AUR or a local build) and requires " + ref.dep +
                             ". Hold " + need.name + " for now; after the rest is updated, rebuild " + blocker +
                             " so it accepts the new version.";
            is.fixes.push_back({"hold", {need.name}, {}, "", "Hold " + need.name + " for now"});
            is.fixes.push_back({"rebuild", {blocker}, {}, "paru -S " + blocker, "Rebuild " + blocker + " after updating"});
            w.ignore.insert(need.name);
            progressed = true;
            res.issues.push_back(is);
            return;
        }
    }
    if (ref.kind == Refusal::Conflict) {
        is.severity = "warn";
        is.title = ref.package + " replaces " + ref.dep;
        is.explanation = "The two cannot be installed together. During Update all pacman will ask whether to remove " + ref.dep +
                         "; answering yes is normally right when one package replaces another.";
        res.issues.push_back(is);
        return;
    }
    is.title = "pacman refused the update";
    is.explanation = "This is not one of the cases ActivityScope knows how to explain. pacman's own words are below.";
    res.issues.push_back(is);
}

}  // namespace

CheckResult check_upgrade(const CheckInput& in, const Runner& run) {
    CheckResult res;
    auto step = [&](int pct, const std::string& what) {
        if (in.progress) in.progress(pct, what);
    };
    step(2, "Copying the package database");
    Work w{in, run, in.scratch + "/db", in.held, {}, {}};
    std::error_code ec;
    fs::remove_all(w.dir, ec);
    fs::create_directories(w.dir, ec);
    fs::copy(in.local_db, w.dir + "/local", fs::copy_options::recursive, ec);
    bool copied = !ec && fs::exists(w.dir + "/local") && !fs::is_empty(w.dir + "/local", ec) && !ec;
    fs::create_symlink(in.sync_db, w.dir + "/sync", ec);
    if (!copied || ec) {
        // A rehearsal against a broken copy would report confident nonsense.
        Issue is;
        is.severity = "fail";
        is.kind = "internal";
        is.title = "The safety check could not run";
        is.explanation = "Copying the package database for the rehearsal failed (likely disk space or permissions under " +
                         in.scratch + "). Nothing is wrong with the updates themselves; the check simply could not look.";
        res.issues.push_back(is);
        fs::remove_all(w.dir, ec);
        return res;
    }

    // 1. Will the upgrade run? Diagnose each refusal, apply its fix to the copy, try again.
    for (int round = 0; round < 6; ++round) {
        step(8 + round * 7, round == 0 ? "Rehearsing Update all on the copy" : "Testing the fix and rehearsing again");
        CmdResult r = w.dry_run();
        if (r.code == 0) {
            res.installs_cleanly = true;
            // Only "name version" lines count; stderr is merged into the
            // output, so pacman's warnings must not inflate the number.
            for (const auto& l : split(r.out, '\n')) {
                auto f = split_ws(trim(l));
                if (f.size() == 2 && f[0].find(':') == std::string_view::npos) ++res.planned;
            }
            break;
        }
        auto refusals = parse_refusals(r.out);
        bool progressed = false;
        if (refusals.empty()) {
            Issue is{"fail", "refusal", "pacman refused the update", "pacman's own words are below.", {}, {}, trim(r.out)};
            res.issues.push_back(is);
            break;
        }
        diagnose(w, refusals.front(), res, progressed);
        if (!progressed) break;
        res.needed_fixes = true;
    }
    if (res.installs_cleanly)
        for (auto& is : res.issues)
            if (is.kind == "refusal") is.tested = true;

    // Facts about every package being updated (official ones), new and current.
    std::vector<std::string> official;
    for (const auto& u : in.updates)
        if (!u.aur && !w.ignore.count(u.name)) official.push_back(u.name);
    step(50, "Reading details of the packages being updated");
    w.load_sync(official);
    w.load_local(official);
    std::map<std::string, std::string> newver;
    for (const auto& n : official)
        if (w.sync[n].count("Version")) newver[n] = w.sync[n]["Version"];

    // 2. AUR programs left behind: libraries the upgrade takes away.
    std::map<std::string, std::string> removed;  // soname -> "provider new-version"
    for (const auto& n : official) {
        std::set<std::string> now, after;
        for (const auto& p : info_list(w.local[n], "Provides"))
            if (auto s = soname_of_provide(p); !s.empty()) now.insert(s);
        for (const auto& p : info_list(w.sync[n], "Provides"))
            if (auto s = soname_of_provide(p); !s.empty()) after.insert(s);
        for (const auto& s : now)
            if (!after.count(s)) removed[s] = n + " " + newver[n];
    }
    step(60, "Checking AUR programs for libraries the update removes");
    auto foreign_out = run({"pacman", "-Qmq", "--dbpath", w.dir});
    std::vector<std::string> foreign;
    for (const auto& l : split(foreign_out.out, '\n'))
        if (!trim(l).empty()) foreign.push_back(trim(l));
    std::map<std::string, std::vector<std::string>> foreign_files;
    bool foreign_loaded = false;
    // The file lists cost one pacman run per AUR package, so they are read
    // only when a check below actually needs them.
    auto load_foreign_files = [&] {
        if (foreign_loaded) return;
        foreign_loaded = true;
        for (std::size_t fi = 0; fi < foreign.size(); ++fi) {
            const auto& pkg = foreign[fi];
            if (fi % 4 == 0) step(60 + int(20 * fi / std::max<std::size_t>(1, foreign.size())), "Checking AUR programs for libraries the update removes");
            auto files = run({"pacman", "-Qlq", "--dbpath", w.dir, pkg});
            for (const auto& f : split(files.out, '\n')) {
                std::string path = trim(f);
                if (path.empty() || path.back() == '/') continue;
                foreign_files[pkg].push_back(path);
            }
        }
    };
    if (!removed.empty()) {
        load_foreign_files();
        for (const auto& pkg : foreign) {
            std::set<std::string> hits;
            int scanned = 0;
            for (const auto& path : foreign_files[pkg]) {
                if (scanned > 400) break;
                if (path.find("/bin/") == std::string::npos && path.find("/lib") == std::string::npos) continue;
                ++scanned;
                for (const auto& needed : elf_needed(in.root + path.substr(1)))
                    if (removed.count(needed)) hits.insert(needed);
            }
            if (hits.empty()) continue;
            Issue is;
            is.severity = "warn";
            is.kind = "aur-library";
            std::vector<std::string> what;
            for (const auto& h : hits) what.push_back(h + " (" + removed[h] + " replaces it)");
            is.title = pkg + " will stop working until it is rebuilt";
            is.explanation = pkg + " was built on this machine (AUR or a local build) against " + join(std::vector<std::string>(hits.begin(), hits.end())) +
                             ", which the upgrade removes. Programs from the repositories are rebuilt for you; this one is not.";
            is.facts = what;
            is.fixes.push_back({"rebuild", {pkg}, {}, "paru -S " + pkg, "Rebuild " + pkg + " after updating"});
            res.issues.push_back(is);
        }
    }

    step(84, "Checking kernels and drivers");
    // 3. Kernels: headers must keep pace for DKMS drivers.
    std::vector<std::string> dkms;
    w.load_local({"dkms"});
    for (const auto& l : split(run({"pacman", "-Qq", "--dbpath", w.dir}).out, '\n'))
        if (trim(l).find("-dkms") != std::string::npos) dkms.push_back(trim(l));
    std::vector<std::string> rebuilt;  // kernels whose DKMS rebuild is expected to just work
    for (const auto& n : official) {
        if (!(n == "linux" || starts_with(n, "linux-")) || !needs_restart(n)) continue;
        if (n.find("nvidia") != std::string::npos || n.find("firmware") != std::string::npos || n.find("-zfs") != std::string::npos) continue;
        std::string headers = n + "-headers";
        w.load_local({headers});
        bool has_headers = !w.local[headers].empty();
        if (dkms.empty() || !has_headers) continue;
        bool headers_updating = newver.count(headers) && newver[headers] == newver[n];
        if (headers_updating) {
            rebuilt.push_back(n + " " + newver[n]);
            continue;
        }
        Issue is;
        is.kind = "kernel";
        {
            is.severity = "warn";
            is.title = join(dkms) + " cannot be rebuilt for the new " + n;
            is.explanation = n + " updates to " + newver[n] + " but " + headers + " does not reach the same version, and DKMS drivers (" +
                             join(dkms) + ") need matching headers to rebuild. After the restart those drivers would not load.";
            is.fixes.push_back({"hold", {n}, {}, "", "Hold " + n + " for now"});
        }
        res.issues.push_back(is);
    }

    if (!rebuilt.empty()) {
        Issue is;
        is.severity = "info";
        is.kind = "kernel";
        is.title = join(dkms) + " will be rebuilt for " + join(rebuilt);
        is.explanation = "DKMS drivers are compiled for each new kernel during the update. If a build fails, pacman shows the error "
                         "and that driver will not load until it is fixed; the rest of the system is unaffected.";
        res.issues.push_back(is);
    }

    step(88, "Checking held packages");
    // 4. Held packages: their versioned requirements, and libraries their programs load.
    for (const auto& h : in.held) {
        w.load_local({h});
        for (const auto& d : info_list(w.local[h], "Depends On")) {
            DepSpec dep = parse_dep(d);
            auto nv = newver.find(dep.name);
            if (nv == newver.end() || dep.op.empty() || satisfies(nv->second, dep)) continue;
            Issue is;
            is.severity = "warn";
            is.kind = "held";
            is.title = h + " (held) may stop working: " + dep.name + " moves to " + nv->second;
            is.explanation = h + " requires " + d + ", and the upgrade takes " + dep.name + " to " + nv->second + ".";
            is.fixes.push_back({"hold", {dep.name}, {}, "", "Hold " + dep.name + " too"});
            res.issues.push_back(is);
        }
        if (removed.empty()) continue;
        std::map<std::string, std::vector<std::string>> hits;  // soname -> files
        int scanned = 0;
        for (const auto& l : split(run({"pacman", "-Qlq", "--dbpath", w.dir, h}).out, '\n')) {
            std::string path = trim(l);
            if (path.empty() || path.back() == '/' || scanned > 3000) continue;
            if (path.find("/bin/") == std::string::npos && path.find(".so") == std::string::npos) continue;
            ++scanned;
            for (const auto& needed : elf_needed(in.root + path.substr(1)))
                if (removed.count(needed)) hits[needed].push_back(path);
        }
        for (const auto& [so, files] : hits) {
            auto provider = split(removed[so], ' ')[0];
            Issue is;
            is.severity = "warn";
            is.kind = "held";
            is.title = h + " (held) loses " + so;
            is.explanation = std::to_string(files.size()) + " of " + h + "'s files load " + so + ", which " + removed[so] +
                             " no longer provides. Hold " + provider + " as well to keep them working.";
            for (std::size_t i = 0; i < files.size() && i < 5; ++i) is.facts.push_back(files[i]);
            is.fixes.push_back({"hold", {provider}, {}, "", "Hold " + provider + " too"});
            res.issues.push_back(is);
        }
    }

    step(93, "Checking Python");
    // 5. Python moving to a new 3.x.
    if (newver.count("python")) {
        std::string cur = w.local["python"]["Version"], nxt = newver["python"];
        auto mm = [](const std::string& v) {
            auto p = split(v, '.');
            return p.size() >= 2 ? p[0] + "." + p[1] : v;
        };
        if (!cur.empty() && mm(cur) != mm(nxt)) {
            Issue is;
            is.severity = "warn";
            is.kind = "python";
            is.title = "Python moves from " + mm(cur) + " to " + mm(nxt);
            std::string site = in.home + "/.local/lib/python" + mm(cur) + "/site-packages";
            int pip = 0;
            for (const auto& e : list_dir(site))
                if (e.ends_with(".dist-info")) ++pip;
            std::vector<std::string> venvs;
            std::error_code e2;
            auto it = fs::recursive_directory_iterator(in.home, fs::directory_options::skip_permission_denied, e2);
            int visited = 0;
            for (; it != fs::recursive_directory_iterator() && visited < 20000; it.increment(e2), ++visited) {
                if (e2) break;
                const auto name = it->path().filename().string();
                if (it->is_directory(e2) && (name == ".cache" || name == "node_modules" || name == ".git" || name == ".local" ||
                                             name == ".mozilla" || name == ".steam" || it.depth() >= 4)) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (name == "pyvenv.cfg") {
                    std::ifstream cfg(it->path());
                    std::string text((std::istreambuf_iterator<char>(cfg)), {});
                    if (text.find("version = " + mm(cur)) != std::string::npos || text.find("version_info = " + mm(cur)) != std::string::npos)
                        venvs.push_back(it->path().parent_path().string());
                }
            }
            std::vector<std::string> aurpy;
            load_foreign_files();
            for (const auto& pkg : foreign)
                for (const auto& f : foreign_files[pkg])
                    if (f.find("/usr/lib/python" + mm(cur) + "/") == 0) {
                        aurpy.push_back(pkg);
                        break;
                    }
            is.explanation = "Python libraries are kept per version, so things installed for " + mm(cur) + " are not seen by " + mm(nxt) + ".";
            if (pip) is.facts.push_back(std::to_string(pip) + " packages installed with pip in " + site + " will need reinstalling");
            for (const auto& v : venvs) is.facts.push_back("Virtual environment " + v + " will need recreating");
            for (const auto& p : aurpy) {
                is.facts.push_back(p + " (AUR) will need rebuilding");
                is.fixes.push_back({"rebuild", {p}, {}, "paru -S " + p, "Rebuild " + p + " after updating"});
            }
            if (is.facts.empty()) is.severity = "info", is.explanation += " Nothing installed for " + mm(cur) + " outside the repositories was found.";
            res.issues.push_back(is);
        }
    }

    step(97, "Checking disk space");
    // 6. Disk space.
    for (const auto& n : official) {
        res.download_bytes += parse_size(w.sync[n].count("Download Size") ? w.sync[n]["Download Size"] : "0 B");
        res.extra_bytes += parse_size(w.sync[n].count("Installed Size") ? w.sync[n]["Installed Size"] : "0 B") -
                           parse_size(w.local[n].count("Installed Size") ? w.local[n]["Installed Size"] : "0 B");
    }
    struct statvfs sv{};
    if (statvfs(in.cache_dir.c_str(), &sv) == 0) res.free_bytes = double(sv.f_bavail) * double(sv.f_frsize);
    double need = res.download_bytes + std::max(0.0, res.extra_bytes) + 2e9;  // 2 GB to spare
    if (res.free_bytes > 0 && res.free_bytes < need) {
        Issue is;
        is.severity = "fail";
        is.kind = "disk";
        is.title = "Not enough disk space for the update";
        is.explanation = "The update downloads " + human_bytes(res.download_bytes) + " and needs " + human_bytes(std::max(0.0, res.extra_bytes)) +
                         " more once installed, but only " + human_bytes(res.free_bytes) + " is free. Clear the package cache "
                         "(paccache -rk1) or free some space first.";
        res.issues.push_back(is);
    }
    fs::remove_all(w.dir, ec);
    step(100, "Done");
    return res;
}

}  // namespace as
