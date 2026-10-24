#include "names.h"

#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>
#include <unordered_map>

#include "facts.h"
#include "snapshot.h"
#include "unllm/meaning.h"

namespace as {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string_view base_of(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    if (auto d = path.find(" (deleted)"); d != std::string_view::npos) path = path.substr(0, d);
    auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// A name without its extension or version: "fan-control.sh" is fan-control, "python3.14" and "python3" are python.
// A file's digits stay: "hard50.py" is hard50, not "hard".
std::string_view bare(std::string_view n) {
    if (auto dot = n.rfind('.'); dot != std::string_view::npos && dot > 0 && n.size() - dot - 1 <= 4 &&
                                 n.size() > dot + 1 &&
                                 std::all_of(n.begin() + std::ptrdiff_t(dot) + 1, n.end(),
                                             [](char c) { return std::isalpha(static_cast<unsigned char>(c)); }))
        return n.substr(0, dot);
    while (!n.empty() && (std::isdigit(static_cast<unsigned char>(n.back())) || n.back() == '.')) n.remove_suffix(1);
    return n;
}

// A word as typed, ready to compare with names.
struct Word {
    std::string w;     // lowercased
    std::string core;  // without extension or version, "" when too short to mean anything alone
    explicit Word(const std::string& word) : w(lower(word)) {
        std::string_view b = bare(w);
        if (b.size() >= 3) core = std::string(b);
    }
    bool names(std::string_view n) const {
        if (n.empty()) return false;
        if (iequal(w, n)) return true;
        if (core.empty()) return false;
        return iequal(core, bare(n));
    }
};

// The command line's words: the program, then the first word that is not an option (a script, a file).
void command_words(std::string_view cmdline, std::string_view* program, std::string_view* first) {
    *program = *first = {};
    std::size_t i = 0;
    auto next = [&]() -> std::string_view {
        while (i < cmdline.size() && cmdline[i] == ' ') ++i;
        std::size_t s = i;
        while (i < cmdline.size() && cmdline[i] != ' ') ++i;
        return cmdline.substr(s, i - s);
    };
    *program = next();
    for (std::string_view w = next(); !w.empty(); w = next())
        if (w[0] != '-') {
            *first = w;
            break;
        }
}

bool matches(const Word& w, std::string_view name, std::string_view exe, std::string_view cmdline) {
    if (w.w.empty()) return false;
    if (w.names(name)) return true;
    if (!exe.empty() && w.names(base_of(exe))) return true;
    std::string_view prog, first;
    command_words(cmdline, &prog, &first);
    // The first argument is a file the program runs (a path, or a name with an extension), not any word handed to it:
    // "ask chrome safe to run?" ran ask, not chrome.
    const bool file = first.find('/') != std::string_view::npos || first.find('.') != std::string_view::npos;
    return w.names(base_of(prog)) || (file && w.names(base_of(first)));
}

void sql_is_program(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    auto text = [&](int i) -> std::string_view {
        const unsigned char* t = i < argc ? sqlite3_value_text(argv[i]) : nullptr;
        return t ? std::string_view(reinterpret_cast<const char*>(t)) : std::string_view();
    };
    // The word is the same on every row: prepared once per statement.
    auto* w = static_cast<Word*>(sqlite3_get_auxdata(ctx, 0));
    if (!w) {
        w = new Word(std::string(text(0)));
        sqlite3_set_auxdata(ctx, 0, w, [](void* p) { delete static_cast<Word*>(p); });
        w = static_cast<Word*>(sqlite3_get_auxdata(ctx, 0));
        if (!w) {
            sqlite3_result_int(ctx, matches(Word(std::string(text(0))), text(1), text(2), text(3)) ? 1 : 0);
            return;
        }
    }
    sqlite3_result_int(ctx, matches(*w, text(1), text(2), text(3)) ? 1 : 0);
}

std::string run_out(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        std::size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        pclose(p);
    }
    return out;
}

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// A description without the process numbers in it ("VTE child process 1497536 launched by ..." is the same
// description for every tab).
std::string without_numbers(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        const bool alone = (i == 0 || s[i - 1] == ' ') && (j == s.size() || s[j] == ' ');
        if (j - i >= 3 && alone) {
            i = j;
            continue;
        }
        if (j > i) {
            out.append(s, i, j - i);
            i = j;
        } else {
            out += s[i++];
        }
    }
    std::string collapsed;
    for (char c : out)
        if (!(c == ' ' && (collapsed.empty() || collapsed.back() == ' '))) collapsed += c;
    return trim(collapsed);
}

// A fact's explanation in its parts: "the KDE window manager and compositor; every window is drawn through it" is
// two descriptions; a part in brackets ("(ran inside x.service)") says where it ran, not what it is. Commas stay:
// "fast, Private & Safe Web Browser" cut there leaves "fast", which describes nothing.
std::vector<std::string> clauses(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    auto flush = [&] {
        std::string t = trim(cur);
        if (t.size() >= 3) out.push_back(t);
        cur.clear();
    };
    for (char c : text) {
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            depth = std::max(0, depth - 1);
        } else if (depth == 0) {
            if (c == ';' || c == ':') flush();
            else cur += c;
        }
    }
    flush();
    return out;
}

bool number_word(const std::string& w) {
    return !w.empty() && std::all_of(w.begin(), w.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ':' || c == '%';
    });
}

}  // namespace

std::string fold_unit(const std::string& unit) {
    // systemd's escapes: "\x2d" is a dash.
    std::string u;
    for (std::size_t i = 0; i < unit.size(); ++i) {
        if (unit[i] == '\\' && i + 3 < unit.size() && unit[i + 1] == 'x' && std::isxdigit(static_cast<unsigned char>(unit[i + 2])) &&
            std::isxdigit(static_cast<unsigned char>(unit[i + 3]))) {
            u += char(std::strtol(unit.substr(i + 2, 2).c_str(), nullptr, 16));
            i += 3;
        } else {
            u += unit[i];
        }
    }
    // A run of hex digits and dashes with six digits or more, at least one of them decimal, is an id made at
    // random (a uuid, a pid): every instance of the unit gets its own.
    std::string out;
    for (std::size_t i = 0; i < u.size();) {
        std::size_t j = i;
        int digits = 0, decimal = 0;
        while (j < u.size() && (std::isxdigit(static_cast<unsigned char>(u[j])) || u[j] == '-')) {
            if (u[j] != '-') ++digits;
            if (std::isdigit(static_cast<unsigned char>(u[j]))) ++decimal;
            ++j;
        }
        if (j == i) {
            out += u[i++];
            continue;
        }
        std::size_t a = i, b = j;  // dashes at the ends belong to the name around it
        while (a < b && u[a] == '-') ++a;
        while (b > a && u[b - 1] == '-') --b;
        if (digits >= 6 && decimal > 0 && b > a)
            out += u.substr(i, a - i) + "*" + u.substr(b, j - b);
        else
            out += u.substr(i, j - i);
        i = j;
    }
    return out;
}

bool is_program(const std::string& word, const std::string& name, const std::string& exe, const std::string& cmdline) {
    return matches(Word(word), name, exe, cmdline);
}

void add_program_function(sqlite3* db) {
    sqlite3_create_function_v2(db, "is_program", 4, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, sql_is_program,
                               nullptr, nullptr, nullptr);
}

struct Names::Impl {
    sqlite3* db = nullptr;
    const unllm::Meaning* model = nullptr;
    const Facts* facts = nullptr;
    const unllm::Analyzer* an = nullptr;
    std::vector<Program> candidates;
    // Each candidate's names (process, executable, script), its own descriptions (desktop entry, facts), and its
    // unit's when it is the unit's own program rather than something the unit's program started.
    std::vector<std::vector<std::string>> names, own, unit_texts;

    // Unit descriptions: given (tests) or asked of systemd once.
    bool units_given = false;
    std::map<std::string, std::string> unit_desc;  // unit or folded unit -> description
    std::set<std::string> asked;
    bool listed = false;
    // Desktop entries by the program they run.
    std::vector<std::string> desktop_dirs;
    bool desktop_read = false;
    std::map<std::string, std::vector<std::string>> desktop;     // program file name -> Name, GenericName, Comment
    std::map<std::string, std::vector<std::string>> desktop_id;  // entry id ("org.gnome.Terminal") -> the same
    // Description vectors, by text.
    std::string cache_path;
    bool cache_read = false, cache_dirty = false;
    std::unordered_map<std::string, std::vector<float>> vec;
    // Facts by name, looked up once: the table is scanned whole on every lookup.
    std::map<std::string, std::string> categories;
    std::map<std::string, std::vector<std::string>> fact_texts;
    // The analyzer's view of single words.
    std::set<std::string> known;
    std::map<std::string, std::vector<unllm::Intent>> topics;
    std::map<std::string, bool> machine;

    const std::vector<float>* embed(const std::string& text) {
        if (!model || !model->can_embed()) return nullptr;
        read_cache();
        auto it = vec.find(text);
        if (it == vec.end()) {
            auto v = model->embed(text, false);
            if (int(v.size()) != model->dim()) return nullptr;
            it = vec.emplace(text, std::move(v)).first;
            cache_dirty = true;
        }
        return &it->second;
    }

    void read_cache() {
        if (cache_read) return;
        cache_read = true;
        if (cache_path.empty() || !model) return;
        std::ifstream in(cache_path, std::ios::binary);
        char magic[8];
        int32_t dim = 0;
        in.read(magic, 8);
        in.read(reinterpret_cast<char*>(&dim), 4);
        if (!in || std::memcmp(magic, "ASKNAM1\0", 8) != 0 || dim != model->dim()) return;
        uint32_t len = 0;
        while (in.read(reinterpret_cast<char*>(&len), 4) && len < 4096) {
            std::string text(len, '\0');
            std::vector<float> v(static_cast<std::size_t>(dim));
            in.read(text.data(), len);
            in.read(reinterpret_cast<char*>(v.data()), std::streamsize(v.size() * sizeof(float)));
            if (!in) break;
            vec.emplace(std::move(text), std::move(v));
        }
    }

    void write_cache() {
        if (!cache_dirty || cache_path.empty() || !model) return;
        std::ofstream out(cache_path + ".tmp", std::ios::binary);
        if (!out) return;
        const int32_t dim = model->dim();
        out.write("ASKNAM1\0", 8);
        out.write(reinterpret_cast<const char*>(&dim), 4);
        for (const auto& [text, v] : vec) {
            const uint32_t len = uint32_t(text.size());
            out.write(reinterpret_cast<const char*>(&len), 4);
            out.write(text.data(), len);
            out.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size() * sizeof(float)));
        }
        out.close();
        if (std::rename((cache_path + ".tmp").c_str(), cache_path.c_str()) == 0) cache_dirty = false;
    }

    void read_desktop() {
        if (desktop_read) return;
        desktop_read = true;
        std::vector<std::string> dirs = desktop_dirs;
        if (dirs.empty()) {
            dirs.push_back("/usr/share/applications");
            const char* xdg = std::getenv("XDG_DATA_HOME");
            const char* home = std::getenv("HOME");
            if (xdg) dirs.push_back(std::string(xdg) + "/applications");
            else if (home) dirs.push_back(std::string(home) + "/.local/share/applications");
        }
        std::error_code ec;
        for (const auto& d : dirs)
            for (const auto& e : std::filesystem::directory_iterator(d, ec)) {
                if (e.path().extension() != ".desktop") continue;
                std::ifstream f(e.path());
                std::string line, name, generic, comment, exec;
                bool in_entry = false;
                while (std::getline(f, line)) {
                    if (!line.empty() && line[0] == '[') {
                        in_entry = line == "[Desktop Entry]";
                        continue;
                    }
                    if (!in_entry) continue;
                    if (line.rfind("Name=", 0) == 0) name = line.substr(5);
                    else if (line.rfind("GenericName=", 0) == 0) generic = line.substr(12);
                    else if (line.rfind("Comment=", 0) == 0) comment = line.substr(8);
                    else if (line.rfind("Exec=", 0) == 0) exec = line.substr(5);
                }
                // Only an entry that starts the program itself (field codes like %U aside) describes it; one that
                // passes it a module ("systemsettings kcm_kwinrules") describes that module.
                std::string prog;
                bool plain = true;
                std::size_t i = 0;
                while (i < exec.size()) {
                    while (i < exec.size() && exec[i] == ' ') ++i;
                    std::size_t s = i;
                    while (i < exec.size() && exec[i] != ' ') ++i;
                    std::string w = exec.substr(s, i - s);
                    if (w.empty()) break;
                    if (prog.empty()) prog = std::string(base_of(w));
                    else if (w[0] != '%') plain = false;
                }
                std::vector<std::string> texts;
                for (const auto& t : {name, generic, comment})
                    if (!trim(t).empty()) texts.push_back(trim(t));
                desktop_id[e.path().stem().string()] = texts;
                if (prog.empty() || !plain) continue;
                auto& by_prog = desktop[lower(prog)];
                by_prog.insert(by_prog.end(), texts.begin(), texts.end());
            }
    }

    // What systemd says the units are: one listing of each manager for the units loaded now (a terminal tab's
    // scope exists only while it runs). Nothing here needs privileges.
    void describe(const std::vector<std::string>& units) {
        if (units_given) return;
        if (!listed) {
            listed = true;
            for (const char* scope : {"systemctl --user", "systemctl"}) {
                std::string out = run_out(std::string(scope) + " list-units --all --plain --no-legend --no-pager 2>/dev/null");
                std::size_t at = 0;
                while (at < out.size()) {
                    std::size_t nl = out.find('\n', at);
                    std::string line = out.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                    at = nl == std::string::npos ? out.size() : nl + 1;
                    std::vector<std::string> f;
                    std::size_t i = 0;
                    while (f.size() < 4 && i < line.size()) {
                        while (i < line.size() && line[i] == ' ') ++i;
                        std::size_t s = i;
                        while (i < line.size() && line[i] != ' ') ++i;
                        if (i > s) f.push_back(line.substr(s, i - s));
                        if (f.size() == 1 && !f[0].empty() && static_cast<unsigned char>(f[0][0]) >= 0x80) f.clear();  // a status mark
                    }
                    if (f.size() < 4) continue;
                    std::string desc = trim(line.substr(std::min(i, line.size())));
                    if (desc.empty() || iequal(desc, f[0])) continue;
                    unit_desc.emplace(f[0], desc);
                    unit_desc.emplace(fold_unit(f[0]), desc);
                }
            }
        }
        // The rest from their unit files, as systemd lays them out; a template's instance ("foo@x.service") from
        // the template ("foo@.service"). Asking systemd itself loads every unit and takes seconds.
        std::vector<std::string> dirs;
        const char* home = std::getenv("HOME");
        const char* config = std::getenv("XDG_CONFIG_HOME");
        if (config) dirs.push_back(std::string(config) + "/systemd/user");
        else if (home) dirs.push_back(std::string(home) + "/.config/systemd/user");
        for (const char* d : {"/etc/systemd/user", "/usr/lib/systemd/user", "/etc/systemd/system", "/run/systemd/system",
                              "/usr/lib/systemd/system"})
            dirs.push_back(d);
        for (const auto& u : units) {
            if (u.empty() || unit_desc.count(u) || unit_desc.count(fold_unit(u)) || !asked.insert(u).second) continue;
            std::string file = u;
            if (auto at = u.find('@'); at != std::string::npos) file = u.substr(0, at + 1) + u.substr(u.rfind('.'));
            for (const auto& d : dirs) {
                std::ifstream f(d + "/" + file);
                if (!f) continue;
                std::string line;
                while (std::getline(f, line))
                    if (line.rfind("Description=", 0) == 0) {
                        std::string desc = trim(line.substr(12));
                        if (!desc.empty() && !iequal(desc, u)) unit_desc.emplace(u, desc);
                        break;
                    }
                break;
            }
        }
    }

    std::string unit_about(const std::string& raw_unit) const {
        if (auto it = unit_desc.find(raw_unit); it != unit_desc.end()) return without_numbers(it->second);
        if (auto it = unit_desc.find(fold_unit(raw_unit)); it != unit_desc.end()) return without_numbers(it->second);
        return "";
    }

    // The fact category of a program name ("runtime", "shell"), for telling an interpreter from what it runs.
    const std::string& category(const std::string& n) {
        auto it = categories.find(n);
        if (it == categories.end()) {
            const Fact* f = facts && !n.empty() ? facts->lookup(n) : nullptr;
            it = categories.emplace(n, f ? f->category : "").first;
        }
        return it->second;
    }

    // What an interpreter or a shell runs: python3's script, node's, the shell's script or command.
    std::string script_of(const std::string& name, const std::string& exe, const std::string& cmdline) {
        std::string_view prog, first;
        command_words(cmdline, &prog, &first);
        if (first.empty()) return "";
        auto runs_others = [&](const std::string& n) {
            const std::string c = category(n);
            return c == "runtime" || c == "shell";
        };
        if (!runs_others(name) && !runs_others(exe) && !runs_others(std::string(base_of(prog)))) return "";
        // A file: a path or a name with an extension, not a folder ("node --test test/") and not the first word of
        // a command a shell was handed ("sh -c set ...").
        if (first.back() == '/' || (first.find('/') == std::string_view::npos && first.find('.') == std::string_view::npos))
            return "";
        std::string s(base_of(first));
        return iequal(s, name) ? "" : s;
    }

    void add(std::map<std::tuple<std::string, std::string, std::string, std::string>, std::size_t>& index,
             const Program& p, bool top) {
        auto key = std::make_tuple(p.name, p.exe, p.script, p.unit);
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, candidates.size()).first;
            candidates.push_back(p);
            unit_texts.emplace_back();
            own.emplace_back();
            names.emplace_back();
        } else {
            Program& c = candidates[it->second];
            c.runs += p.runs;
            c.cpu += p.cpu;
            c.live |= p.live;
            if (c.about.empty()) c.about = p.about;
        }
        if (top && !p.about.empty() && unit_texts[it->second].empty()) unit_texts[it->second].push_back(p.about);
    }

    void describe_candidates() {
        read_desktop();
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const Program& p = candidates[i];
            std::vector<std::string>& t = own[i];
            std::set<std::string> seen;
            auto put = [&](const std::string& s) {
                if (!s.empty() && seen.insert(lower(s)).second) t.push_back(s);
            };
            // A kernel thread is described by its note alone: its names are many and say nothing.
            if (!(p.exe.empty() && p.script.empty() && p.unit.empty()))
                for (const std::string& n : {p.name, p.exe, p.script})
                    if (!n.empty() && seen.insert(lower(n)).second) names[i].push_back(n);
            // A unit that says which application started it ("launched by org.gnome.Terminal") is described by
            // that application's entry too.
            if (!unit_texts[i].empty()) {
                const std::string about = unit_texts[i][0];
                std::size_t at = 0;
                while (at < about.size()) {
                    std::size_t sp = about.find(' ', at);
                    std::string w = about.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
                    at = sp == std::string::npos ? about.size() : sp + 1;
                    if (w.find('.') == std::string::npos) continue;
                    if (auto d = desktop_id.find(w); d != desktop_id.end())
                        for (const auto& s : d->second)
                            if (std::find(unit_texts[i].begin(), unit_texts[i].end(), s) == unit_texts[i].end()) unit_texts[i].push_back(s);
                }
            }
            for (const std::string& n : {p.name, p.exe, p.script}) {
                if (n.empty()) continue;
                if (auto d = desktop.find(lower(n)); d != desktop.end())
                    for (const auto& s : d->second) put(s);
                auto ft = fact_texts.find(n);
                if (ft == fact_texts.end()) {
                    std::vector<std::string> texts;
                    if (facts)
                        for (const Fact* f : facts->lookup_all(n))
                            for (const auto& c : clauses(f->explanation)) texts.push_back(c);
                    ft = fact_texts.emplace(n, std::move(texts)).first;
                }
                for (const auto& c : ft->second) put(c);
            }
        }
    }

    void gather(int64_t from, int64_t to, const std::vector<ProcessInfo>& live) {
        candidates.clear();
        names.clear();
        own.clear();
        unit_texts.clear();
        std::map<std::tuple<std::string, std::string, std::string, std::string>, std::size_t> index;
        struct Row {
            std::string name, exe, cmdline, unit, parents;
            int64_t runs;
            double cpu;
        };
        std::vector<Row> rows;
        if (db) {
            const bool all = to <= 0;
            std::string sql = "SELECT name, IFNULL(exe, ''), IFNULL(cmdline, ''), IFNULL(unit, ''), "
                              "IFNULL(GROUP_CONCAT(DISTINCT parent), ''), COUNT(*), SUM(cpu_seconds) FROM procs ";
            if (!all) sql += "WHERE (started >= ?1 AND started < ?2) OR (ended >= ?1 AND started < ?2) OR (ended IS NULL AND started < ?2) ";
            sql += "GROUP BY name, exe, cmdline, unit";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                if (!all) {
                    sqlite3_bind_int64(st, 1, from);
                    sqlite3_bind_int64(st, 2, to);
                }
                auto col = [&](int i) {
                    const unsigned char* t = sqlite3_column_text(st, i);
                    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
                };
                while (sqlite3_step(st) == SQLITE_ROW)
                    rows.push_back({col(0), col(1), col(2), col(3), col(4), sqlite3_column_int64(st, 5), sqlite3_column_double(st, 6)});
                sqlite3_finalize(st);
            }
            // The per-minute lists: kernel threads and anything the process table missed.
            std::string msql = "SELECT name, '', IFNULL(cmdline, ''), IFNULL(unit, ''), '', COUNT(DISTINCT pid), SUM(cpu_seconds) "
                               "FROM minute_top ";
            if (!all) msql += "WHERE minute >= ?1 AND minute < ?2 ";
            msql += "GROUP BY name, cmdline, unit";
            if (sqlite3_prepare_v2(db, msql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                if (!all) {
                    sqlite3_bind_int64(st, 1, from);
                    sqlite3_bind_int64(st, 2, to);
                }
                std::set<std::string> in_procs;
                for (const auto& r : rows) in_procs.insert(r.name);
                auto col = [&](int i) {
                    const unsigned char* t = sqlite3_column_text(st, i);
                    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
                };
                while (sqlite3_step(st) == SQLITE_ROW)
                    if (!in_procs.count(col(0)))
                        rows.push_back({col(0), "", col(2), col(3), "", sqlite3_column_int64(st, 5), sqlite3_column_double(st, 6)});
                sqlite3_finalize(st);
            }
        }
        // Which names each unit holds, to tell a unit's own program from the ones it started.
        std::map<std::string, std::set<std::string>> in_unit;
        for (const auto& r : rows) in_unit[fold_unit(r.unit)].insert(r.name);
        std::map<int, const ProcessInfo*> by_pid;
        for (const auto& p : live) by_pid[p.key.pid] = &p;
        for (const auto& p : live) in_unit[fold_unit(p.unit)].insert(p.name);
        std::vector<std::string> units;
        for (const auto& r : rows) units.push_back(r.unit);
        for (const auto& p : live) units.push_back(p.unit);
        std::sort(units.begin(), units.end());
        units.erase(std::unique(units.begin(), units.end()), units.end());
        describe(units);

        auto program_of = [&](const std::string& name, const std::string& exe_path, const std::string& cmdline,
                              const std::string& unit) {
            std::string_view prog, first;
            command_words(cmdline, &prog, &first);
            Program p;
            p.name = name;
            p.exe = std::string(base_of(exe_path.empty() ? prog : std::string_view(exe_path)));
            p.script = script_of(name, p.exe, cmdline);
            p.unit = fold_unit(unit);
            p.about = unit.empty() ? "" : unit_about(unit);
            return p;
        };
        for (const auto& r : rows) {
            Program p = program_of(r.name, r.exe, r.cmdline, r.unit);
            p.runs = r.runs;
            p.cpu = r.cpu;
            // The unit's own program: every run started from outside the unit, by systemd for a service or by
            // another program for a scope (a terminal starting its tab's shell). In a scope, a parent called
            // systemd took in an orphan whose own parent had ended.
            bool top = true;
            const auto& names_here = in_unit[p.unit];
            const bool service = p.unit.ends_with(".service");
            std::size_t at = 0;
            while (at <= r.parents.size()) {
                std::size_t comma = r.parents.find(',', at);
                std::string parent = r.parents.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!parent.empty() && (parent == "systemd" ? !service : names_here.count(parent) > 0)) top = false;
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
            add(index, p, top);
        }
        for (const auto& lp : live) {
            if (lp.kernel_thread) continue;
            Program p = program_of(lp.name, lp.exe, lp.cmdline, lp.unit);
            p.live = true;
            auto parent = by_pid.find(lp.ppid);
            const bool top = parent == by_pid.end() || lp.ppid <= 1 || fold_unit(parent->second->unit) != p.unit;
            add(index, p, top);
        }
        describe_candidates();
    }

    // The analyzer's own words: stop words, its topic, time and generic words; a topic word is one it reads,
    // alone, as a topic.
    const std::vector<unllm::Intent>& topics_of(const std::string& w, int64_t now) {
        auto it = topics.find(w);
        if (it == topics.end()) it = topics.emplace(w, an ? an->read(w, now).intents : std::vector<unllm::Intent>{}).first;
        return it->second;
    }
    // A word the analyzer reads, alone, as the machine itself ("laptop", "computer").
    bool machine_word(const std::string& w, int64_t now) {
        auto it = machine.find(w);
        if (it == machine.end()) it = machine.emplace(w, an && an->read(w, now).self_named).first;
        return it->second;
    }

    // A question's words: which may be a program's name, and which runs of them may describe one.
    struct Parsed {
        std::vector<unllm::Token> tokens;
        std::vector<int> cls;                         // each word's class (kStop... below)
        std::vector<std::vector<std::size_t>> spans;  // runs of one to three words to match by meaning
        std::vector<std::size_t> exact;               // words to match as names
    };
    Parsed parse(const std::string& question, const unllm::Reading& reading, int64_t now);
    // Those words against the gathered candidates.
    std::vector<NameMatch> match(const Parsed& q);
};

namespace {
enum { kStop = 0, kUnknown = 1, kTopic = 2, kOther = 3 };
}

std::vector<NameMatch> Names::Impl::match(const Parsed& q) {
    const auto& tokens = q.tokens;
    const auto& cls = q.cls;
    std::vector<NameMatch> best(candidates.size());
    // Exact names first: a word the analyzer does not know, said as a program's name, its executable's or its
    // script's. A script is a file, and plain words make many file names ("new", "main.py", "server.js"): it counts
    // only when the word has a name's shape ("realm-companion", "fan-control.sh").
    for (std::size_t ti : q.exact) {
        Word w(tokens[ti].lower);
        const bool shaped = std::any_of(w.w.begin(), w.w.end(), [](char ch) { return !std::isalpha(static_cast<unsigned char>(ch)); });
        for (std::size_t c = 0; c < candidates.size(); ++c) {
            const Program& p = candidates[c];
            for (const std::string& n : {p.name, p.exe, shaped ? p.script : std::string()})
                if (best[c].score < 1.0f && w.names(n)) {
                    best[c] = {p, 1.0f, tokens[ti].lower, n, false};
                    break;
                }
        }
    }
    // Then meaning: each run of words against every description.
    if (model && model->can_embed()) {
        std::vector<std::vector<const std::vector<float>*>> nv(candidates.size()), tv(candidates.size()), uv(candidates.size());
        for (std::size_t c = 0; c < candidates.size(); ++c) {
            for (const auto& t : names[c]) nv[c].push_back(embed(t));
            for (const auto& t : own[c]) tv[c].push_back(embed(t));
            for (const auto& t : unit_texts[c]) uv[c].push_back(embed(t));
        }
        auto dot = [](const std::vector<float>& a, const std::vector<float>& b) {
            float s = 0;
            for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) s += a[k] * b[k];
            return s;
        };
        // The nearest text of a candidate to a vector: which it was, and whether it says what the unit is.
        struct Near {
            float score = -2;
            std::string via;
            bool by_unit = false;
        };
        auto nearest_in = [&](const std::vector<const std::vector<float>*>& vs, const std::vector<std::string>& texts,
                              const std::vector<float>& q, bool by_unit, Near& n) {
            for (std::size_t k = 0; k < vs.size(); ++k)
                if (vs[k] && dot(*vs[k], q) > n.score) n = {dot(*vs[k], q), texts[k], by_unit};
        };
        auto name_near = [&](std::size_t c, const std::vector<float>& q) {
            Near n;
            nearest_in(nv[c], names[c], q, false, n);
            return n;
        };
        auto description_near = [&](std::size_t c, const std::vector<float>& q) {
            Near n;
            nearest_in(tv[c], own[c], q, false, n);
            nearest_in(uv[c], unit_texts[c], q, true, n);
            return n;
        };
        for (const auto& span : q.spans) {
            std::string text;
            for (std::size_t i : span) text += (text.empty() ? "" : " ") + tokens[i].lower;
            const std::vector<float>* sv = embed(text);
            if (!sv) continue;
            std::vector<const std::vector<float>*> topic_words, words;
            bool has_unknown = false;
            for (std::size_t i : span) {
                has_unknown |= cls[i] == kUnknown;
                words.push_back(embed(tokens[i].lower));
                if (cls[i] == kTopic) topic_words.push_back(words.back());
            }
            for (std::size_t c = 0; c < candidates.size(); ++c) {
                // A single word is compared with the descriptions only: it names a program by its name only when it
                // is that name (above), or "steak" would come near "fish". Several words with one the analyzer does not
                // know may be a name taken apart ("fan control" for fan-control.sh), when the whole phrase is nearer
                // the name than any word of it alone. A topic word with a generic one ("torrent app") is compared with
                // descriptions only too, or "weird happen" would be the script called weird.
                Near n = description_near(c, *sv);
                if (span.size() > 1 && has_unknown) {
                    Near nn = name_near(c, *sv);
                    bool whole = true;
                    for (const auto* w : words)
                        if (w && name_near(c, *w).score >= nn.score) whole = false;
                    if (whole && nn.score > n.score) n = nn;
                }
                if (n.score < (span.size() > 1 ? kMeaning : kMeaningWord) || n.score <= best[c].score) continue;
                // A topic word with an unknown word must match better than the topic word alone: "fan control"
                // names fan-control.sh, "fan speed" only repeats the fan.
                if (has_unknown && !topic_words.empty()) {
                    float base = -2;
                    for (const auto* tw : topic_words)
                        if (tw) base = std::max({base, name_near(c, *tw).score, description_near(c, *tw).score});
                    if (n.score - base < kLift) continue;
                }
                best[c] = {candidates[c], n.score, text, n.via, n.by_unit};
            }
        }
        write_cache();
    }
    std::vector<NameMatch> out;
    for (auto& m : best)
        if (m.score > 0) out.push_back(std::move(m));
    std::stable_sort(out.begin(), out.end(), [](const NameMatch& a, const NameMatch& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.by_unit != b.by_unit) return !a.by_unit;
        if (a.program.live != b.program.live) return a.program.live;
        if (a.program.runs != b.program.runs) return a.program.runs > b.program.runs;
        return a.program.cpu > b.program.cpu;
    });
    return out;
}

Names::Names() : impl_(std::make_unique<Impl>()) {}
Names::~Names() = default;

void Names::open(sqlite3* db) { impl_->db = db; }
void Names::set_model(const unllm::Meaning* model) {
    impl_->model = model;
    impl_->cache_read = false;
}
void Names::set_facts(const Facts* facts) {
    impl_->facts = facts;
    impl_->categories.clear();
    impl_->fact_texts.clear();
}
void Names::set_analyzer(const unllm::Analyzer* an) {
    impl_->an = an;
    impl_->known.clear();
    impl_->topics.clear();
    impl_->machine.clear();
}
void Names::set_cache(const std::string& path) {
    impl_->cache_path = path;
    impl_->cache_read = false;
}
void Names::set_unit_descriptions(std::map<std::string, std::string> descriptions) {
    impl_->unit_desc.clear();
    for (auto& [unit, desc] : descriptions) impl_->unit_desc[unit] = std::move(desc);
    impl_->units_given = true;
}
void Names::set_desktop_dirs(std::vector<std::string> dirs) {
    impl_->desktop_dirs = std::move(dirs);
    impl_->desktop_read = false;
    impl_->desktop.clear();
}

void Names::gather(int64_t from, int64_t to, const std::vector<ProcessInfo>& live) { impl_->gather(from, to, live); }
const std::vector<Program>& Names::candidates() const { return impl_->candidates; }

Names::Impl::Parsed Names::Impl::parse(const std::string& question, const unllm::Reading& reading, int64_t now) {
    Parsed q;
    q.tokens = unllm::tokenize(question);
    const auto& tokens = q.tokens;
    std::set<std::size_t> time_words;
    unllm::find_time(tokens, now, &time_words);
    if (known.empty() && an)
        for (const auto& w : an->known_words(false)) known.insert(w);
    // Each word's class: a stop or time word, a number or a word of one or two letters ("as", "by": nobody names a
    // program that way in a question) splits the question, and so does a word for the machine itself ("did my laptop
    // throttle": the laptop is no program, and what follows it is what the machine did); the analyzer's own words
    // are topics or its other words; the rest it does not know.
    q.cls.assign(tokens.size(), kStop);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        if (time_words.count(i) || unllm::stop_word(t.lower) || unllm::stop_word(t.norm) || number_word(t.lower) ||
            t.lower.size() <= 2 || machine_word(t.lower, now))
            continue;
        if (!known.count(t.lower) && !known.count(t.norm)) q.cls[i] = kUnknown;
        else q.cls[i] = topics_of(t.lower, now).empty() ? kOther : kTopic;
    }
    // A topic word is the question's subject unless the question has another topic: "torrent" in "the torrent
    // app didn't crash" is not what is asked about.
    auto off_topic = [&](std::size_t i) {
        const auto& own_topics = topics_of(tokens[i].lower, now);
        for (auto in : reading.intents)
            if (std::find(own_topics.begin(), own_topics.end(), in) == own_topics.end()) return true;
        return false;
    };
    // Runs of words with nothing between them but spaces. A program is named where a run starts, after "the",
    // "that" or "my": its first one to three words are tried ("taskbar", "taskbar kept"), never a word inside it
    // ("as" in "running as root" is not the assembler).
    std::vector<std::size_t> run;
    auto close_run = [&] {
        if (!run.empty() && q.cls[run[0]] == kUnknown) q.exact.push_back(run[0]);
        for (std::size_t n = 1; n <= 3 && n <= run.size(); ++n) {
            std::vector<std::size_t> part(run.begin(), run.begin() + std::ptrdiff_t(n));
            bool unknown = false;
            for (std::size_t i : part) unknown |= q.cls[i] == kUnknown;
            // A topic word before one of the analyzer's other words ("torrent app") names a program when the
            // topic is not the question's.
            const bool topic_thing = n == 2 && q.cls[part[0]] == kTopic && q.cls[part[1]] == kOther && off_topic(part[0]);
            if (unknown || topic_thing) q.spans.push_back(part);
        }
        run.clear();
    };
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (q.cls[i] == kStop) {
            close_run();
            continue;
        }
        if (!run.empty()) {
            const auto& prev = tokens[run.back()];
            const std::size_t end = prev.start + prev.text.size();
            bool gap_clean = end <= tokens[i].start;
            for (std::size_t k = end; gap_clean && k < tokens[i].start && k < question.size(); ++k) gap_clean = question[k] == ' ';
            if (!gap_clean) close_run();
        }
        run.push_back(i);
    }
    close_run();
    return q;
}

std::vector<NameMatch> Names::find(const std::string& question, const unllm::Reading& reading, int64_t now,
                                   const std::function<std::vector<ProcessInfo>()>& live) {
    Impl& im = *impl_;
    const Impl::Parsed q = im.parse(question, reading, now);
    if (q.spans.empty() && q.exact.empty()) return {};
    // The reading's window first; then the whole records.
    std::vector<ProcessInfo> now_running;
    if (live) now_running = live();
    std::vector<NameMatch> out;
    if (reading.window.from > 0 && reading.window.to > reading.window.from) {
        im.gather(reading.window.from, reading.window.to, now_running);
        out = im.match(q);
    }
    if (out.empty()) {
        im.gather(0, 0, now_running);
        out = im.match(q);
    }
    return out;
}

std::vector<NameMatch> Names::resolve(const std::string& phrase, int64_t now) {
    const Impl::Parsed q = impl_->parse(phrase, unllm::Reading{}, now);
    return q.spans.empty() && q.exact.empty() ? std::vector<NameMatch>{} : impl_->match(q);
}

std::string Names::installed(const std::string& question, const unllm::Reading& reading, int64_t now) {
    const Impl::Parsed q = impl_->parse(question, reading, now);
    const char* path = std::getenv("PATH");
    if (!path) return "";
    std::vector<std::string> dirs;
    for (std::string_view p = path; !p.empty();) {
        const auto colon = p.find(':');
        if (colon != 0) dirs.emplace_back(p.substr(0, colon));
        p = colon == std::string_view::npos ? std::string_view() : p.substr(colon + 1);
    }
    for (const auto& span : q.spans) {
        if (span.size() < 2) continue;  // one word may be any command ("file", "watch"): a name taken apart is not
        for (const char* joint : {"-", "_", ""}) {
            std::string name;
            for (std::size_t i : span) name += (name.empty() ? "" : joint) + q.tokens[i].lower;
            for (const auto& d : dirs) {
                const std::string f = d + "/" + name;
                std::error_code ec;
                if (std::filesystem::is_regular_file(f, ec) && access(f.c_str(), X_OK) == 0) return f;
            }
        }
    }
    return "";
}

std::vector<NameMatch> Names::resolve_whole(const std::string& phrase, int64_t) {
    Impl::Parsed q;
    q.tokens = unllm::tokenize(phrase);
    q.cls.assign(q.tokens.size(), kOther);  // compared with descriptions only, at the usual cut-offs
    std::vector<std::size_t> all;
    for (std::size_t i = 0; i < q.tokens.size(); ++i) all.push_back(i);
    if (!all.empty()) q.spans.push_back(all);
    return q.spans.empty() ? std::vector<NameMatch>{} : impl_->match(q);
}

}  // namespace as
