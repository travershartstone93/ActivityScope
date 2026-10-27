#include "askcore.h"

#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <regex>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <mntent.h>
#include <strings.h>
#include <sstream>
#include "dirscan.h"
#include <atomic>
#include <thread>
#include <sys/statvfs.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>

#include "diagnose.h"
#include "events.h"
#include "judge.h"
#include "sampler.h"
#include "sysid.h"
#include "unllm/meaning.h"

namespace as {

using unllm::Clause;
using unllm::Degree;
using unllm::Form;
using unllm::Intent;
using unllm::NP;
using unllm::Tense;

namespace {

// The clock the current answer is given at (Ask::ask_once sets it): a moment more than six days from it needs its date.
int64_t when_now = 0;

// "Thu 14:35"; "Fri Oct 16 00:00" when the moment is more than six days from the answer's clock, or when dated.
std::string when_text(int64_t t, bool dated = false) {
    std::time_t tt = t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    if (dated || (when_now > 0 && std::llabs(when_now - t) > 6 * 86400)) {
        char day[16];
        std::strftime(day, sizeof day, "%a %b", &tm);
        std::snprintf(buf, sizeof buf, "%s %d %02d:%02d", day, tm.tm_mday, tm.tm_hour, tm.tm_min);
    } else {
        std::strftime(buf, sizeof buf, "%a %H:%M", &tm);
    }
    return buf;
}

// Two moments joined ("Thu 05:00 and Thu 12:00"); a span longer than six days dates both ends.
std::string span_text(int64_t from, int64_t to, const std::string& joint) {
    const bool long_span = to - from > 6 * 86400;
    return when_text(from, long_span) + joint + when_text(to, long_span);
}

std::string day_text(int64_t t) {
    std::time_t tt = t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%A %B %d", &tm);
    return buf;
}

// "Tuesday October 13 at 20:35": the shape a "when was the last..." answer needs.
std::string moment_text(int64_t t) {
    std::time_t tt = t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[48];
    std::strftime(buf, sizeof buf, "%A %B %d at %H:%M", &tm);
    return buf;
}

struct Row {
    int64_t t = 0;
    double a = 0, b = 0;
    std::string s1, s2;
};

bool answers_from_records(const std::string& kind);  // kinds answered by Ask::answer_records
bool from_logs(const std::string& kind);             // those of them read from the log and the boot records
// A package the question names, as the software log lists it ("cuda"); never a stop word.
std::string named_package(sqlite3* db, const unllm::Analyzer& an, const std::string& question);
// One software log row told by its packages and who started it ("Wed 20:40, installed a and b (user ran sudo ...)").
std::string software_change(sqlite3* db, int64_t t, const std::string& title, const std::string& detail, bool* started);
std::string run_out(const std::string& cmd);         // a command's output
std::vector<std::string> oom_kills(sqlite3* db, int64_t from, int64_t to, std::size_t keep, std::size_t* total);
std::string failed_unit(const std::string& title, const std::string& detail);  // the unit a services row names
std::string unit_name(const std::string& unit);
struct Space {  // a mounted filesystem and its size
    std::string mount;
    double total = 0, free = 0;
};
std::vector<Space> real_filesystems();          // the real mounted filesystems, with their sizes
std::string space_line(const Space& s);          // "/ has 117 GB free of 512 GB (77% used)"                                 // a unit as a person reads it
double swap_size(sqlite3* db, int64_t from, int64_t to, double live);           // the swap's size over a window, 0: unknown
std::string health_now(const Snapshot& s, double low_battery);                  // the live figures, what is acute first
double low_battery_level(const std::string& etc);                               // the level the system calls low
std::string right_before(sqlite3* db, int64_t t);                               // what the machine did right before t
std::string stop_story(sqlite3* db, int64_t t);                                 // the minutes before a stop at t
std::string last_run_text(sqlite3* db, const std::string& program, int64_t now);  // " It last ran ...": a program's latest run
std::vector<Row> series(sqlite3* db, const std::string& column, const std::string& hourly, int64_t from, int64_t to);  // per-second readings, hourly beyond them

std::vector<Row> query(sqlite3* db, const std::string& sql,
                       const std::vector<int64_t>& binds,
                       const std::vector<std::string>& text_binds = {}) {
    std::vector<Row> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    int n = 1;
    for (int64_t v : binds) sqlite3_bind_int64(st, n++, v);
    for (const auto& v : text_binds) sqlite3_bind_text(st, n++, v.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Row r;
        r.t = sqlite3_column_int64(st, 0);
        if (sqlite3_column_count(st) > 1) r.a = sqlite3_column_double(st, 1);
        if (sqlite3_column_count(st) > 2) r.b = sqlite3_column_double(st, 2);
        if (sqlite3_column_count(st) > 3 && sqlite3_column_text(st, 3))
            r.s1 = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
        if (sqlite3_column_count(st) > 4 && sqlite3_column_text(st, 4))
            r.s2 = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return out;
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

// "1,260"
std::string thousands(double v) {
    std::string s = std::to_string(std::llround(v));
    for (int i = int(s.size()) - 3; i > (s[0] == '-' ? 1 : 0); i -= 3) s.insert(std::size_t(i), ",");
    return s;
}

// A program as an answer names it: one started through /proc/self/exe is called "exe", so its command line says
// which it is ("exe (unllm --serve)").
std::string shown_name(const std::string& name, const std::string& cmdline) {
    if (name != "exe" || cmdline.empty()) return name;
    std::size_t end = cmdline.find(' ');
    if (end != std::string::npos) end = cmdline.find(' ', end + 1);
    const std::string head = cmdline.substr(0, end);
    const std::size_t slash = head.rfind('/', head.find(' '));
    return name + " (" + (slash == std::string::npos ? head : head.substr(slash + 1)) + ")";
}

// The events of a kind that touch [?1, ?2) (kinds mask ?3), each with its part inside the window: the share of its
// readings (event_samples) taken inside, or of its time when it has none. An event left open across the night's
// sleep and closed by the first reading after waking (Wed 21:15 to Thu 06:45) is all but outside Thursday morning.
const char* kEventParts =
    "WITH p AS (SELECT e.id, CASE WHEN e.start >= ?1 AND e.\"end\" <= ?2 THEN 1.0 "
    "ELSE IFNULL((SELECT 1.0 * SUM(s.t >= ?1 AND s.t < ?2) / COUNT(*) FROM event_samples s WHERE s.event_id = e.id), "
    "MAX(0, MIN(e.\"end\", ?2) - MAX(e.start, ?1)) / MAX(1.0, e.\"end\" - e.start)) END AS part "
    "FROM events e WHERE e.start < ?2 AND e.\"end\" >= ?1 AND (e.kinds & ?3) != 0) ";

// The culprits of a kind's events in [from, to), best first by the kind's own measure: bytes read and written for
// the disk (4), video memory for the GPU (8), memory held for memory (2), CPU time for heat and slowdowns. Each event
// counts for its part inside the window, and the 'unseen' rows (a parent's finished children, which have rows of
// their own) are not its work. A program given keeps only its rows, and those of the units it heads when given (`units`:
// ollama.service with the llama-server runners ollama starts). Row: a = the measure, b = CPU seconds, s1 = the name,
// s2 = a command line.
std::vector<Row> ranked_culprits(sqlite3* db, int64_t from, int64_t to, unsigned mask, const std::string& program, int limit,
                                 const std::vector<std::string>& units = {}) {
    const std::string cpu = "SUM(c.share * IFNULL(e.total_cpu_seconds, 1) * p.part)";
    const std::string measure = mask == 4   ? "SUM(IFNULL(c.io_bytes, 0) * p.part)"
                                : mask == 8 ? "MAX(IFNULL(c.gpu_mem, 0))"
                                : mask == 2 ? "MAX(IFNULL(c.peak_rss, 0))"
                                            : cpu;
    std::string sql = std::string(kEventParts) + "SELECT 0, " + measure + ", " + cpu + ", c.name, MAX(c.cmdline) "
                      "FROM p JOIN events e ON e.id = p.id JOIN event_culprits c ON c.event_id = e.id "
                      "WHERE p.part > 0 AND IFNULL(c.kind, '') <> 'unseen' ";
    std::vector<std::string> tb;
    if (!program.empty()) {
        tb.push_back(program);
        std::string in;
        for (const auto& u : units) {
            tb.push_back(u);
            in += (in.empty() ? "?" : ", ?") + std::to_string(3 + tb.size());
        }
        sql += "AND (is_program(?4, c.name, '', c.cmdline)" + (in.empty() ? std::string() : " OR c.unit IN (" + in + ")") + ") ";
    }
    sql += "GROUP BY c.name ORDER BY 2 DESC, 3 DESC LIMIT " + std::to_string(limit);
    return query(db, sql, {from, to, int64_t(mask)}, tb);
}

// The top name among the culprits of the matching events.
std::string top_culprit(sqlite3* db, int64_t from, int64_t to, unsigned mask,
                        const std::string& program) {
    auto rows = ranked_culprits(db, from, to, mask, program, 1);
    return rows.empty() ? "" : shown_name(rows[0].s1, rows[0].s2);
}

// CPU seconds from the per-minute lists over [from, to): the rows `where` picks (text binds from ?4), summed per
// `group`, best first. A process's finished children are booked to it in the minute they end, and in the records
// made before 2026-10-23 their whole lives are in their own rows too (systemd --user got 11,958 s in the minute a
// terminal tab was killed, when the whole machine used 110 s), so each row is held to what the machine used in its
// minute: the readings' CPU over all cores, or the hour's peak beyond the day the readings are kept. A parent's minute
// that holds its finished children's whole lives (the children, with all they started in turn, ended in it and the
// minute is at least that big, as the recorder wrote them before 2026-10-23 11:22) counts at the parent's usual minute
// in the window instead (its minutes that hold none), at most what is left after them: the shell and the build tool
// that waited on a build are not the build. Row: a = seconds, b = processes, s1 = the group, s2 = a command line.
std::vector<Row> minute_cpu(sqlite3* db, int64_t from, int64_t to, const std::string& where,
                            const std::vector<std::string>& tb, const std::string& group, int limit) {
    auto last = query(db, "SELECT 0, 0, 0, cores FROM samples WHERE IFNULL(cores, '') <> '' ORDER BY t DESC LIMIT 1", {});
    const int64_t cores = last.empty() ? 0 : 1 + std::count(last[0].s1.begin(), last[0].s1.end(), ',');
    const std::string sql =
        "WITH RECURSIVE d(par, m, pid, s, e, cpu, depth) AS (SELECT p.ppid, CAST(p.ended / 60 AS INTEGER) * 60, p.pid, p.started, p.ended, p.cpu_seconds, 0 "
        "FROM procs p WHERE p.ended >= ?1 AND p.ended < ?2 AND EXISTS (SELECT 1 FROM minute_top x WHERE x.minute = CAST(p.ended / 60 AS INTEGER) * 60 "
        "AND x.pid = p.ppid) UNION ALL SELECT d.par, d.m, p.pid, p.started, p.ended, p.cpu_seconds, d.depth + 1 FROM d JOIN procs p "
        "ON p.ppid = d.pid AND p.started >= d.s AND p.started <= d.e WHERE d.depth < 32), "
        "kids AS (SELECT par, m, SUM(cpu) AS cpu FROM d GROUP BY par, m), "
        "kept AS (SELECT t.*, IFNULL(k.cpu, 0) AS kids FROM minute_top t LEFT JOIN kids k ON k.par = t.pid AND k.m = t.minute "
        "WHERE t.minute >= ?1 AND t.minute < ?2), "
        "usual AS (SELECT pid, start_ticks, AVG(cpu_seconds) AS cpu FROM kept WHERE kids = 0 GROUP BY pid, start_ticks), "
        "own AS (SELECT kept.*, CASE WHEN kept.kids > 0 AND kept.cpu_seconds >= kept.kids THEN MIN(kept.cpu_seconds - kept.kids, "
        "IFNULL(usual.cpu, 0)) ELSE kept.cpu_seconds END AS cpu FROM kept LEFT JOIN usual ON usual.pid = kept.pid AND usual.start_ticks = kept.start_ticks) "
        "SELECT 0, SUM(MIN(mt.cpu, COALESCE(CASE WHEN ?3 > 0 THEN (SELECT AVG(s.cpu) * 0.6 * ?3 FROM samples s WHERE s.t >= mt.minute "
        "AND s.t < mt.minute + 60) END, CASE WHEN ?3 > 0 THEN h.max * 0.6 * ?3 END, mt.cpu))), "
        "COUNT(DISTINCT CASE WHEN mt.cpu >= 0.5 THEN mt.pid END), " + group + ", MAX(mt.cmdline) FROM own mt "
        "LEFT JOIN samples_hourly h ON h.metric = 'cpu' AND h.hour_ts = mt.minute - mt.minute % 3600 "
        "WHERE mt.minute >= ?1 AND mt.minute < ?2 AND (" + where + ") GROUP BY " + group +
        " ORDER BY 2 DESC LIMIT " + std::to_string(limit);
    return query(db, sql, {from, to, cores}, tb);
}

// The units a program heads in [from, to): every other process in them was started inside them (ollama.service holds
// ollama and the llama-server runners it starts). A terminal tab's scope is headed by its shell, not by a program
// run in it.
std::vector<std::string> headed_units(sqlite3* db, const std::string& program, int64_t from, int64_t to, int64_t now) {
    std::vector<std::string> out;
    for (const auto& u : query(db, "SELECT 0, 0, 0, unit FROM procs WHERE started < ?1 AND IFNULL(ended, ?2) >= ?3 AND IFNULL(unit, '') <> '' "
                                   "AND is_program(?4, name, exe, cmdline) GROUP BY unit",
                               {to, now, from}, {program})) {
        auto rows = query(db, "SELECT pid, ppid, is_program(?4, name, exe, cmdline) FROM procs "
                              "WHERE started < ?1 AND IFNULL(ended, ?2) >= ?3 AND unit = ?5",
                          {to, now, from}, {program, u.s1});
        std::set<int64_t> pids;
        for (const auto& x : rows) pids.insert(x.t);
        bool headed = rows.size() > 1;
        for (const auto& x : rows) headed = headed && (x.b > 0 || pids.count(int64_t(x.a)) > 0);
        if (headed) out.push_back(u.s1);
    }
    return out;
}

// The per-minute lists' first minute (they are kept about a day); `to` when there are none.
int64_t lists_start(sqlite3* db, int64_t to) {
    auto first = query(db, "SELECT CAST(MIN(minute) AS INTEGER) FROM minute_top", {});
    return first.empty() || first[0].t == 0 ? to : first[0].t;
}

// CPU seconds over [from, to) from the processes' runs, for the time the per-minute lists don't reach: each run's CPU
// shared out by the time the machine was awake inside and outside the window (the hourly records' readings, and the
// clock after the last of them; with no hourly records nothing is shared). A run cut off by a crash or a power loss
// (its end estimated, its CPU as last seen) is estimated from what the spike records charged it, less the lives of its
// children that ended in each spike (the recorder booked those to their parent too); a row estimated so while its
// twin (the same process, its start read a few seconds apart) ran on is no run of its own. The runs `where` picks
// (text binds from ?4), summed per `group`. Row: a = seconds, b = processes, s1 = the group, s2 = a command line,
// t = the part of a estimated from the spikes for runs cut off.
std::vector<Row> runs_cpu(sqlite3* db, int64_t from, int64_t to, int64_t now, const std::string& where,
                          const std::vector<std::string>& tb, const std::string& group) {
    auto rolled = query(db, "SELECT CAST(MAX(hour_ts) AS INTEGER) + 3600 FROM samples_hourly WHERE metric = 'cpu'", {});
    if (rolled.empty() || rolled[0].t <= 3600) return {};
    const std::string last = std::to_string(rolled[0].t);
    const std::string sql =
        "WITH h AS (SELECT hour_ts, n FROM samples_hourly WHERE metric = 'cpu'), "
        "r AS (SELECT pid, name, cmdline, started AS s, IFNULL(ended, ?3) AS e, cpu_seconds AS c, IFNULL(end_estimated, 0) AS est "
        "FROM procs WHERE started < ?2 AND IFNULL(ended, ?3) > ?1 AND (cpu_seconds > 0 OR end_estimated = 1) AND (" + where + ") "
        "AND NOT (IFNULL(end_estimated, 0) = 1 AND EXISTS "
        "(SELECT 1 FROM procs t WHERE t.pid = procs.pid AND t.rowid <> procs.rowid AND ABS(t.started - procs.started) <= 5))), "
        "w AS (SELECT r.*, (SELECT IFNULL(SUM(h.n * (MIN(r.e, h.hour_ts + 3600) - MAX(r.s, h.hour_ts)) / 3600.0), 0) FROM h "
        "WHERE h.hour_ts > r.s - 3600 AND h.hour_ts < r.e) + MAX(0, r.e - MAX(r.s, " + last + ")) AS life, "
        "(SELECT IFNULL(SUM(h.n * (MIN(r.e, ?2, h.hour_ts + 3600) - MAX(r.s, ?1, h.hour_ts)) / 3600.0), 0) FROM h "
        "WHERE h.hour_ts > MAX(r.s, ?1) - 3600 AND h.hour_ts < MIN(r.e, ?2)) AS inside, "
        "CASE WHEN r.est = 1 THEN (SELECT IFNULL(SUM(MAX(0, c.cpu_seconds - (SELECT IFNULL(SUM(k.cpu_seconds), 0) FROM procs k "
        "WHERE k.ended >= v.start - 2 AND k.ended <= v.\"end\" + 2 AND k.ppid = r.pid AND k.started >= r.s))), 0) "
        "FROM event_culprits c JOIN events v ON v.id = c.event_id "
        "WHERE c.pid = r.pid AND v.start >= r.s - 1 AND v.start < r.e AND IFNULL(c.kind, '') <> 'unseen') ELSE 0 END AS charged FROM r) "
        "SELECT CAST(SUM((MAX(c, charged) - c) * inside / life) AS INTEGER), SUM(MAX(c, charged) * inside / life), "
        "COUNT(*), " + group + ", MAX(cmdline) FROM w WHERE inside > 0 AND life > 0 GROUP BY " + group + " ORDER BY 2 DESC";
    return query(db, sql, {from, to, now}, tb);
}

// A window's per-minute rows joined, per group, with the runs before the lists begin (runs_cpu): best first.
std::vector<Row> with_runs(sqlite3* db, std::vector<Row> rows, int64_t from, int64_t to, int64_t now, const std::string& where,
                           const std::vector<std::string>& tb, const std::string& group, std::size_t limit) {
    const int64_t detail = lists_start(db, to);
    if (from >= detail) return rows;
    for (const auto& x : runs_cpu(db, from, std::min(to, detail), now, where, tb, group)) {
        auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& y) { return y.s1 == x.s1; });
        if (it == rows.end()) {
            rows.push_back(x);
        } else {
            it->a += x.a;
            it->b += x.b;
            it->t += x.t;
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.a > b.a; });
    if (rows.size() > limit) rows.resize(limit);
    return rows;
}

// A program's CPU seconds in [from, to), with the units it heads: per name, best first. The per-minute lists where they
// reach, its runs before them ("since the weekend", "on monday").
std::vector<Row> program_cpu(sqlite3* db, const std::string& program, int64_t from, int64_t to, int64_t now) {
    std::vector<std::string> tb = {program};
    std::string where = "is_program(?4, mt.name, '', mt.cmdline)", runs = "is_program(?4, name, exe, cmdline)";
    for (const auto& u : headed_units(db, program, from, to, now)) {
        tb.push_back(u);
        where += " OR mt.unit = ?" + std::to_string(3 + tb.size());
        runs += " OR unit = ?" + std::to_string(3 + tb.size());
    }
    return with_runs(db, minute_cpu(db, from, to, where, tb, "mt.name", 8), from, to, now, runs, tb, "name", 8);
}

// A reading over spans of time (the per-second samples): how many, the average, the peak and when.
struct Stats {
    int64_t n = 0;
    double avg = 0, peak = 0;
    int64_t peak_t = 0;
};
Stats sample_stats(sqlite3* db, const std::string& column, const std::vector<std::pair<int64_t, int64_t>>& spans) {
    Stats s;
    double sum = 0;
    for (const auto& [a, b] : spans) {
        auto rows = query(db, "SELECT COUNT(*), SUM(" + column + "), 0 FROM samples WHERE t >= ?1 AND t < ?2 AND " + column + " >= 0", {a, b});
        if (rows.empty() || rows[0].t == 0) continue;
        s.n += rows[0].t;
        sum += rows[0].a;
        auto top = query(db, "SELECT CAST(t AS INTEGER), " + column + " FROM samples WHERE t >= ?1 AND t < ?2 AND " + column +
                                 " >= 0 ORDER BY " + column + " DESC, t LIMIT 1", {a, b});
        if (!top.empty() && (s.peak_t == 0 || top[0].a > s.peak)) {
            s.peak = top[0].a;
            s.peak_t = top[0].t;
        }
    }
    if (s.n > 0) s.avg = sum / double(s.n);
    return s;
}

// "09:21:55"
std::string clock_text(int64_t t) {
    std::time_t tt = t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    return buf;
}

// "was firefox open when the fan hit its peak": the moments a reading was at its peak in [from, to) (runs of
// readings at that value; the per-second samples, or the events' own readings beyond them), and whether the
// program was running at each. The reading is the samples column a word of the question names ("fan"), else the
// topic's own (`column`). The yes or no goes to `lead`.
std::string peak_answer(sqlite3* db, const std::vector<std::string>& words, std::string column, std::string noun,
                        const std::string& program, int64_t from, int64_t to, int64_t now, std::vector<std::string>* receipts,
                        std::string* lead) {
    for (const auto& c : query(db, "SELECT 0, 0, 0, name FROM pragma_table_info('samples') WHERE type = 'REAL' AND name <> 't'", {}))
        for (const auto& w : words)
            if (w == c.s1) column = w, noun = "the " + w;
    if (column.empty()) return "";
    auto rows = query(db, "SELECT CAST(t AS INTEGER), " + column + " FROM samples WHERE t >= ?1 AND t < ?2 AND " + column +
                              " IS NOT NULL ORDER BY t", {from, to});
    if (rows.empty())
        rows = query(db, "SELECT CAST(s.t AS INTEGER), s." + column + " FROM event_samples s JOIN events e ON e.id = s.event_id "
                         "WHERE s.t >= ?1 AND s.t < ?2 AND s." + column + " IS NOT NULL ORDER BY s.t", {from, to});
    if (rows.empty()) return "";
    double peak = rows[0].a;
    for (const auto& x : rows) peak = std::max(peak, x.a);
    // Readings at the peak, a minute or less apart, are one stretch of it.
    std::vector<std::pair<int64_t, int64_t>> runs;
    for (const auto& x : rows)
        if (x.a >= peak) {
            if (!runs.empty() && x.t - runs.back().second <= 60) runs.back().second = x.t;
            else runs.push_back({x.t, x.t});
        }
    std::vector<std::string> moments, running;
    for (const auto& [a, b] : runs) {
        const std::string m = a == b ? clock_text(a) : clock_text(a) + " to " + clock_text(b);
        moments.push_back(m);
        auto there = query(db, "SELECT COUNT(*) FROM procs WHERE started <= ?1 AND IFNULL(ended, ?3) >= ?2 AND is_program(?4, name, exe, cmdline)",
                           {b, a, now}, {program});
        if (!there.empty() && there[0].t > 0) running.push_back(m);
        if (receipts && receipts->size() < 6) receipts->push_back(when_text(a) + "  " + noun + " at " + fmt("%.0f", peak));
    }
    const std::string unit = column == "temp" ? " C" : "%";
    if (moments.size() > 4) moments.resize(4);
    std::string text = noun + " peaked at " + fmt("%.0f", peak) + unit + " at " + unllm::join_list(moments) + " in that window";
    text[0] = char(std::toupper(static_cast<unsigned char>(text[0])));
    if (running.empty()) {
        *lead = "No.";
        text += ", and " + program + " wasn't running at " + (runs.size() == 1 ? "that moment." : "any of those moments.");
        // When it did run: its processes' lifetimes, joined where they overlap or touch.
        std::vector<std::pair<int64_t, int64_t>> ran;
        for (const auto& x : query(db, "SELECT CAST(started AS INTEGER), IFNULL(ended, ?3) FROM procs WHERE started < ?1 AND IFNULL(ended, ?3) >= ?2 "
                                       "AND is_program(?4, name, exe, cmdline) ORDER BY started", {to, from, now}, {program})) {
            if (!ran.empty() && x.t <= ran.back().second + 60) ran.back().second = std::max(ran.back().second, int64_t(x.a));
            else ran.push_back({x.t, int64_t(x.a)});
        }
        std::vector<std::string> spans;
        for (const auto& [a, b] : ran)
            if (spans.size() < 4) spans.push_back(clock_text(a).substr(0, 5) + (b - a >= 60 ? " to " + clock_text(b).substr(0, 5) : ""));
        if (ran.size() > spans.size()) spans.push_back(unllm::number_word(long(ran.size() - spans.size())) + " more");
        text += spans.empty() ? " It didn't run in that window at all." : " It ran at " + unllm::join_list(spans) + ".";
    } else if (running.size() == runs.size()) {
        *lead = "Yes.";
        text += ", and " + program + " was running at " + (runs.size() == 1 ? "that moment." : "all of them.");
    } else {
        *lead = "Yes, at " + unllm::number_word(long(running.size())) + " of them.";
        text += ", and " + program + " was running at " + unllm::join_list(running) + ".";
    }
    return text;
}

const char* intent_example(Intent i) {
    switch (i) {
    case Intent::Heat: return "why did the fan scream last night";
    case Intent::Slow: return "what was using the cpu this morning";
    case Intent::Memory: return "which program used the most memory this week";
    case Intent::Disk: return "what was hammering the disk yesterday";
    case Intent::Network: return "who used the network today";
    case Intent::Sharing: return "is anything torrenting";
    case Intent::Update: return "what got installed yesterday";
    case Intent::Config: return "did my settings change this week";
    case Intent::Crash: return "what crashed today";
    case Intent::Trust: return "is this machine safe";
    case Intent::Battery: return "what drained the battery yesterday";
    case Intent::Startup: return "what starts at boot";
    case Intent::Devices: return "what usb devices did i plug in today";
    case Intent::Flash: return "when did i flash the esp32";
    case Intent::Diagnose: return "what is wrong with this machine";
    case Intent::Identity: return "what is this machine";
    default: return "what happened last night";
    }
}

// "two days and three hours", "five hours", "41 minutes"
std::string duration_text(int64_t s) {
    using unllm::number_word;
    using unllm::plural_noun;
    if (s < 60) return "under a minute";
    int64_t days = s / 86400, hours = (s % 86400) / 3600, mins = (s % 3600) / 60;
    if (days > 0) {
        std::string out = number_word(days) + " " + plural_noun("day", days);
        if (hours > 0) out += " and " + number_word(hours) + " " + plural_noun("hour", hours);
        return out;
    }
    if (hours > 0) {
        std::string out = number_word(hours) + " " + plural_noun("hour", hours);
        if (mins > 0) out += " and " + number_word(mins) + " " + plural_noun("minute", mins);
        return out;
    }
    return number_word(mins) + " " + plural_noun("minute", mins);
}

int local_hour(int64_t t) {
    std::time_t tt = t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    return tm.tm_hour;
}

std::string local_date(int64_t t) {
    std::time_t tt = t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

std::string ordinal(int n) {
    int tens = n % 100;
    const char* suf = (tens >= 11 && tens <= 13) ? "th"
                      : n % 10 == 1              ? "st"
                      : n % 10 == 2              ? "nd"
                      : n % 10 == 3              ? "rd"
                                                 : "th";
    return std::to_string(n) + suf;
}

// "Thu 23:33 to 23:37", "Wed 21:35 to Thu 07:23": a span with its day said once; one moment when it is under a minute.
std::string run_span(int64_t a, int64_t b) {
    if (b - a < 60) return when_text(a);
    return when_text(a) + " to " + (local_date(a) == local_date(b) ? clock_text(b).substr(0, 5) : when_text(b));
}

// The stretches of readings at or over a level (rows in time order: t, value), readings a minute or less apart being
// one stretch: from its first reading to its last, with its peak.
struct Spell {
    int64_t from = 0, to = 0;
    double peak = 0;
};
std::vector<Spell> spells(const std::vector<Row>& rows, double level) {
    std::vector<Spell> out;
    for (const auto& x : rows) {
        if (x.a < level) continue;
        if (!out.empty() && x.t - out.back().to <= 60) {
            out.back().to = x.t;
            out.back().peak = std::max(out.back().peak, x.a);
        } else {
            out.push_back({x.t, x.t, x.a});
        }
    }
    return out;
}

// "how has the cpu temperature been", "what was the gpu doing": a reading over [from, to) told from the per-second
// samples: where it ranged, with the peak's moment, its typical level (the median), and the stretches at or over
// `level`, the recorder's own threshold for that reading (below 0: none), the three highest in time order. `value`
// writes a reading ("50 C", "12%"). The part of the window before the samples begin is told from the hourly records
// (the metric named like the column; the hours that end by the first reading): the highest hour and the typical level. ""
// when the samples don't reach the window.
std::string readings_text(sqlite3* db, const std::string& column, const std::string& noun, const std::function<std::string(double)>& value,
                          double level, int64_t from, int64_t to) {
    auto rows = query(db, "SELECT CAST(t AS INTEGER), " + column + " FROM samples WHERE t >= ?1 AND t < ?2 AND " + column + " >= 0 ORDER BY t",
                      {from, to});
    if (rows.empty()) return "";
    std::string before;
    if (rows.front().t > from + 900) {
        auto hours = query(db, "SELECT hour_ts, max, med FROM samples_hourly WHERE metric = ?3 AND hour_ts >= ?1 AND hour_ts + 3600 <= ?2 ORDER BY hour_ts",
                           {from - 3599, rows.front().t}, {column});
        std::vector<double> meds;
        const Row* top = nullptr;
        for (const auto& h : hours) {
            meds.push_back(h.b);
            if (!top || h.a > top->a) top = &h;
        }
        if (top)
            before = " Before " + when_text(rows.front().t) + ", in the hourly records, it reached " + value(top->a) + " (in the hour from " +
                     when_text(top->t) + "), typically " + value(judge::median(meds)) + ".";
    }
    std::vector<double> v;
    const Row* hi = &rows[0];
    for (const auto& x : rows) {
        v.push_back(x.a);
        if (x.a > hi->a) hi = &x;
    }
    std::string text = noun + " between " + span_text(rows.front().t > from + 900 ? rows.front().t : from, to, " and ") + " ranged from " +
                       value(*std::min_element(v.begin(), v.end())) + " to " + value(hi->a) + " (the highest at " + when_text(hi->t) + "), typically " +
                       value(judge::median(v));
    if (level >= 0) {
        const auto all = spells(rows, level);
        if (all.empty()) return text + "; it never reached " + value(level) + ", the recorder's threshold for it." + before;
        // The time at or over it: each such reading counts for the time to the next, a minute at most.
        int64_t secs = 0;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].a >= level) secs += i + 1 < rows.size() ? std::min<int64_t>(rows[i + 1].t - rows[i].t, 60) : 1;
        std::vector<Spell> top = all;
        std::stable_sort(top.begin(), top.end(), [](const Spell& a, const Spell& b) { return a.peak > b.peak; });
        if (top.size() > 3) top.resize(3);
        std::sort(top.begin(), top.end(), [](const Spell& a, const Spell& b) { return a.from < b.from; });
        std::vector<std::string> said;
        for (const auto& x : top) said.push_back(run_span(x.from, x.to) + " (up to " + value(x.peak) + ")");
        if (all.size() > top.size()) said.push_back(unllm::number_word(long(all.size() - top.size())) + " more");
        text += "; it was at " + value(level) + " or more for " + duration_text(secs) + " in all, in " +
                (all.size() == 1 ? std::string("one spell: ") : unllm::number_word(long(all.size())) + " spells: ") + unllm::join_list(said);
    }
    return text + "." + before;
}

// "how long was the cpu above 80% today": the time a reading spent at or over `level` in [from, to) by the per-second
// samples, each reading counting for the time to the next (a minute at most), with its stretches (readings a minute or
// less apart joined) and the longest of them; before the samples begin, the hours of the hourly records (metric
// `hourly`, the hours that end by the first reading) whose highest reading reached it. "" when neither reaches the window.
std::string above_text(sqlite3* db, const std::string& column, const std::string& hourly, const std::string& noun,
                       const std::function<std::string(double)>& value, double level, int64_t from, int64_t to) {
    auto rows = query(db, "SELECT CAST(t AS INTEGER), " + column + ", MIN(IFNULL(LEAD(t) OVER (ORDER BY t) - t, 1), 60) FROM samples "
                          "WHERE t >= ?1 AND t < ?2 AND " + column + " >= 0 ORDER BY t", {from, to});
    const int64_t detail = rows.empty() ? to : rows.front().t;
    auto hours = detail > from + 900 ? query(db, "SELECT hour_ts, max FROM samples_hourly WHERE metric = ?3 AND hour_ts >= ?1 AND hour_ts + 3600 <= ?2 ORDER BY hour_ts",
                                             {from - 3599, detail}, {hourly})
                                     : std::vector<Row>{};
    if (rows.empty() && hours.empty()) return "";
    const std::string at = value(level) + " or more";
    std::string text;
    if (!rows.empty()) {
        std::vector<Row> over;
        double secs = 0, peak = -1;
        for (const auto& x : rows)
            if (x.a >= level) {
                over.push_back(x);
                secs += x.b;
                peak = std::max(peak, x.a);
            }
        const std::string where = "between " + span_text(detail, to, " and ") + (detail > from + 900 ? ", where the per-second readings reach" : "");
        const auto all = spells(over, level);
        if (all.empty()) {
            text = noun + " never reached " + value(level) + " " + where + ".";
        } else {
            const Spell* longest = &all[0];
            for (const auto& x : all)
                if (x.to - x.from > longest->to - longest->from) longest = &x;
            text = noun + " was at " + at + " for " + duration_text(int64_t(secs)) + " in all " + where + " (" + thousands(double(over.size())) +
                   " readings, one a second), in " +
                   (all.size() == 1 ? "one spell: " + run_span(all[0].from, all[0].to)
                                    : unllm::number_word(long(all.size())) + " spells from " + when_text(all.front().from) + " to " +
                                          when_text(all.back().to) + "; the longest " + run_span(longest->from, longest->to)) +
                   ". It peaked at " + value(peak) + ".";
        }
    }
    if (!hours.empty()) {
        const Row* top = nullptr;
        long reached = 0;
        for (const auto& x : hours)
            if (x.a >= level) {
                ++reached;
                if (!top || x.a > top->a) top = &x;
            }
        const std::string it = rows.empty() ? noun : "it";
        text += std::string(text.empty() ? "" : " ") + (rows.empty() ? "In the hourly records, " : "Before " + when_text(detail) + ", in the hourly records, ") +
                (reached == 0 ? it + " stayed under " + value(level) + " in all " + unllm::number_word(long(hours.size())) + " hours on record"
                              : it + " reached " + at + " in " + unllm::number_word(reached) + " of the " + unllm::number_word(long(hours.size())) +
                                    " hours on record, up to " + value(top->a) + " in the hour from " + when_text(top->t) +
                                    " (those records keep each hour's highest reading, not how long it lasted)") +
                ".";
    }
    return text;
}

// Checked on the question's own tokens: "os" is too short to be a search
// term but is exactly the word that picks the sub-answer.
bool has_word(const std::vector<unllm::Token>& tokens, std::initializer_list<const char*> keys) {
    for (const auto& t : tokens)
        for (const char* k : keys)
            if (t.lower == k || t.norm == k) return true;
    return false;
}

// "90 C" or "80 %" from the question, so "did it hit 90 degrees" compares
// the peak against 90 instead of just counting events.
double quantity_threshold(const unllm::Reading& r, const char* unit) {
    for (const auto& e : r.entities) {
        if (e.type != "quantity") continue;
        auto sp = e.value.find(' ');
        if (sp == std::string::npos) continue;
        if (e.value.substr(sp + 1) == unit) return std::strtod(e.value.c_str(), nullptr);
    }
    return -1;
}

// A yes-or-no answer's opening when the records can't settle it.
const std::string kNoYesNo = "I can't say yes or no to that from the records. What they show:";

// A yes or no the handler's own records settle, as the lead. A question that sets the window against another time or
// the usual ("more than usual", "than last month": the reading's judge) isn't settled by one window's records: no lead,
// and the answer says the records can't settle it.
void settle(AskResult& res, const unllm::Reading& r, bool yes) {
    if (r.judge == unllm::Judge::None) res.lead = yes ? "Yes." : "No.";
    else res.undecided = true;
}

// The yes-or-no tests the heat readings settle, each as a question it answers (Ask::asked_test): the recorder's hot level,
// the fan at the level it counts as loud, the shutdown point near (measured with bge-small on 21 heat questions: "did my
// laptop overheat last night", "was it hot yesterday" 0; "did the fan scream last night" 1; "did it nearly shut down
// from heat" 2).
const std::vector<std::vector<const char*>> kHeatTests = {{"did it overheat"}, {"did the fan get loud"},
                                                          {"did it nearly reach the temperature where it shuts down"}};

// realize_polar's yes or no in its parts: the lead by settle, and the clauses as their own sentences. A body that
// opens with "No " ("No crashes were recorded") is already the no, as realize_polar has it.
void polar_answer(AskResult& res, const unllm::Reading& r, bool yes, const std::vector<Clause>& clauses) {
    res.body = unllm::realize(clauses);
    if (yes || res.body.compare(0, 3, "No ") != 0) settle(res, r, yes);
}

// The sleep the session log puts a moment in, said in a sentence; "" when the machine was awake (with the session log's
// other helpers, below).
std::string asleep_at(sqlite3* db, int64_t at, int64_t now);

}  // namespace

std::string compose_answer(const AskResult& r) {
    std::string out;
    auto add = [&](const std::string& part) {
        if (!part.empty()) out += (out.empty() ? "" : " ") + part;
    };
    for (const auto& s : r.scope) add(s);
    add(r.lead);
    if (r.undecided) add(kNoYesNo);
    add(r.body);
    for (const auto& n : r.notes) add(n);
    return out;
}

Ask::Ask() {
    names_.set_facts(&facts_);
    names_.set_analyzer(&an_);
}

void Ask::set_roots(const Roots& roots) {
    sampler_ = std::make_unique<Sampler>(roots);
    roots_ = roots;
}

Ask::~Ask() {
    if (db_) sqlite3_close(db_);
}

bool Ask::open(const std::string& db_path) {
    if (sqlite3_open_v2(db_path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    // The gazetteer: every program the machine has ever seen run, plus every
    // name the resource events blamed (kernel threads like kswapd0 are never
    // in the program history, but they are culprits).
    std::vector<std::string> programs;
    for (const auto& r : query(db_, "SELECT 0,0,0,name FROM procs GROUP BY name ORDER BY COUNT(*) DESC LIMIT 400", {}))
        programs.push_back(r.s1);
    for (const auto& r : query(db_, "SELECT 0,0,0,name FROM event_culprits WHERE name NOT IN "
                                    "(SELECT name FROM procs) GROUP BY name ORDER BY COUNT(*) DESC LIMIT 200", {}))
        if (!r.s1.empty() && r.s1.find(' ') == std::string::npos) programs.push_back(r.s1);
    an_.set_programs(programs);
    program_words_.clear();
    for (std::string p : programs) {
        for (char& c : p) c = char(std::tolower(static_cast<unsigned char>(c)));
        program_words_.insert(p);
    }
    // Program filters compare the executable's name and the script on the command line too.
    add_program_function(db_);
    names_.open(db_);
    // The retrieval index: event summaries and log lines.
    for (const auto& r : query(db_, "SELECT start,0,0,summary FROM events", {}))
        index_.add(0, r.t, "event", r.s1);
    for (const auto& r : query(db_, "SELECT t,0,0,category,title FROM log", {}))
        index_.add(0, r.t, r.s1, r.s2);
    return true;
}

void Ask::load_language(const std::string& synonyms_path, const std::string& vectors_path,
                        const std::string& banter_path, const std::string& rewrites_path) {
    auto syn = unllm::load_synonyms(synonyms_path);
    if (!syn.empty()) an_.set_synonyms(std::move(syn));
    if (vectors_.load(vectors_path)) an_.set_vectors(&vectors_);
    if (!banter_path.empty()) banter_.load(banter_path);
    if (!rewrites_path.empty()) rewrites_.load(rewrites_path);
}

void Ask::log_miss(const std::string& question, const unllm::Reading& r) {
    if (miss_log_.empty()) return;
    std::ofstream f(miss_log_, std::ios::app);
    if (!f) return;
    std::time_t tt = std::time(nullptr);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    std::string intents;
    for (auto i : r.intents) intents += (intents.empty() ? "" : ",") + std::string(unllm::intent_name(i));
    if (intents.empty()) intents = "-";
    const char* form = r.form == Form::Polar ? "polar" : r.form == Form::Count ? "count" : "open";
    std::string q = question;
    for (char& c : q)
        if (c == '\n' || c == '\t') c = ' ';
    f << ts << "\t" << fmt("%.2f", r.domain) << "\t" << intents << "\t" << form << "\t" << q << "\n";
}

void Ask::load_conversation(const std::string& followups_path, const std::string& chatspeak_path) {
    if (followups_.load(followups_path)) dialogue_.set_followups(&followups_);
    speller_.load_chatspeak(chatspeak_path);
    // Typos are corrected toward the analyzer's own words, never toward
    // program names (those have their own fuzzy match), and a program on
    // record is no typo ("systemd crashes" is not "system crashes").
    speller_.set_known(an_.known_words(false));
    speller_.set_protected(&program_words_);
    speller_ready_ = true;
}

AskResult Ask::ask_once(const std::string& question, int64_t now, const std::string& force_kind, Mode mode) {
    AskResult res;
    remember_ = fresh_ = false;
    when_now = now;
    question_ = question;
    if (mode == Mode::Fresh) dialogue_.reset();  // a new question: nothing earlier is in force
    if (!db_) {
        res.body = "I can't answer that: the history database is not open.";
        res.refused = true;
        return res;
    }
    // Banter sees the raw words: "tell me a joke" keeps its exact shape.
    // Analysis sees the question with its chat-speak expanded, typos fixed
    // and chatty coat removed.
    auto raw_tokens = unllm::tokenize(question);
    std::vector<std::pair<std::string, std::string>> spelling;
    const std::string fixed = speller_ready_ ? speller_.fix(question, &spelling) : question;
    const std::string q = rewrites_.size() ? rewrites_.apply(fixed) : fixed;
    const unllm::Turn turn = dialogue_.interpret(q, now, an_);
    // The catalogue matcher names the topic from the question's nearest example
    // phrasing; it leads the reading's intents. "none" leaves the reading as the
    // word lists made it, so the usual clarify and refusal paths still apply.
    unllm::Reading reading = turn.reading;
    // A follow-up the person marked as one, asked as a full question: the last answer is in force. "It", "that" and
    // "this" are what it named (whatever the topic; the analyzer leaves a pronoun unbound in a question about the
    // machine or about trust); "after that", "since then" and "before that" are spans from or to its moment; a question
    // naming no time keeps its time.
    if (mode == Mode::FollowUp && turn.kind == unllm::Turn::Fresh && dialogue_.last() && !dialogue_.last()->refused) {
        const unllm::Card& prev = *dialogue_.last();
        bool has_program = false;
        for (const auto& e : reading.entities) has_program |= e.type == "program";
        bool pronoun = false, anchored = false;
        for (std::size_t i = 0; i < raw_tokens.size(); ++i) {
            const std::string& w = raw_tokens[i].lower;
            pronoun |= w == "it" || w == "that" || w == "this";
            const std::string next = i + 1 < raw_tokens.size() ? raw_tokens[i + 1].lower : "";
            const bool at_then = next == "that" || next == "then" || next == "it" || next == "this";
            if ((w == "after" || w == "since" || w == "before") && at_then && prev.moment > 0) {
                reading.window.from = w == "before" ? (prev.window.stated && prev.window.from < prev.moment ? prev.window.from : prev.moment - 86400) : prev.moment;
                reading.window.to = w == "before" ? prev.moment : now;
                reading.window.stated = true;
                reading.window.live = reading.window.present = false;
                reading.event_clause.clear();
                reading.event_after = reading.event_at = reading.event_during = false;
                anchored = true;
            }
        }
        if (!has_program && pronoun) {
            std::string subject = prev.subjects_named.empty() ? prev.subject : prev.subjects_named[0];
            auto known = subject.empty() ? std::vector<Row>{} : query(db_, "SELECT COUNT(*) FROM procs WHERE name = ?1", {}, {subject});
            if (!known.empty() && known[0].t > 0) reading.entities.push_back({"program", subject});
        }
        if (!anchored && !reading.window.stated && !reading.window.live && !reading.window.present && prev.window.stated) reading.window = prev.window;
    }
    // Never a span after now ("after that" with nothing to anchor it): the last day instead, said in the trace.
    bool future_fixed = false;
    if (!reading.window.live && !reading.window.present && reading.window.from >= now) {
        reading.window.from = now - 86400;
        reading.window.to = now;
        reading.window.stated = false;
        future_fixed = true;
    }
    const QuestionKind* unanswered = nullptr;  // the nearest kind is one ask has no records for
    std::string space_kind;                    // a free-disk-space question, answered by answer_space
    float kind_score = 1;                      // how near the question is to that kind's nearest phrasing
    bool doubted = false;                      // the read-back gate below doubts the reading
    bool unplaced = false;                     // doubted, far from every topic, and the analyzer read no topic: unanswered
    std::vector<std::string> readings;
    if (kinds_.ready()) {
        // The speller's fixes are guesses toward the analyzer's words ("health" became "healthy"):
        // the question as typed is matched too, and the closer of the two wins.
        as::KindMatch km = kinds_.match(q);
        if (!force_kind.empty() && kinds_.find(force_kind)) {
            // The check chose another of the closest readings: answer as that kind, a close match.
            km.id = force_kind;
            km.score = std::max(km.score, 0.90f);
        } else if (fixed != question) {
            as::KindMatch typed = kinds_.match(rewrites_.size() ? rewrites_.apply(question) : question);
            if (typed.score > km.score) km = typed;
        }
        // Two kinds of one topic about as near as each other, one a cause and one not: the question's own form decides
        // ("fan noise last night" asks no why: the fan's level, not the heat's culprits; "why was the fan loud" keeps them).
        if (const QuestionKind* top = force_kind.empty() ? kinds_.find(km.id) : nullptr) {
            const bool why = reading.wants == unllm::Wants::Cause;
            for (std::size_t i = 0; i < km.runners.size() && km.score - km.runners[i].second <= kinds_.cutoff("band", 0.02f); ++i)
                if (const QuestionKind* k = kinds_.find(km.runners[i].first);
                    k && k->topic == top->topic && (top->form == "a cause") != why && (k->form == "a cause") == why) {
                    km.id = k->id;
                    km.score = km.runners[i].second;
                    if (i < km.runner_phrasings.size()) km.phrasing = km.runner_phrasings[i];
                    break;
                }
        }
        // A time named that has ended ("battery percent this morning" asked in the afternoon) is not now: a kind about
        // now gives way to its sibling about a window, the catalogue's same id with "window" for "now".
        if (force_kind.empty() && reading.window.stated && !reading.window.live && reading.window.to < now)
            if (const QuestionKind* k = kinds_.find(km.id); k && k->window == "now")
                if (const auto at = km.id.find(".now"); at != std::string::npos)
                    for (std::size_t i = 0; i < km.runners.size(); ++i)
                        if (km.runners[i].first == km.id.substr(0, at) + ".window" + km.id.substr(at + 4)) {
                            km.id = km.runners[i].first;
                            km.score = km.runners[i].second;
                            if (i < km.runner_phrasings.size()) km.phrasing = km.runner_phrasings[i];
                            break;
                        }
        // A yes-or-no question with no why in it ("was firefox open when the fan hit its peak") is not asking for a
        // cause: a "cause" kind nearest by wording gives way to the nearest kind of another form within a few hundredths.
        if (force_kind.empty() && reading.form == Form::Polar && reading.wants != unllm::Wants::Cause)
            if (const QuestionKind* k = kinds_.find(km.id); k && k->form == "a cause")
                for (std::size_t i = 0; i < km.runners.size(); ++i) {
                    const QuestionKind* o = kinds_.find(km.runners[i].first);
                    if (!o || o->form == "a cause" || km.runners[i].second < km.score - 0.05f) continue;
                    km.id = o->id;
                    km.score = km.runners[i].second;
                    if (i < km.runner_phrasings.size()) km.phrasing = km.runner_phrasings[i];
                    break;
                }
        // A file path in the question ("is ~/Downloads/x.AppImage safe") is about this machine, never "outside the records".
        const bool names_path = q.find(" /") != std::string::npos || q.find("~/") != std::string::npos || q.rfind("/", 0) == 0;
        if (const QuestionKind* k = kinds_.find(km.id); k && k->data == "missing" && !k->reply.empty() &&
                                                         !(names_path && k->id == "none"))
            unanswered = k;
        if (names_path) reading.domain = std::max(reading.domain, 0.8);  // a path on this machine: understood
        kind_ = km.id;
        if (force_kind.empty()) match_ = km;
        readings.push_back(km.id);
        for (const auto& [id, score] : km.runners)
            if (id != km.id && readings.size() < 5) readings.push_back(id);
        if (answers_from_records(km.id)) space_kind = km.id;
        // "gpu temp while gaming sunday": the GPU's temperature is read live only; asked of a past window, the window's GPU
        // records answer, saying that first.
        if (km.id == "gpu.value.now.temp" && reading.window.stated && !reading.window.live && !reading.window.present) space_kind = km.id;
        kind_score = km.score;
        // "what is kswapd0" is answered from the fact table; a topic in front of it would bury that.
        bool what_is = false;
        for (const auto& e : reading.entities)
            what_is |= e.type == "name" || (e.type == "program" && reading.frame == unllm::Frame::Define);
        if (what_is) space_kind.clear();
        static const std::map<std::string, Intent> by_name = [] {
            std::map<std::string, Intent> m;
            for (int i = 0; i <= int(Intent::Story); ++i) m[unllm::intent_name(Intent(i))] = Intent(i);
            m["updates"] = Intent::Update;
            m["crashes"] = Intent::Crash;
            return m;
        }();
        // The read-back gate: the chosen topic as a whole is far from the question (its three nearest phrasings average
        // under 0.83) and the analyzer's own reading, before the topic is put first, names another topic or none. On 195
        // new questions (2026-10-24) that marked 17 of the 26 answers judged wrong and 2 of the 88 judged right, and on
        // other questions more right ones, so the answer stays and says how the question was taken. Not for a forced
        // kind, a "what is", or "none" (answered only for a path).
        if (const QuestionKind* k = kinds_.find(km.id); k && force_kind.empty() && k->topic != "none" && !what_is) {
            const auto it = by_name.find(k->topic);
            const auto& own = turn.reading.intents;
            doubted = km.topic_score(k->topic) < kinds_.cutoff("doubt", 0.83f) &&
                      (it == by_name.end() || std::find(own.begin(), own.end(), it->second) == own.end());
            // Nothing fits: no phrasing near (under 0.80, the understanding line), the topic below the score where right
            // answers were rare (0.795 on the same questions, 1 of 88), and no topic read by the analyzer either. Then a
            // near miss is not given as the answer.
            unplaced = doubted && own.empty() && km.score < kinds_.cutoff("understand", 0.80f) && km.topic_score(k->topic) < kinds_.cutoff("unplaced_topic", 0.795f);
        }
        if (const QuestionKind* k = kinds_.find(km.id); k && k->topic != "none" && !what_is) {
            if (auto it = by_name.find(k->topic); it != by_name.end()) {
                reading.intents.erase(std::remove(reading.intents.begin(), reading.intents.end(), it->second), reading.intents.end());
                reading.intents.insert(reading.intents.begin(), it->second);
                // A close match is understanding; a loose one leads the intents but leaves the
                // clarify gate to decide ("how do i cook a steak" must still ask).
                if (km.score >= kinds_.cutoff("understand", 0.80f)) reading.domain = std::max(reading.domain, double(km.score));
                // A close match on a kind about a past window ("show me the memory spike during the game")
                // outranks a present tense the grammar only assumed; one said outright ("is firefox using a lot of
                // memory", "does it ...") stays about now, as do words that say now ("right now", "currently").
                if (km.score >= kinds_.cutoff("understand", 0.80f) && k->window == "window" && !reading.window.live &&
                    !reading.window.present_marked)
                    reading.window.present = false;
                // The kind's answer form: a close "cause" kind is a why question, whatever the wording.
                if (km.score >= kinds_.cutoff("understand", 0.80f) && k->form == "a cause") reading.wants = unllm::Wants::Cause;
                // A close match on another topic outranks the words that read as "the machine itself"
                // ("what happened to my laptop at lunch" is not asking what the laptop is).
                if (km.score >= kinds_.cutoff("understand", 0.80f) && it->second != Intent::Identity)
                    reading.intents.erase(std::remove(reading.intents.begin(), reading.intents.end(), Intent::Identity), reading.intents.end());
            }
        }
    }
    // "While i was asleep": no record holds the person's sleep, but the records hold what the person started. The longest
    // stretch of the night in which nothing of theirs started, three hours or more, is taken as the sleep, and said.
    sleep_note_.clear();
    if (reading.window.sleep && db_) {
        std::vector<int64_t> starts{reading.window.from};
        for (const auto& x : query(db_, "SELECT CAST(started AS INTEGER), 0 FROM procs WHERE started >= ?1 AND started < ?2 AND "
                                        "(unit LIKE 'app-%' OR unit LIKE 'vte-spawn-%') ORDER BY started", {reading.window.from, reading.window.to}))
            starts.push_back(x.t);
        starts.push_back(reading.window.to);
        std::size_t best = 0;
        for (std::size_t i = 1; i + 1 < starts.size(); ++i)
            if (starts[i + 1] - starts[i] > starts[best + 1] - starts[best]) best = i;
        if (starts.size() > 2 && starts[best + 1] - starts[best] >= 3 * 3600) {
            reading.window.from = starts[best] / 60 * 60;
            reading.window.to = starts[best + 1] / 60 * 60;
            sleep_note_ = "Taking your sleep as " + span_text(reading.window.from, reading.window.to, " to ") + ", when nothing of yours started.";
        }
    }
    // Everyday words for a program ("the movie player", "that fan control thing"): when the reading names none,
    // the program the words describe in the records joins the reading under the records' name, so the program
    // filters apply.
    named_.clear();
    const bool fresh = turn.kind == unllm::Turn::Fresh || (turn.kind == unllm::Turn::TopicReturn && !turn.card);
    if (fresh && reading.choices.empty()) {
        bool named = false;
        for (const auto& e : reading.entities) named |= e.type == "program" || e.type == "name";
        if (!named) {
            const bool about_now = reading.window.live || reading.window.present;
            named_ = names_.find(q, reading, now, about_now ? [this] {
                if (!sampler_) sampler_ = std::make_unique<Sampler>();
                return sampler_->sample()->procs;
            } : std::function<std::vector<ProcessInfo>()>());
            // Words that name a unit ("the terminal": a terminal tab) name no one program in it.
            if (!named_.empty() && !named_[0].by_unit) reading.entities.push_back({"program", named_[0].program.name});
        }
    }
    // A loose match on a kind read from the logs is understanding only when the question names something the records
    // hold: a program, or a package the software log lists ("when did i put cuda on here" matches at 0.758).
    if (!space_kind.empty() && from_logs(space_kind) && kind_score < kinds_.cutoff("understand", 0.80f) && reading.domain < 0.6) {
        bool named = !named_.empty();
        for (const auto& e : reading.entities) named |= e.type == "program";
        if (!named && named_package(db_, an_, q).empty()) space_kind.clear();
    }
    // "while i watched the movie": the program the clause is about, through the same names (its thing, then the whole
    // clause); the handlers take that program's runs as the window. The words tried are kept for the answer.
    condition_tried_.clear();
    if (fresh && reading.frame == unllm::Frame::Condition && reading.condition.empty() && !reading.condition_thing.empty()) {
        names_.gather(reading.window.from, reading.window.to);
        for (const std::string& words : {reading.condition_thing, reading.condition_clause}) {
            auto m = words.empty() ? std::vector<NameMatch>{} : names_.resolve_whole(words, now);
            if (!m.empty() && !m[0].by_unit) {
                reading.condition = m[0].program.name;
                break;
            }
            if (!words.empty() && (condition_tried_.empty() || condition_tried_.back() != words)) condition_tried_.push_back(words);
        }
        if (!reading.condition.empty()) condition_tried_.clear();
    }
    // A clause that names a state the session log keeps ("while lid closed") is checked against its rows instead.
    std::string state = fresh ? state_lead(reading, now) : "";
    if (!state.empty()) condition_tried_.clear();
    event_at_ = 0;
    event_what_.clear();
    event_unmatched_ = false;
    event_nearest_.clear();
    if (fresh) place_event(reading, now);
    card_ = unllm::Card{};
    card_.question = q;
    card_.at = now;
    receipt_offset_ = 0;
    bool remember = true;
    // The answer's tags: how the question was read (its topic, its time, the nearest kinds), whichever answer it got.
    auto tag = [&](AskResult& out) {
        out.topic = reading.intents.empty() ? "" : unllm::intent_name(reading.intents[0]);
        out.when = reading.window.live || reading.window.present ? "now" : "window";
        out.span = reading.window.live || reading.window.present ? "now"
                   : reading.window.from > 0 ? span_text(reading.window.from, reading.window.to, " to ")
                                             : "not stated";
        if (reading.window.shifted) out.span += " (moved back a day)";
        out.readings = readings;
        if (!match_.id.empty()) out.nearest = match_.phrasing + fmt(" (%.3f)", match_.score);
    };

    // "what cpu do i have again" with no earlier talk of it is simply a question.
    switch (turn.kind == unllm::Turn::TopicReturn && !turn.card ? unllm::Turn::Fresh : turn.kind) {
    case unllm::Turn::NoCard: {
        const char* lead = turn.act == unllm::Act::Why ? "Why what?"
                           : turn.act == unllm::Act::When ? "When what?"
                                                          : "About what?";
        res.body = std::string(lead) + " Ask me about the machine first, for example: \"" +
                   intent_example(Intent::Heat) + "\".";
        res.branch = "followup:nocard";
        return res;
    }
    case unllm::Turn::FollowUp:
        if (turn.act == unllm::Act::Thanks) {
            res.body = "You're welcome.";
            res.branch = "followup:thanks";
            return res;
        }
        if (turn.act == unllm::Act::Wrong) {
            res.body = "Sorry about that. Tell me what you meant, for example: \"no, I meant firefox\" or "
                       "\"no, I meant last night\".";
            res.branch = "followup:wrong";
            return res;
        }
        card_ = *turn.card;  // the follow-up keeps pointing at the same thing
        card_.at = now;
        res = follow_up(turn.act, *turn.card, now);
        res.branch = std::string("followup:") + unllm::act_name(turn.act);
        break;
    case unllm::Turn::ClarifyNo:
        res.body = "OK. What did you want to know?";
        res.branch = "clarify:no";
        card_.refused = true;
        break;
    case unllm::Turn::TopicReturn:
        if (!turn.card) {
            res.body = turn.note;
            res.branch = "topic-return:none";
            return res;
        }
        res = answer_reading(reading, unllm::tokenize(turn.card->question), raw_tokens, question, now);
        res.scope.insert(res.scope.begin(), trim(turn.note));
        break;
    case unllm::Turn::Ambiguous:
        // Ask which one; the reply ("firefox") answers this question.
        res.body = turn.note;
        res.branch = "ambiguous";
        card_.reading = reading;
        card_.clarify = true;
        card_.refused = true;
        card_.offered_names = reading.ambiguous;
        break;
    case unllm::Turn::ClarifyYes:
    case unllm::Turn::Correction:
        res = answer_reading(reading, unllm::tokenize(turn.card->question), raw_tokens, question, now);
        break;
    case unllm::Turn::Fresh: {
        fresh_ = true;
        // Questions aimed at the tool itself ("are you connected to the
        // internet") go to banter first; everything else must miss the answer
        // gate before banter is even considered, so real questions always win.
        bool second_person = false;
        for (const auto& t : raw_tokens) second_person |= t.lower == "you" || t.lower == "your" || t.lower == "u";
        if (second_person) {
            std::string b = banter_.match(raw_tokens);
            if (!b.empty()) {
                res.body = b;
                res.branch = "banter";
                return res;
            }
        }
        // A program the facts know but that has never run here ("did chrome eat all my ram"): the
        // records hold nothing about it, which is the answer. Words the analyzer knows are never names. A question
        // the records can't hold at all ("is chrome tracking which websites i visit") says that first, then this.
        std::string never_ran;
        {
            // A fuzzy match to another program ("chrome" read as chroma) doesn't count as naming one;
            // "what is X" has its own answer.
            bool named = reading.frame == unllm::Frame::Define;
            const auto qt = unllm::tokenize(q);
            for (const auto& e : reading.entities)
                for (const auto& t : qt) named |= e.type == "program" && t.lower == e.value;
            static std::set<std::string> words;
            if (words.empty())
                for (const auto& w : an_.known_words(false)) words.insert(w);
            for (const auto& t : qt) {
                if (named || t.lower.size() < 3 || words.count(t.lower)) continue;
                const Fact* f = facts_.lookup(t.lower);
                if (!f || f->pattern.size() != t.lower.size() || strcasecmp(f->pattern.c_str(), t.lower.c_str()) != 0) continue;
                auto seen = query(db_, "SELECT (SELECT COUNT(*) FROM procs WHERE is_program(?1, name, exe, cmdline)) + "
                                       "(SELECT COUNT(*) FROM minute_top WHERE is_program(?1, name, '', cmdline)), "
                                       "0, 0", {}, {t.lower});
                if (seen.empty() || seen[0].t > 0) continue;
                auto first = query(db_, "SELECT CAST(MIN(started) AS INTEGER) FROM procs", {});
                const std::string said = t.lower + " (" + f->explanation + ") hasn't run on this machine as far as the records go" +
                                         (first.empty() || first[0].t == 0 ? std::string(".") : ", back to " + moment_text(first[0].t) + ".");
                if (unanswered) {
                    never_ran = " " + said;
                    break;
                }
                res.body = said;
                // Asked whether to trust it: a copy that never ran is a file, which weird can check by its path.
                if (std::find(reading.intents.begin(), reading.intents.end(), Intent::Trust) != reading.intents.end())
                    res.body += " If you have a copy you downloaded, give its path (like \"is ~/Downloads/tool.AppImage safe\") and "
                                "I'll check where it came from with weird.";
                res.branch = "never-ran:" + t.lower;
                tag(res);
                return res;
            }
        }
        // "was it the ram or the swap": two memory measures side by side, whatever kind the whole question matched.
        if (memory_choice(reading, &res)) {
            reading.intents.erase(std::remove(reading.intents.begin(), reading.intents.end(), Intent::Memory), reading.intents.end());
            reading.intents.insert(reading.intents.begin(), Intent::Memory);
            break;
        }
        if (!space_kind.empty()) {
            res = answer_records(space_kind, reading, now);
            break;
        }
        if (unanswered) {
            // Say what is not recorded and what is, rather than answer a
            // different question with a real-looking figure.
            res.body = unanswered->reply + never_ran;
            res.branch = "unanswered:" + unanswered->id;
            res.refused = true;
            card_.refused = true;
            break;
        }
        // "what was running when the machine shut off": a question its clause placed at an event's moment is told
        // from the records of that moment; a stop without a clean shutdown by its own story.
        if (reading.event_at && event_at_) {
            const bool stop = !query(db_, "SELECT 1 FROM events WHERE (kinds & ?) != 0 AND start BETWEEN ? AND ? LIMIT 1",
                                     {int64_t(KindShutdown), event_at_ - 1, event_at_ + 1}).empty();
            const std::string told = stop ? stop_story(db_, event_at_) : right_before(db_, event_at_);
            if (!told.empty()) {
                res.body = stop ? "The machine stopped without a clean shutdown on " + moment_text(event_at_) + ". " + told : told;
                res.branch = stop ? "event:stop" : "event:moment";
                card_.moment = event_at_;
                break;
            }
        }
        res = answer_reading(reading, unllm::tokenize(q), raw_tokens, question, now);
        break;
    }
    }
    // Each sentence below on how the question was taken is said before those already there.
    if (!state.empty() && fresh_ && !res.refused) res.scope.insert(res.scope.begin(), state);
    // "did it overheat while gaming": a clause that named no program is said once, before the whole window's answer
    // from the events (the readings' own whole-window answer, mask:...:while, says it itself).
    if (fresh && !condition_tried_.empty() && reading.condition.empty() && !res.refused && res.branch.rfind("mask:", 0) == 0 &&
        res.branch.find(":while") == std::string::npos)
        res.scope.insert(res.scope.begin(), "I couldn't tell which program \"" + reading.condition_thing + "\" was: no program that ran "
                                            "then is described that way, so this is for the whole window.");
    // "was there a spike at noon yesterday": a moment the machine slept through is said first, and settles the yes or no
    // the records of the time around it could not (an undecided answer with nothing said before it); what follows is
    // that time.
    if (fresh && reading.window.at > 0 && !reading.window.live && !res.refused && compose_answer(res).find("asleep") == std::string::npos)
        if (const std::string slept = asleep_at(db_, reading.window.at, now); !slept.empty()) {
            if (res.undecided && res.scope.empty() && res.lead.empty()) {
                res.lead = "No: a" + slept.substr(1) + " Around then:";
                res.undecided = false;
            } else {
                res.scope.insert(res.scope.begin(), slept + " Around then:");
            }
        }
    // The answer form is the part of a question most often read wrong (right 7 in 10 on unseen questions; the topic 8 in
    // 10), so the forms the matcher nearly picked within the same topic are answered too, after the asked one, in a fixed
    // order of forms: a question read as "which program" still gets its "when" and its "yes or no", and the answer is
    // right whichever was meant. Two at most; a form that says nothing, refuses, or repeats the main answer is left out.
    // Under --trace every candidate gets a receipt saying why it was added or left out.
    if (!fresh || res.refused || !force_kind.empty() || kind_ != match_.id)
        res.receipts.push_back(std::string("forms: none tried (") + (!fresh ? "a follow-up" : res.refused ? "refused" : !force_kind.empty() ? "kind forced" : "kind changed from " + match_.id + " to " + kind_) + ")");
    else if (const QuestionKind* k0 = kinds_.find(kind_); k0 && k0->topic != "none" && k0->topic != "programs" && k0->topic != "identity") {
            static const std::vector<std::string> order = {"yes or no", "a current value", "a value", "a value over a window", "when", "a time",
                                                           "which thing", "a list", "a cause", "how often or how long"};
            auto family = [](const std::string& f) {
                return f.rfind("a value", 0) == 0 || f == "a current value" ? std::string("value") : f == "a time" ? std::string("when") : f;
            };
            const bool about_now = reading.window.live || reading.window.present;
            std::vector<const QuestionKind*> near;
            for (std::size_t i = 0; i < match_.runners.size() && i < 5; ++i) {
                const std::string& id = match_.runners[i].first;
                const QuestionKind* ks = kinds_.find(id);
                if (!ks || ks->id == k0->id) continue;
                std::string why;
                if (ks->topic != k0->topic) why = "another topic";
                else if (ks->data == "missing") why = "not recorded";
                // A sibling well below the asked kind's own score is about something else in the same topic (the GPU's
                // clocks under a question on its memory): not added.
                else if (match_.runners[i].second < match_.score - kinds_.cutoff("sibling_gap", 0.06f)) why = "too far from the question (" + fmt("%.3f", match_.runners[i].second) + " against " + fmt("%.3f", match_.score) + ")";
                else if (family(ks->form) == family(k0->form)) why = "the asked form";
                else if (ks->window == (about_now ? "window" : "now")) why = "another time";  // a past question gets no "now" form
                else if (std::find(order.begin(), order.end(), ks->form) == order.end()) why = "a form not added";
                else
                    for (const auto* o : near)
                        if (family(o->form) == family(ks->form)) why = "form already taken by " + o->id;
                if (!why.empty()) {
                    res.receipts.push_back("form " + id + ": " + why);
                    continue;
                }
                near.push_back(ks);
            }
            std::sort(near.begin(), near.end(), [&](const QuestionKind* x, const QuestionKind* y) {
                return std::find(order.begin(), order.end(), x->form) < std::find(order.begin(), order.end(), y->form);
            });
            const unllm::Card card_saved = card_;
            const std::string kind_saved = kind_;
            std::size_t added = 0;
            for (const QuestionKind* ks : near) {
                if (added == 2) {
                    res.receipts.push_back("form " + ks->id + ": two already added");
                    continue;
                }
                // The sibling's form as the grammar would have read it, on the same reading otherwise.
                unllm::Reading alt = reading;
                alt.form = Form::Open;
                alt.wants = unllm::Wants::Fact;
                alt.judge = unllm::Judge::None;
                if (ks->form == "yes or no") alt.form = Form::Polar;
                else if (family(ks->form) == "when") alt.wants = unllm::Wants::Time;
                else if (ks->form == "a cause") alt.wants = unllm::Wants::Cause;
                else if (ks->form == "which thing") alt.wants = unllm::Wants::Name;
                else if (family(ks->form) == "value") alt.wants = unllm::Wants::Quantity;
                else if (ks->form == "how often or how long") alt.wants = unllm::Wants::Duration;
                kind_ = ks->id;
                card_ = card_saved;
                const AskResult s = answers_from_records(ks->id) ? answer_records(ks->id, alt, now)
                                                                 : answer_reading(alt, unllm::tokenize(q), raw_tokens, question, now);
                std::string why;
                // The sibling's first sentence: up to the first "; " or ". " (a figure's decimal point is not an end).
                std::size_t cut = std::string::npos;
                for (std::size_t i = 0; i + 1 < s.body.size() && cut == std::string::npos; ++i)
                    if ((s.body[i] == '.' || s.body[i] == ';') && s.body[i + 1] == ' ') cut = i;
                if (cut == std::string::npos || cut < 20) cut = std::min<std::size_t>(60, s.body.size());
                if (s.refused) why = "refused";
                else if (s.undecided) why = "undecided";
                else if (s.body.empty()) why = "nothing to say";
                else if (s.branch == res.branch) why = "the same answer code (" + s.branch + ")";
                else if (s.branch.find("none") != std::string::npos) why = "no records (" + s.branch + ")";
                else if (s.branch.find("which") != std::string::npos || s.body.rfind("Which ", 0) == 0) why = "a question back";
                else if (s.branch.rfind("live:", 0) == 0 && res.branch.rfind("live:", 0) != 0) why = "a live reading on a records answer";
                else if (s.body.rfind("No ", 0) == 0) why = "a bare no";
                else if (s.body.size() > 400) why = "too long";
                else if (res.body.find(s.body.substr(0, cut)) != std::string::npos) why = "already said";  // its first sentence is in the answer already
                else {
                    // A form whose every figure and time the main answer already gives repeats it: left out.
                    static const std::regex figure("[0-9]+:[0-9]{2}|[0-9]+(?:[.,][0-9]+)?%?");
                    bool fresh_fact = false, any = false;
                    for (std::sregex_iterator it(s.body.begin(), s.body.end(), figure), end; it != end; ++it) {
                        any = true;
                        if (res.body.find(it->str()) == std::string::npos && res.lead.find(it->str()) == std::string::npos) fresh_fact = true;
                    }
                    if (any && !fresh_fact) why = "its figures are already in the answer";
                }
                if (!why.empty()) {
                    res.receipts.push_back("form " + ks->id + ": " + why + " [" + s.branch + "]");
                    continue;
                }
                res.notes.push_back((s.lead.empty() ? std::string() : s.lead + " ") + s.body);
                res.receipts.push_back("also answered: " + ks->id + " [" + s.branch + "]");
                ++added;
            }
            kind_ = kind_saved;
            card_ = card_saved;
        }
    // "when i had the zoom call": a clause about the person, or a thing "during" names, that no record matches leaves
    // the time asked, and says so; one placed says which record it is.
    if (fresh && event_unmatched_ && !res.refused)
        res.scope.insert(res.scope.begin(), "Nothing in the records matches \"" + reading.event_clause + "\"" +
                                            (event_nearest_.empty() ? std::string(": no program on record is called or described that way")
                                                                    : " in that window: nothing about " + event_nearest_ + " was recorded then") +
                                            ", so this is for the time asked.");
    if (fresh && !sleep_note_.empty() && !res.refused) res.scope.insert(res.scope.begin(), sleep_note_);
    if (fresh && reading.event_during && event_at_ && !res.refused)
        res.scope.insert(res.scope.begin(), "Taking \"" + reading.event_clause + "\" as " + event_what_ + " (" + when_text(event_at_) + "):");
    // "This morning" asked at 02:38: say so, then answer yesterday's.
    if (reading.window.shifted && !res.refused) {
        std::time_t tt = now;
        std::tm tm{};
        localtime_r(&tt, &tm);
        char hm[8];
        std::strftime(hm, sizeof hm, "%H:%M", &tm);
        std::string part = "that part of the day";
        for (const auto& t : unllm::tokenize(q))
            if (t.lower == "morning" || t.lower == "afternoon" || t.lower == "evening") part = "this " + t.lower;
        res.scope.insert(res.scope.begin(), "It is only " + std::string(hm) + ", so " + part + " hasn't started yet. Yesterday's:");
    }
    // A question nothing fits gets the honest reply. A reading the read-back gate merely doubts is answered as read, with
    // no sentence about how it was taken (the user, 2026-10-25: that sentence helped nobody); the doubt stays in the
    // measurements (read_back) and in the trace.
    if (fresh_ && unplaced && !res.refused) {
        res.scope.clear();
        res.lead.clear();
        res.notes.clear();
        res.receipts.clear();
        res.undecided = false;
        res.body = "I can't place that in what this machine records.";
        res.branch = "unanswered:unplaced";
        res.refused = true;
        res.read_back = true;
        card_.refused = true;
    } else if (fresh_ && doubted && !res.refused) {
        res.read_back = true;
    }
    for (const auto& [typed, meant] : spelling) res.receipts.push_back("spelling: " + typed + " -> " + meant);
    if (future_fixed) res.receipts.push_back("window: a span after now was replaced by the last day");
    if (fresh && !reading.event_clause.empty())
        res.receipts.push_back(event_at_ ? "event: " + event_what_ + " (" + when_text(event_at_) + ")"
                                         : "event: nothing in the records matches \"" + reading.event_clause + "\" in that window");
    tag(res);
    remember_ = remember;
    window_ = reading.window;
    programs_.clear();
    for (const auto& e : reading.entities)
        if (e.type == "program") {
            programs_.push_back(e.value);
            const bool resolved = !named_.empty() && named_[0].program.name == e.value;
            res.programs.push_back(resolved ? named_[0].words + " -> " + e.value +
                                                  (named_[0].program.script.empty() ? "" : " (" + named_[0].program.script + ")")
                                            : e.value);
        }
    if (!named_.empty() && named_[0].by_unit) res.programs.push_back(named_[0].words + " -> unit " + named_[0].program.unit);
    return res;
}

void Ask::place_event(unllm::Reading& r, int64_t now) {
    if (r.event_clause.empty() || !kinds_.ready() || !db_) return;
    int64_t from = r.window.from, to = r.window.to;
    // The moments an event-like kind of the catalogue answers from, with their titles; the key names the record set.
    auto records = [&](const std::string& kind, std::string* key) -> std::vector<Row> {
        const std::string in = " AND t >= ?1 AND t < ?2 ORDER BY t";
        *key = kind;
        if (kind == "memory.oom.list.window")
            return query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'services' AND title LIKE '%oom-kill%'" + in,
                         {from, to});
        if (kind == "crashes.yesno.window.system") {
            // A stop by its first sentence after the "HH:MM:SS to HH:MM:SS (1 s). " lead ("The machine stopped without
            // warning on ..."): the rest of the summary is readings, which would blur its meaning.
            auto rows = query(db_, "SELECT CAST(start AS INTEGER), 0, 0, summary FROM events WHERE (kinds & ?3) != 0 AND start >= ?1 AND start < ?2 "
                                   "ORDER BY start", {from, to, int64_t(KindShutdown)});
            for (auto& x : rows) {
                if (const auto at = x.s1.find("). "); at != std::string::npos && at < 40) x.s1 = x.s1.substr(at + 3);
                x.s1 = x.s1.substr(0, x.s1.find(". "));
            }
            return rows;
        }
        if (kind.rfind("crashes.", 0) == 0) {
            *key = "crashes";
            return query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'problems'" + in, {from, to});
        }
        if (kind.rfind("battery.sleep.", 0) == 0 || kind.rfind("battery.lid.", 0) == 0) {
            *key = "sleep";
            return query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'session' AND (title LIKE 'Went to sleep%' "
                              "OR title LIKE 'Woke%' OR title LIKE 'Lid %')" + in, {from, to});
        }
        if (kind.rfind("updates.", 0) == 0) {
            *key = "software";
            return query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'software'" + in, {from, to});
        }
        return {};
    };
    // What the clause names ("the terminal": a terminal tab's unit) must be what the record names (its first word, a
    // unit or a program). With nothing named there, the program the question names is preferred ("is ollama still
    // running since it got killed").
    std::string unit, prog;
    const auto named = names_.find(r.event_clause, r, now);
    const bool from_clause = !named.empty();
    if (from_clause && named[0].by_unit)
        unit = named[0].program.unit;
    else if (from_clause)
        prog = named[0].program.name;
    else
        for (const auto& e : r.entities)
            if (e.type == "program") prog = e.value;
    auto names_it = [&](const std::string& title) {
        const std::string first = title.substr(0, title.find(' '));
        if (!unit.empty()) return fold_unit(first) == unit;
        return prog.empty() || is_program(prog, first, "", "") || title.find(prog) != std::string::npos;
    };
    struct Source {
        std::string key;
        std::vector<Row> rows;
    };
    std::vector<Source> sources;
    const KindMatch km = placer().match(r.event_clause);
    // The person's own doing ("when i had the zoom call") or a thing "during" names (the update) is an event only through
    // the record set of its nearest kind, when it names no program: else any record near it in meaning would do, and
    // none of them is it.
    const auto ct = unllm::tokenize(r.event_clause);
    const bool strict = !from_clause && (r.event_during || (!ct.empty() && (ct[0].lower == "i" || ct[0].lower == "we")));
    auto gather = [&] {
        for (std::size_t i = 0; i < km.runners.size() && i < (strict ? 1u : 8u) && km.runners[i].second >= placer().cutoff("records", 0.70f); ++i) {
            std::string key;
            auto rows = records(km.runners[i].first, &key);
            bool seen = false;
            for (const auto& s : sources) seen |= s.key == key;
            if (rows.empty() || seen) continue;
            // A stop is the whole machine's: a program the clause names ("it cut out" reads "cut") doesn't filter it.
            const bool machine = key == "crashes.yesno.window.system";
            std::vector<Row> kept;
            for (const auto& x : rows)
                if (machine || names_it(x.s1)) kept.push_back(x);
            if (kept.empty() && !from_clause) kept = rows;
            if (!kept.empty()) sources.push_back({key, std::move(kept)});
        }
    };
    // The clause's records over the window's own days, so one just outside the part of the day named counts ("when
    // it shut off on friday evening": the stop at 17:33, just before the evening began, not the lid closed at 22:03).
    auto midnight = [](int64_t t, int days) {
        std::time_t tt = t;
        std::tm tm{};
        localtime_r(&tt, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        tm.tm_mday += days;
        tm.tm_isdst = -1;
        return int64_t(std::mktime(&tm));
    };
    if (r.window.stated) {
        from = std::min(r.window.from, midnight(r.window.from, 0));
        to = std::max(r.window.to, std::min(now, midnight(r.window.to - 1, 1)));
    } else if (r.event_during) {
        // "during the update" with no time named: the latest one, however far back.
        from = 0;
        to = now;
    }
    gather();
    event_unmatched_ = strict && sources.empty();
    if (sources.empty()) {
        // "After waking up" with no wake then: the clause is nearest a kind of record the machine does keep (sleeps and
        // wakes), and none falls in the window; the note says so in the catalogue's plain words. The person's own doing
        // keeps the note about programs; a clause the answer code reads itself ("when the fan hit its peak") is nearest
        // no record set and gets no note.
        if (!strict && !from_clause) {
            const int64_t f0 = from, t0 = to;
            from = 0;
            to = now;
            for (const auto& [id, score] : km.runners) {
                if (score < placer().cutoff("records", 0.70f)) break;
                const QuestionKind* k = kinds_.find(id);
                if (!k || k->topic == "none" || k->topic == "programs") continue;
                std::string key;
                if (records(id, &key).empty()) continue;
                event_nearest_ = kinds_.plain("topic", k->topic);
                event_unmatched_ = true;
                break;
            }
            from = f0;
            to = t0;
        }
        return;
    }
    // Which record set, and for the session's own lines which line: the title nearest the clause by meaning
    // ("i shut the lid" is "Lid closed", not an out-of-memory kill).
    std::size_t pick = 0;
    std::string title;
    if (const unllm::Meaning* model = meaning(); model && model->can_embed()) {
        const auto q = model->embed(r.event_clause, false);
        float best = -2;
        for (std::size_t s = 0; s < sources.size(); ++s) {
            std::set<std::string> titles;
            for (const auto& x : sources[s].rows) titles.insert(x.s1);
            for (const auto& t : titles) {
                const auto v = model->embed(t, false);
                float d = 0;
                for (std::size_t k = 0; k < q.size() && k < v.size(); ++k) d += q[k] * v[k];
                if (d > best) best = d, pick = s, title = t;
            }
        }
    }
    const std::vector<Row>& all = sources[pick].rows;
    std::vector<Row> rows = all;
    if (sources[pick].key == "sleep" && !title.empty())
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& x) { return x.s1 != title; }), rows.end());
    if (rows.empty()) return;
    // The latest such record in the window, else the one nearest it on its days; the span runs up to it or on from it,
    // or is its moment. With no length named, the span before it starts at the record of the same set just before it
    // ("before i shut the lid": since it woke).
    auto off = [&](int64_t t) { return t < r.window.from ? r.window.from - t : t >= r.window.to ? t - r.window.to + 1 : 0; };
    const Row* hit = nullptr;
    for (const auto& x : rows)
        if (!hit || off(x.t) == 0 || off(x.t) < off(hit->t)) hit = &x;
    const bool beside = off(hit->t) > 0;
    event_at_ = hit->t;
    event_what_ = hit->s1;
    if (beside && event_at_ < r.window.from) r.window.from = from;
    if (beside && event_at_ >= r.window.to) r.window.to = to;
    if (r.event_at || r.event_during) {
        // Its moment: the two minutes before it, as the recorder keeps them for a stop. "During" answers over them and
        // the minute after: a log row (the software one) is written as the work ends, and its programs end just after.
        r.window.from = event_at_ - 120;
        r.window.to = event_at_ + (r.event_during ? 60 : 1);
    } else if (r.event_after) {
        r.window.from = event_at_;
        if (r.event_span) r.window.to = event_at_ + r.event_span;
    } else {
        r.window.to = event_at_;
        if (r.event_span) {
            r.window.from = event_at_ - r.event_span;
        } else {
            for (const auto& x : all)
                if (x.t < event_at_) r.window.from = std::max(r.window.from, x.t);
        }
    }
    r.window.stated = true;
    r.window.live = r.window.present = false;
    r.window.at = 0;  // the span is the event's now, not a moment's
}

std::string Ask::kind_words(const std::string& kind) const {
    const QuestionKind* k = kinds_.find(kind);
    if (!k) return kind;
    if (k->topic == "none") return "outside what the records hold";
    const std::string when = k->window == "now" ? "now" : k->window == "window" ? "in the past" : "now or in the past";
    return kinds_.plain("topic", k->topic) + " (" + kinds_.plain("form", k->form) + ", " + when + ")";
}

// "while the lid was closed": a "while ..." clause that names no program, checked against the states the session log
// keeps. Each state begins at a row like its first title and ends at the next row like its second, as the recorder
// writes them; the clause names the state whose first title is nearest it by meaning, at 0.85 or more (bge-small:
// "lid closed" 1.00, "the lid was shut" 0.91, "it was unplugged" 0.87; clauses about programs or the person stay
// below 0.76). The lead says when the state held in the window, or that it never did; empty when the clause names
// none.
std::string Ask::state_lead(const unllm::Reading& r, int64_t now) const {
    struct State { const char *begins, *ends, *held, *never; };
    static const State states[] = {{"Lid closed", "Lid opened", "the lid was closed", "the lid wasn't closed"},
                                   {"Lid opened", "Lid closed", "the lid was open", "the lid wasn't open"},
                                   {"Went to sleep", "Woke from sleep", "it was asleep", "it didn't sleep"},
                                   {"Unplugged from power", "Plugged into power", "it was on battery", "it wasn't on battery"},
                                   {"Plugged into power", "Unplugged from power", "it was on the charger", "it wasn't on the charger"}};
    const unllm::Meaning* model = meaning();
    if (r.condition_clause.empty() || !r.condition.empty() || !model || !model->can_embed() || !db_) return "";
    const auto q = model->embed(r.condition_clause, false);
    const State* named = nullptr;
    float best = 0.85f;
    for (const auto& st : states) {
        const auto v = model->embed(st.begins, false);
        float d = 0;
        for (std::size_t k = 0; k < q.size() && k < v.size(); ++k) d += q[k] * v[k];
        if (d >= best) best = d, named = &st;
    }
    if (!named) return "";
    const int64_t from = r.window.stated ? r.window.from : now - 86400, to = r.window.stated ? r.window.to : now;
    const std::string b = std::string(named->begins) + "%", e = std::string(named->ends) + "%";
    auto rows = query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'session' AND t < ?1 AND (title LIKE ?2 OR title LIKE ?3) "
                           "ORDER BY t", {to}, {b, e});
    std::vector<std::pair<int64_t, int64_t>> held;
    int64_t since = 0, last = 0;
    for (const auto& x : rows) {
        const bool begins = x.s1.rfind(named->begins, 0) == 0;
        if (begins) {
            if (!since) since = x.t;
            if (x.t < from) last = x.t;
        } else if (since) {
            if (x.t > from) held.push_back({std::max(since, from), x.t});
            since = 0;
        }
    }
    if (since) held.push_back({std::max(since, from), to});
    const std::string span = r.window.stated ? "between " + span_text(from, to, " and ") : "in the last day";
    std::string text;
    if (held.empty()) {
        text = std::string(named->never) + " " + span + (last ? " (the last time was " + when_text(last) + ")" : "") + ". For the whole window:";
    } else {
        std::vector<std::string> when;
        for (const auto& [a, z] : held)
            if (when.size() < 3) when.push_back("from " + span_text(a, z, " to "));
        if (held.size() > when.size()) when.push_back(unllm::number_word(long(held.size() - when.size())) + " more times");
        text = std::string(named->held) + " " + unllm::join_list(when) + ".";
    }
    text[0] = char(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

int Ask::asked_test(const unllm::Reading& r, const std::vector<std::vector<const char*>>& tests) const {
    if (r.judge != unllm::Judge::None) return -1;
    const unllm::Meaning* model = meaning();
    if (!model || !model->can_embed()) return 0;
    const auto q = model->embed(card_.question, false);
    int pick = 0;
    float best = -2;
    for (std::size_t i = 0; i < tests.size(); ++i)
        for (const char* w : tests[i]) {
            const auto v = model->embed(w, false);
            float d = 0;
            for (std::size_t k = 0; k < q.size() && k < v.size(); ++k) d += q[k] * v[k];
            if (d > best) best = d, pick = int(i);
        }
    return pick;
}

std::vector<std::string> Ask::named_words(const unllm::Reading& r, const std::string& kind) const {
    static std::set<std::string> known;
    if (known.empty())
        for (const auto& w : an_.known_words(false)) known.insert(w);
    std::set<std::string> own;
    if (const QuestionKind* k = kinds_.find(kind))
        for (const auto& t : unllm::tokenize(k->answer)) own.insert(t.norm), own.insert(t.lower);
    std::vector<std::string> out;
    for (const auto& t : r.terms) {
        bool program = false;
        for (const auto& e : r.entities) program |= e.type == "program" && e.value == t;
        if (!program && (t.size() < 3 || known.count(t) || own.count(t) || kinds_.topics_using(t) >= 5)) continue;
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    return out;
}

std::optional<Ask::Verdict> Ask::check_answer(const std::string& question, const std::string& kind, const AskResult& res,
                                              const std::vector<std::string>& others) {
    auto example = [&](const std::string& id) {
        for (std::size_t i = 0; i < match_.runners.size() && i < match_.runner_phrasings.size(); ++i)
            if (match_.runners[i].first == id && !match_.runner_phrasings[i].empty())
                return " (like \"" + match_.runner_phrasings[i] + "\")";
        return std::string();
    };
    std::string span = window_.live || window_.present ? "now"
                       : window_.from > 0          ? span_text(window_.from, window_.to, " to ")
                                                   : "not stated";
    const std::string said = compose_answer(res);
    std::string answer = said.size() > 700 ? said.substr(0, 700) + "..." : said;
    for (std::size_t i = 0; i < res.receipts.size() && i < 4; ++i) {
        if (res.receipts[i].rfind("spelling:", 0) == 0) continue;
        const std::string& line = res.receipts[i];
        answer += "\n  " + (line.size() > 160 ? line.substr(0, 160) + "..." : line);
    }
    std::string programs;
    for (const auto& p : programs_) programs += (programs.empty() ? "" : ", ") + p;
    std::string prompt =
        "A tool answers questions about one Linux laptop from that laptop's own records: temperatures and fans, "
        "CPU and memory use per program, disk and network use, crashes, boots and sleep, battery, GPU, updates, "
        "config changes, devices. Check one of its answers.\n\nQuestion: \"" + question + "\"\n\n"
        "The tool read the question as:\nA. " + kind_words(kind) + example(kind) + "\nTime it looked at: " + span +
        "\nProgram named: " + (programs.empty() ? "none" : programs) + "\n\nIts answer:\n" + answer + "\n";
    if (!others.empty()) {
        prompt += "\nOther readings it considered:\n";
        for (std::size_t i = 0; i < others.size(); ++i)
            prompt += std::string(1, char('B' + i)) + ". " + kind_words(others[i]) + example(others[i]) + "\n";
    }
    prompt += "\nDecide, in this order:\n\"asks\": what the question asks for, at most eight words.\n"
              "\"answers\": \"no\" if the answer is about a different subject, or leaves out a program the question "
              "names; \"partly\" if it is on the right subject but gives a different figure or a different time than "
              "asked, or a general summary where something specific was asked; \"yes\" if it gives what was asked.\n";
    if (!others.empty())
        prompt += "\"better\": the letter of the reading that fits the question best; \"A\" if the tool's own reading "
                  "fits best.\n";
    prompt += "\"why\": at most six words.\nReply with only JSON: {\"asks\": \"...\", \"answers\": \"yes|partly|no\", " +
              std::string(others.empty() ? "" : "\"better\": \"A\", ") + "\"why\": \"...\"}";
    auto reply = checker_.json(prompt, 60);
    if (!reply) return std::nullopt;
    Verdict v{unllm::json_field(*reply, "answers"), unllm::json_field(*reply, "better"), unllm::json_field(*reply, "why")};
    for (auto& c : v.answers) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (v.answers != "yes" && v.answers != "partly" && v.answers != "no") return std::nullopt;
    return v;
}

AskResult Ask::ask(const std::string& question, int64_t now, Mode mode) {
    // "what's the battery health and how fast is it draining": two questions about now in one line, each close to a kind of
    // its own and told by a handler of its own, get both answers in turn. Anything else is one question.
    if (forced_.empty() && kinds_.ready()) {
        if (const std::size_t cut = unllm::second_question(question); cut != std::string::npos) {
            std::string first = question.substr(0, cut);
            const std::string second = question.substr(cut);
            auto trim_end = [](std::string& s) {
                while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == ',' || s.back() == ';')) s.pop_back();
            };
            trim_end(first);
            if (first.size() > 4 && first.compare(first.size() - 4, 4, " and") == 0) first.resize(first.size() - 4), trim_end(first);
            const KindMatch a = kinds_.match(first), b = kinds_.match(second);
            const QuestionKind* ka = kinds_.find(a.id);
            const QuestionKind* kb = kinds_.find(b.id);
            if (ka && kb && a.id != b.id && a.score >= kinds_.cutoff("two_questions", 0.85f) && b.score >= kinds_.cutoff("two_questions", 0.85f) && ka->window != "window" && kb->window != "window" &&
                ka->topic != "none" && kb->topic != "none") {
                AskResult one = ask_once(first, now, "", mode);
                unllm::Card card_one = card_;
                const bool keep_one = remember_;
                AskResult two = ask_once(second, now, "", mode);
                if (one.when == "now" && two.when == "now" && !one.refused && !two.refused && one.branch != two.branch) {
                    one.answer = compose_answer(one);
                    two.answer = compose_answer(two);
                    if (keep_one) {
                        card_one.answer = one.answer;
                        card_one.receipts = one.receipts;
                        card_one.branch = one.branch;
                        dialogue_.remember(card_one);
                    }
                    if (remember_) {
                        card_.answer = two.answer;
                        card_.receipts = two.receipts;
                        card_.branch = two.branch;
                        dialogue_.remember(card_);
                    }
                    AskResult res = one;
                    res.answer = one.answer + " " + two.answer;
                    res.branch = one.branch + " + " + two.branch;
                    res.read_back = one.read_back || two.read_back;
                    res.receipts.insert(res.receipts.end(), two.receipts.begin(), two.receipts.end());
                    res.programs.insert(res.programs.end(), two.programs.begin(), two.programs.end());
                    return res;
                }
            }
        }
    }
    AskResult res = ask_once(question, now, forced_, mode);
    // The final check: only a new question with an answer from the records, not banter,
    // a follow-up or a request to clarify.
    if (checker_.enabled() && forced_.empty() && remember_ && fresh_ && !card_.clarify && !match_.id.empty() &&
        res.branch.rfind("never-ran", 0) != 0) {
        const std::string kind = match_.id;
        std::vector<std::string> others;
        if (!offered_.empty() && offered_ != kind && kinds_.find(offered_)) others.push_back(offered_);
        for (const auto& [id, score] : match_.runners)
            if (id != kind && id != offered_ && others.size() < 4) others.push_back(id);
        auto v = check_answer(question, kind, res, others);
        if (!v) {
            res.check = "unavailable";
        } else {
            std::string verdict = v->answers;
            const int pick = v->better.size() == 1 ? v->better[0] - 'B' : -1;
            if (pick >= 0 && pick < int(others.size())) {
                // Another reading fits better: ask's own code answers it, and the check reads that answer too.
                const unllm::Card first_card = card_;
                const KindMatch first_match = match_;
                const unllm::TimeWindow first_window = window_;
                const std::vector<std::string> first_programs = programs_;
                const std::string other = others[std::size_t(pick)];
                AskResult again = ask_once(question, now, other, mode);
                auto v2 = remember_ ? check_answer(question, other, again, {}) : std::nullopt;
                if (v2 && v2->answers == "yes") {
                    again.check = "kind " + kind + " -> " + other + " (" + v->why + ")";
                    res = again;
                    verdict = "yes";
                } else {
                    card_ = first_card;
                    window_ = first_window;
                    programs_ = first_programs;
                    remember_ = true;
                    res.check = "tried " + other + ": " + (v2 ? v2->answers : std::string("unavailable"));
                }
                match_ = first_match;
            }
            if (res.check.rfind("kind ", 0) != 0) {
                if (verdict == "no" && !res.refused) {
                    // Not a confident answer to a different question: say how it was read.
                    std::string span = window_.live || window_.present ? "as about now"
                                       : window_.from > 0 ? "as about " + span_text(window_.from, window_.to, " to ")
                                                          : "";
                    if (res.read_back) res.scope.erase(res.scope.begin());  // the gate's read-back, said again here
                    res.scope.insert(res.scope.begin(), "I may have misread this. I took it as a question about " + kind_words(kind) +
                                                            (span.empty() ? "" : ", " + span) + ", and found:");
                    res.check = (res.check.empty() ? "" : res.check + "; ") + "doubt (" + v->why + ")";
                } else {
                    res.check = (res.check.empty() ? "" : res.check + "; ") + verdict + (v->why.empty() ? "" : " (" + v->why + ")");
                }
            }
        }
    }
    res.answer = compose_answer(res);
    if (remember_) {
        card_.answer = res.answer;
        card_.receipts = res.receipts;
        card_.refused = card_.refused || res.refused;
        card_.branch = res.branch;
        dialogue_.remember(card_);
    }
    return res;
}

AskResult Ask::answer_reading(const unllm::Reading& r, const std::vector<unllm::Token>& tokens,
                              const std::vector<unllm::Token>& raw_tokens, const std::string& question,
                              int64_t now) {
    AskResult res;
    card_.reading = r;
    card_.window = r.window;
    card_.topic = r.intents.empty() ? Intent::Story : r.intents[0];

    // Understanding under 0.6 means guessing, and guessing is the failure
    // mode this tool exists to not have: banter the classics, otherwise say
    // what was understood and suggest the nearest askable question.
    if (r.domain < 0.6) {
        std::string b = banter_.match(raw_tokens);
        if (!b.empty()) {
            res.body = b;
            return res;
        }
        res.refused = true;
        if (!r.intents.empty()) {
            std::vector<std::string> topics;
            for (auto i : r.intents) topics.push_back(unllm::intent_name(i));
            res.body = "I half understood that. Are you asking about " + unllm::join_list(topics) +
                       "? Try: \"" + intent_example(r.intents[0]) + "\"";
            // A "yes" or a topic word in reply answers this question.
            card_.clarify = true;
            card_.offered = r.intents;
            res.branch = "clarify";
        } else {
            res.body = unllm::refuse(r, "this machine");
            res.branch = "refuse";
        }
        log_miss(question, r);
        return res;
    }

    const bool polar = r.form == Form::Polar;
    int64_t from = r.window.from, to = r.window.to;
    // "when did i last update", "when did i flash the esp32", "what was the
    // last crash": a when/last question with no stated window means the most
    // recent occurrence in the whole history, not within the default day.
    const bool when_frame = r.wants == unllm::Wants::Time && !r.window.stated;
    std::string program;
    std::vector<std::string> search_terms = r.terms;
    for (const auto& e : r.entities)
        if (e.type == "program") {
            program = e.value;
            search_terms.push_back(e.value);
        }

    // "Tell me more" reruns the same answer with receipt_offset_ set, so the
    // next page of records shows instead of the first again.
    auto add_receipts = [&](const std::string& sql, const std::vector<int64_t>& binds,
                            const std::vector<std::string>& tb = {}) {
        std::size_t seen = 0;
        for (const auto& row : query(db_, sql, binds, tb)) {
            if (seen++ < receipt_offset_) continue;
            if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
        }
    };

    // One intent leads; Identity takes the lead over a workload topic when no
    // time window was stated ("what cpu does this machine have").
    Intent lead = r.intents.empty() ? Intent::Story : r.intents[0];
    bool wants_identity = false;
    for (auto i : r.intents) wants_identity |= i == Intent::Identity;
    if (wants_identity && !r.window.stated) lead = Intent::Identity;

    // ---- help: what can be asked ----
    if (lead == Intent::Help) {
        res.body = "Ask about heat, slowdowns, memory, disk, network and sharing, crashes, updates, "
                   "startup, battery, trust, devices you plugged in, firmware flashing, the machine "
                   "itself, or what is happening right now. "
                   "Plain words work; every answer shows the records it stands on.";
        for (const char* ex : {"why did the fan scream last night", "what crashed this week",
                               "which program used the most memory this week", "how hot is it right now",
                               "what is this machine", "did blender run yesterday"})
            res.receipts.push_back(std::string("try: ") + ex);
        return res;
    }

    // ---- the health check: run the whole rule book (a named program has its own story, below) ----
    if (lead == Intent::Diagnose && program.empty()) {
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        SnapshotPtr s = sampler_->sample();
        // About now: the live figures and what is acute in them lead (rates need a second look); the rule book's
        // findings over the records follow.
        const bool about_now = r.window.live || r.window.present;
        if (about_now) {
            usleep(600 * 1000);
            s = sampler_->sample();
        }
        Diagnose diag(db_);
        auto cons = diag.run(now - 30 * 86400, now, now, s.get());
        if (about_now) {
            res.body = health_now(*s, low_battery_level(roots_.etc));
            // Slow with the CPU idle: the other places a machine waits, said when they show.
            if (s->mem.pressure_some10.ok() && s->mem.pressure_some10.get() >= 1)
                res.body += " Programs waited on memory " + fmt("%.0f%%", s->mem.pressure_some10.get()) + " of the time in the last ten seconds.";
            res.body += " From the last 30 days of records:";
            for (std::size_t i = 0; i < cons.size() && i < 3; ++i) {
                res.body += " " + cons[i].sentence;
                if (!cons[i].gap.empty() && res.receipts.size() < 6) res.receipts.push_back("gap: " + cons[i].gap);
                for (const auto& rc : cons[i].receipts)
                    if (res.receipts.size() < 6) res.receipts.push_back(rc);
            }
            if (cons.empty()) res.body += " nothing the rules can see is wrong.";
            res.branch = "diagnose:now";
            return res;
        }
        if (cons.empty()) {
            res.body = "Nothing is wrong that the rules can see: no disk errors, no dead fans, no "
                       "thermal shutdowns, no out-of-memory kills, no update regressions, no memory "
                       "leaks, no swap thrashing and no disk filling up, across the last 30 days.";
            res.receipts.push_back("checks run: disk errors, dead fan, thermal shutdown, OOM, update "
                                   "regression, swap, leak, disk space, disk wait, battery wear");
        } else {
            for (std::size_t i = 0; i < cons.size() && i < 3; ++i) {
                res.body += (res.body.empty() ? "" : " ") + cons[i].sentence;
                if (!cons[i].gap.empty() && res.receipts.size() < 6)
                    res.receipts.push_back("gap: " + cons[i].gap);
                for (const auto& rc : cons[i].receipts)
                    if (res.receipts.size() < 6) res.receipts.push_back(rc);
            }
        }
        res.branch = "diagnose";
        return res;
    }

    // The resource events a topic maps to.
    auto mask_for = [](Intent i) -> unsigned {
        switch (i) {
        case Intent::Heat: return 16;
        case Intent::Slow: return 1;
        case Intent::Memory: return 2;
        case Intent::Disk: return 4;
        case Intent::Gpu: return 8;
        default: return 0;
        }
    };

    // ---- worries and advice: the rule book, narrowed to the topic ----
    if (r.frame == unllm::Frame::Worry || r.frame == unllm::Frame::Advice) {
        static const std::map<Intent, std::set<std::string>> rules_for = {
            {Intent::Disk, {"disk_errors", "disk_wait", "time_to_full"}},
            {Intent::Heat, {"dead_fan", "thermal_shutdown"}},
            {Intent::Gpu, {"dead_fan"}},
            {Intent::Memory, {"oom_kill", "leak", "swap_thrash", "swap_parked"}},
            {Intent::Battery, {"battery_aged"}},
            {Intent::Update, {"update_regression"}},
        };
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        SnapshotPtr s = sampler_->sample();
        Diagnose diag(db_);
        auto all = diag.run(now - 30 * 86400, now, now, s.get());
        auto want = rules_for.find(lead);
        std::vector<Conclusion> mine;
        for (const auto& c : all)
            if (want == rules_for.end() || want->second.count(c.rule_id)) mine.push_back(c);
        const bool advice = r.frame == unllm::Frame::Advice;
        const std::string topic = lead == Intent::Story ? "the machine" : unllm::intent_name(lead);
        if (!mine.empty()) {
            bool real = false;
            for (const auto& c : mine) real |= c.rule_id != "swap_parked";
            res.lead = advice ? "" : (real ? "Yes, that deserves attention." : "No.");
            for (std::size_t i = 0; i < mine.size() && i < 3; ++i) {
                res.body += mine[i].sentence;
                if (advice || mine[i].salience >= 2.5) res.body += " " + advice_for(mine[i].rule_id);
                res.body += i + 1 < mine.size() && i < 2 ? " " : "";
                for (const auto& rc : mine[i].receipts)
                    if (res.receipts.size() < 6) res.receipts.push_back(rc);
            }
        } else {
            unsigned mask = mask_for(lead);
            std::string culprit = mask ? top_culprit(db_, now - 30 * 86400, now, mask, "") : "";
            if (advice && !culprit.empty()) {
                res.body = "Nothing is broken that the rules can see; " + culprit + " did most of that work in the "
                           "last 30 days, so closing or pausing it is the lever. Nothing has been run for you.";
            } else {
                if (!advice) res.lead = "No.";
                res.body = std::string(advice ? "Nothing in the records needs doing about " : "Nothing in the records says there is a problem with ") +
                           topic + " in the last 30 days.";
            }
        }
        if (lead == Intent::Disk)
            res.notes.push_back("The drive's own health counters (SMART) need administrator access, so I can't read its "
                                "wear; what I can see is the kernel log and the disk timings.");
        res.branch = std::string(advice ? "advice:" : "worry:") + unllm::intent_name(lead);
        return res;
    }

    // ---- either/or: "is blender or firefox worse for heat" ----
    if (r.frame == unllm::Frame::Choice && r.choices.size() == 2) {
        unsigned mask = mask_for(lead);
        const int64_t cfrom = r.window.stated ? from : now - 30 * 86400;
        // CPU work (slowdowns, heat, no topic): each program's CPU over the window from the per-minute lists, with
        // the units it heads ("ollama" with its llama-server runners), not its shares of the spikes alone.
        if (mask == 1 || mask == 16 || mask == 0) {
            std::vector<Row> parts[2];
            double sum[2] = {0, 0};
            for (int i = 0; i < 2; ++i) {
                parts[i] = program_cpu(db_, r.choices[std::size_t(i)], cfrom, to, now);
                for (const auto& x : parts[i]) sum[i] += x.a;
            }
            if (sum[0] > 0 || sum[1] > 0) {
                const int a = sum[0] >= sum[1] ? 0 : 1, b = 1 - a;
                auto side = [&](int i) {
                    std::string s = thousands(sum[i]) + " CPU seconds";
                    if (parts[i].size() > 1 && parts[i][1].a >= 1) {
                        std::vector<std::string> each;
                        for (const auto& x : parts[i])
                            if (each.size() < 3 && x.a >= 1) each.push_back(shown_name(x.s1, x.s2) + " " + thousands(x.a) + " s");
                        s += " (" + unllm::join_list(each) + ")";
                    }
                    return s;
                };
                const std::string reach = r.window.stated ? "in that window" : "in the 30 days of records";
                std::string body = r.choices[std::size_t(a)] + " used more CPU " + reach + ": about " + side(a) + ", against " +
                                   r.choices[std::size_t(b)] + "'s " + (sum[b] > 0 ? side(b) : "none recorded") +
                                   (sum[b] > 0 && sum[a] >= 1.5 * sum[b] ? ", " + fmt("%.1f", sum[a] / sum[b]) + " times as much" : "") + ".";
                if (polar) settle(res, r, r.degree == Degree::Least ? sum[0] < sum[1] : sum[0] > sum[1]);
                body[0] = char(std::toupper(static_cast<unsigned char>(body[0])));
                res.body = body;
                res.receipts.push_back("per-minute CPU, each minute held to what the whole machine used in it");
                res.branch = "choice";
                card_.subject = r.choices[std::size_t(a)];
                card_.subjects_named = {r.choices[0], r.choices[1]};
                return res;
            }
        }
        const int64_t cmask = mask ? int64_t(mask) : 0xff;
        struct Side { std::string name; int64_t events = 0; double work = 0; };
        std::vector<Side> sides;
        for (const auto& name : r.choices) {
            auto rows = query(db_, "SELECT COUNT(*), SUM(c.share * IFNULL(e.total_cpu_seconds, 1)), 0 "
                                   "FROM events e JOIN event_culprits c ON c.event_id = e.id "
                                   "WHERE e.start < ? AND e.\"end\" >= ? AND (e.kinds & ?) != 0 AND is_program(?, c.name, '', c.cmdline)",
                              {to, cfrom, cmask}, {name});
            sides.push_back({name, rows.empty() ? 0 : rows[0].t, rows.empty() ? 0 : rows[0].a});
        }
        const std::string about = mask ? std::string(" for ") + unllm::intent_name(lead) : "";
        const std::string reach = r.window.stated ? "in that window" : "in the 30 days of records";
        if (sides[0].events == 0 && sides[1].events == 0) {
            res.body = "Neither " + sides[0].name + " nor " + sides[1].name + " was behind any recorded " +
                       (mask ? std::string(unllm::intent_name(lead)) + " " : "") + "events " + reach + ".";
        } else {
            const Side& a = sides[0].work >= sides[1].work ? sides[0] : sides[1];
            const Side& b = &a == &sides[0] ? sides[1] : sides[0];
            auto work = [&](const Side& s) {
                return "about " + duration_text(int64_t(s.work)) + " of work across " + unllm::number_word(s.events) +
                       " " + unllm::plural_noun("event", s.events);
            };
            res.body = unllm::realize({{{a.name, true}, "weigh", "more" + about + " " + reach + ": " + work(a),
                                        Tense::Past}});
            res.body.pop_back();
            res.body += b.events ? ", against " + b.name + "'s " + work(b) + "." : ", while " + b.name + " was behind none.";
        }
        res.branch = "choice";
        card_.subject = sides[0].work >= sides[1].work ? sides[0].name : sides[1].name;
        card_.subjects_named = {sides[0].name, sides[1].name};
        return res;
    }

    // ---- identity: the machine itself, measured, not recalled ----
    if (lead == Intent::Identity) {
        SystemId id = system_identity();
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        SnapshotPtr s = sampler_->sample();
        std::vector<Clause> clauses;
        if (has_word(tokens, {"kernel"})) {
            clauses.push_back({{"this machine", true}, "run", "kernel " + id.kernel, Tense::Present});
        } else if (has_word(tokens, {"os", "distro"})) {
            clauses.push_back({{"this", true}, "be", id.os_pretty + ", on kernel " + id.kernel, Tense::Present});
        } else if (has_word(tokens, {"hostname", "host", "name", "call"})) {
            clauses.push_back({{"this machine", true}, "be", "called " + id.hostname, Tense::Present});
        } else if (has_word(tokens, {"long", "uptime"})) {
            clauses.push_back({{"it", true}, "have", "been up for " + duration_text(id.uptime_seconds), Tense::Present});
        } else if (has_word(tokens, {"cpu", "processor", "core"})) {
            clauses.push_back({{"the CPU", true}, "be",
                               (s->cpu.model.empty() ? std::string("unknown") : s->cpu.model) + ", " +
                                   unllm::number_word(s->cpu.physical) + " cores and " +
                                   unllm::number_word(s->cpu.logical) + " threads",
                               Tense::Present});
        } else if (has_word(tokens, {"ram", "memory"})) {
            clauses.push_back({{"it", true}, "have", human_bytes(double(s->mem.total)) + " of memory, with " +
                                                         human_bytes(double(s->mem.available)) + " free right now",
                               Tense::Present});
        } else if (has_word(tokens, {"disk", "drive", "space", "storage"})) {
            std::string parts;
            for (const auto& d : s->disks) {
                if (!parts.empty()) parts += " and ";
                parts += human_bytes(double(d.size)) + " " + (d.kind.empty() ? d.name : d.kind);
            }
            clauses.push_back({{"it", true}, "have", parts.empty() ? "no disks it can see" : parts, Tense::Present});
        } else {
            std::string what;
            if (!id.vendor.empty() || !id.product.empty())
                what = unllm::indefinite(id.vendor + (id.product.empty() ? "" : " " + id.product));
            else
                what = "a machine";
            if (!id.os_pretty.empty()) what += " running " + id.os_pretty;
            what += " (kernel " + id.kernel + ")";
            clauses.push_back({{"this", true}, "be", what, Tense::Present});
            std::string parts = s->cpu.model.empty() ? "an unknown CPU" : unllm::indefinite(s->cpu.model);
            parts += " with " + unllm::number_word(s->cpu.physical) + " cores and " +
                     human_bytes(double(s->mem.total)) + " of memory";
            clauses.push_back({{"it", true}, "have", parts, Tense::Present});
            clauses.push_back({{"it", true}, "have", "been up for " + duration_text(id.uptime_seconds), Tense::Present});
        }
        res.body = unllm::realize(clauses);
        res.receipts.push_back("vendor: " + id.vendor + "  product: " + id.product);
        res.receipts.push_back("os: " + id.os_pretty + "  kernel: " + id.kernel + "  arch: " + id.arch);
        res.receipts.push_back("hostname: " + id.hostname + "  up: " + duration_text(id.uptime_seconds));
        res.branch = "identity";
        card_.subject = "this machine";
        return res;
    }

    // ---- what is X: the fact table plus the measured history ----
    {
        std::string whatis;
        for (const auto& e : r.entities)
            if (e.type == "name") whatis = e.value;
        if (whatis.empty() && (r.wants == unllm::Wants::Name || r.frame == unllm::Frame::Define) &&
            r.intents.empty() && r.degree == Degree::None && !r.window.stated && !program.empty()) {
            whatis = program;
            // A word the fact table holds exactly beats a near-miss program ("chrome" read as chroma).
            for (const auto& t : tokens)
                if (t.lower != program && t.lower.size() >= 3)
                    if (const Fact* f = facts_.lookup(t.lower); f && strcasecmp(f->pattern.c_str(), t.lower.c_str()) == 0) whatis = t.lower;
        }
        if (!whatis.empty() && r.intents.empty() && !r.window.stated) {
            // The records can resolve a name the table alone cannot: the
            // executable path, or the script an interpreter was running.
            auto prow = query(db_, "SELECT 0,0,0,exe,cmdline FROM procs WHERE is_program(?, name, exe, cmdline) AND exe <> '' "
                                   "ORDER BY started DESC LIMIT 1", {}, {whatis});
            std::string exe = prow.empty() ? "" : prow[0].s1;
            std::string cmdline = prow.empty() ? "" : prow[0].s2;
            const Fact* f = facts_.lookup(whatis);
            if (!f && !exe.empty()) {
                f = facts_.lookup(exe.substr(exe.rfind('/') + 1));
                if (!f) f = facts_.lookup(exe);  // the path rules
            }
            // An interpreter is only a vehicle: name the script when there is one.
            std::string script;
            if (f && f->category == "runtime" && !cmdline.empty()) {
                std::size_t sp = cmdline.find(' ');
                while (sp != std::string::npos && sp + 1 < cmdline.size() && cmdline[sp + 1] == '-') {
                    sp = cmdline.find(' ', sp + 1);
                }
                if (sp != std::string::npos) {
                    std::string arg = cmdline.substr(sp + 1, cmdline.find(' ', sp + 1) - sp - 1);
                    script = arg.substr(arg.rfind('/') + 1);
                }
            }
            auto hist = query(db_, "SELECT COUNT(*), MIN(started), MAX(peak_rss), CAST(SUM(cpu_seconds) AS TEXT) "
                                   "FROM procs WHERE is_program(?, name, exe, cmdline)", {}, {whatis});
            int64_t runs = hist.empty() ? 0 : hist[0].t;
            if (f) {
                res.body = unllm::realize({{{whatis, true}, "be", f->explanation, Tense::Present}});
                if (!script.empty()) res.body += " Here it was running " + script + ".";
                if (runs > 0) {
                    double cpu = hist.empty() ? 0 : std::strtod(hist[0].s1.c_str(), nullptr);
                    res.body += " It has run " + (runs == 1 ? std::string("once") : unllm::number_word(runs) + " times") +
                                " since " + day_text(int64_t(hist[0].a)) +
                                (hist[0].b > 0 ? ", peaking at " + human_bytes(hist[0].b) + " of memory" : "") +
                                (cpu >= 60 ? ", with " + duration_text(int64_t(cpu)) + " of processor time" : "") + ".";
                } else if (f->category == "kernel") {
                    res.notes.push_back("Kernel threads are not recorded in the program history.");
                }
                res.receipts.push_back("note: " + f->pattern + "  (" + f->category +
                                       (f->source.empty() ? "" : ", " + f->source) + ")");
            } else if (runs > 0) {
                res.body = "I don't have a note on " + whatis + ". It has run " +
                           (runs == 1 ? std::string("once") : unllm::number_word(runs) + " times") +
                           " since " + day_text(int64_t(hist[0].a)) + ".";
                if (!exe.empty()) res.receipts.push_back("exe: " + exe);
            } else if (!r.entities.empty() && r.entities[0].type == "name") {
                res.body = "I don't know what " + whatis + " is: no note in the fact table, and it has "
                           "never run here in the 30 days of records.";
            } else {
                res.body.clear();
            }
            if (!res.body.empty()) {
                add_receipts("SELECT started,0,0,name || '  pid ' || pid FROM procs WHERE is_program(?, name, exe, cmdline) "
                             "ORDER BY started DESC LIMIT 4", {}, {whatis});
                return res;
            }
        }
    }

    // ---- judgment: is this normal, versus yesterday, trends, changes ----
    // Statistics over the hourly rollups; the raw seconds only provide the
    // "now" side. Every minimum below refuses by SHOWING what data exists.
    if (r.judge != unllm::Judge::None) {
        const char* metric = nullptr;
        const char* noun = nullptr;
        std::string sexpr, unitw;
        switch (lead) {
        case Intent::Heat: metric = "temp"; noun = "CPU temperature"; sexpr = "temp"; unitw = " C"; break;
        case Intent::Slow: metric = "cpu"; noun = "CPU use"; sexpr = "cpu"; unitw = "%"; break;
        case Intent::Memory: metric = "mem_pct"; noun = "memory use"; sexpr = "mem_pct"; unitw = "%"; break;
        case Intent::Disk: metric = "disk_rw"; noun = "disk traffic"; sexpr = "disk_r+disk_w"; break;
        case Intent::Network: metric = "net_rxtx"; noun = "network traffic"; sexpr = "net_rx+net_tx"; break;
        case Intent::Battery: metric = "power"; noun = "battery draw"; sexpr = "power"; unitw = " W"; break;
        default: break;
        }
        const bool sentinel = lead == Intent::Heat || lead == Intent::Battery;
        auto value_text = [&](double v) {
            return (lead == Intent::Disk || lead == Intent::Network) ? human_bytes(v) + "/s" : fmt("%.0f", v) + unitw;
        };
        // Daily medians over the last 30 days, from the hourly rollups.
        auto daily = [&]() {
            std::vector<std::pair<std::string, std::vector<double>>> days;
            auto rows = query(db_, "SELECT hour_ts, med, 0 FROM samples_hourly WHERE hour_ts >= ? AND metric = ? "
                                   "ORDER BY hour_ts",
                              {int64_t(now - 30 * 86400)}, {metric});
            for (const auto& row : rows) {
                std::string d = local_date(row.t);
                if (days.empty() || days.back().first != d) days.push_back({d, {}});
                days.back().second.push_back(row.a);
            }
            std::vector<std::pair<std::string, double>> out;
            for (auto& [d, vals] : days) out.push_back({d, judge::median(vals)});
            return out;
        };
        if (metric && r.judge == unllm::Judge::Normal && program.empty()) {  // a named program is judged by its own history, below
            // The "now" side: a 10-minute median of the raw seconds.
            std::vector<double> nowv;
            double ac_now = -1;
            for (const auto& row : query(db_, "SELECT t, " + sexpr + ", IFNULL(on_ac,-1) FROM samples WHERE t >= ?",
                                         {int64_t(now - 600)})) {
                if (!(sentinel && row.a < 0)) nowv.push_back(row.a);
                if (row.b >= 0) ac_now = row.b;
            }
            if (nowv.empty()) {
                res.body = "I have no readings from the last ten minutes to judge (is the recorder running?).";
                return res;
            }
            double nowval = judge::median(nowv);
            // The baseline: the same hour of day (plus or minus one) over 30
            // days, on the same power regime when that is known.
            int h = local_hour(int64_t(now));
            std::vector<double> pool;
            for (const auto& row : query(db_, "SELECT hour_ts, med, IFNULL(on_battery_frac,-1) FROM samples_hourly "
                                              "WHERE hour_ts >= ? AND metric = ?",
                                         {int64_t(now - 30 * 86400)}, {metric})) {
                int hh = local_hour(row.t);
                int d = std::abs(hh - h);
                if (std::min(d, 24 - d) > 1) continue;
                if (ac_now >= 0 && row.b >= 0 && (row.b > 0.5) != (ac_now == 0)) continue;
                pool.push_back(row.a);
            }
            if (pool.size() < 10) {
                std::string have;
                std::vector<double> sorted = pool;
                std::sort(sorted.begin(), sorted.end());
                for (double p : sorted) have += (have.empty() ? "" : ", ") + value_text(p);
                res.body = "Right now " + std::string(noun) + " is " + value_text(nowval) + ", but I only have " +
                           unllm::number_word(int64_t(pool.size())) + " comparable " +
                           unllm::plural_noun("hour", int64_t(pool.size())) + " on record so far" +
                           (have.empty() ? "" : " (" + have + ")") +
                           "; I need ten before calling anything normal. Ask again in a few days.";
                res.receipts.push_back("baseline: same hour of day over 30 days, " + std::to_string(pool.size()) + " hours");
                return res;
            }
            double rank = judge::percentile_rank(pool, nowval);
            double typical = judge::median(pool);
            double lo = judge::percentile(pool, 0.05), hi = judge::percentile(pool, 0.95);
            bool usual = rank >= 5 && rank <= 95;
            std::string word = rank > 99 ? " That is anomalous for this machine."
                               : rank > 95 ? " That is unusual for this machine."
                                           : "";
            // "is it normal" is yes inside the usual range; "is it a lot" is yes only above it.
            const bool yes = r.excess ? rank > 95 : usual;
            res.lead = yes ? "Yes:" : "No:";
            res.body = std::string(noun) + " is " + value_text(nowval) +
                       ", the " + ordinal(int(rank + 0.5)) + " percentile for this hour of day. Typical is " +
                       value_text(typical) + "; the usual range is " + value_text(lo) + " to " + value_text(hi) +
                       "." + word;
            res.receipts.push_back("baseline: " + std::to_string(pool.size()) + " comparable hours over 30 days" +
                                   (ac_now >= 0 ? ", same power regime" : ""));
            res.receipts.push_back("now: median of the last 10 minutes (" + std::to_string(nowv.size()) + " readings)");
            return res;
        }
        // Two times set against each other ("hotter yesterday morning or last night", "hotter yesterday than last
        // night"): each window's average and peak, from the per-second readings or the hourly ones beyond them.
        if (metric && r.judge == unllm::Judge::Compare && r.second_window && program.empty()) {
            struct Side {
                int64_t from, to;
                double avg = 0, peak = 0, fan = -1;
                int64_t n = 0;
                int64_t hourly_to = 0;  // up to here from the hourly records; 0 for none
            };
            // The per-second readings are kept a day or so: the hours of a side before the first hour they fill come
            // from the hourly records (each hour's median weighted by its readings, and its highest).
            auto kept_from = query(db_, "SELECT CAST(MIN(t) AS INTEGER) FROM samples", {});
            const int64_t detail = kept_from.empty() || kept_from[0].t == 0 ? INT64_MAX : kept_from[0].t;
            auto side = [&](const unllm::TimeWindow& w) {
                Side s{w.from, w.to};
                const int64_t cut = w.from < detail ? std::min(w.to, detail == INT64_MAX ? w.to : (detail + 3599) / 3600 * 3600) : w.from;
                double sum = 0;
                if (cut > w.from) {
                    auto h = query(db_, "SELECT SUM(n), SUM(med * n), MAX(max) FROM samples_hourly WHERE hour_ts >= ?1 AND hour_ts < ?2 AND metric = ?3",
                                   {w.from, cut}, {metric});
                    if (!h.empty() && h[0].t > 0) s.n = h[0].t, sum = h[0].a, s.peak = h[0].b, s.hourly_to = cut;
                    if (lead == Intent::Heat)
                        if (auto f = query(db_, "SELECT COUNT(*), MAX(max) FROM samples_hourly WHERE hour_ts >= ?1 AND hour_ts < ?2 AND metric = 'fan'",
                                           {w.from, cut});
                            !f.empty() && f[0].t > 0)
                            s.fan = f[0].a;
                }
                auto rows = query(db_, "SELECT COUNT(*), SUM(" + sexpr + "), MAX(" + sexpr + ") FROM samples WHERE t >= ?1 AND t < ?2" +
                                           (sentinel ? " AND " + sexpr + " >= 0" : std::string()), {cut, w.to});
                if (!rows.empty() && rows[0].t > 0) {
                    s.peak = s.n > 0 ? std::max(s.peak, rows[0].b) : rows[0].b;
                    s.n += rows[0].t, sum += rows[0].a;
                }
                if (s.n > 0) s.avg = sum / double(s.n);
                if (lead == Intent::Heat) {
                    auto f = query(db_, "SELECT COUNT(*), MAX(fan) FROM samples WHERE t >= ?1 AND t < ?2 AND fan >= 0", {cut, w.to});
                    if (!f.empty() && f[0].t > 0) s.fan = std::max(s.fan, f[0].a);
                }
                return s;
            };
            const Side one = side(r.window), two = side(*r.second_window);
            auto span = [](const Side& s) { return when_text(s.from) + " to " + when_text(s.to); };
            if (one.n == 0 || two.n == 0) {
                const Side& none = one.n == 0 ? one : two;
                res.body = "I can't compare them: there are no " + std::string(noun) + " readings for " + span(none) + ".";
                return res;
            }
            const bool first = one.avg >= two.avg;
            const Side& hi = first ? one : two;
            const Side& lo = first ? two : one;
            const char* more = lead == Intent::Heat ? "hotter" : "higher";
            const bool split = lo.peak > hi.peak && value_text(lo.peak) != value_text(hi.peak);
            if (split) {
                // The averages and the peaks disagree: both said, each with its measure.
                res.body = span(lo) + " peaked " + more + ": " + noun + " reached " + value_text(lo.peak) + " against " + value_text(hi.peak) +
                           " for " + span(hi) + "; " +
                           (value_text(hi.avg) == value_text(lo.avg) ? "on average the two were about the same, " + value_text(hi.avg)
                                                                     : "on average " + span(hi) + " ran " + more + ", " + value_text(hi.avg) + " against " +
                                                                           value_text(lo.avg)) +
                           ".";
            } else {
                res.body = span(hi) + " ran " + more + ": " + noun + " averaged " + value_text(hi.avg) + " and peaked at " + value_text(hi.peak) +
                           ", against an average of " + value_text(lo.avg) + " and a peak of " + value_text(lo.peak) + " for " + span(lo) + ".";
            }
            const Side& said_first = split ? lo : hi;
            const Side& said_next = split ? hi : lo;
            if (hi.fan >= 0 && lo.fan >= 0)
                res.body += " The fan peaked at " + fmt("%.0f%%", said_first.fan) + " against " + fmt("%.0f%%", said_next.fan) + ".";
            for (const Side* x : {&one, &two})
                if (x->hourly_to)
                    res.notes.push_back("For " + span(*x) + ", the hours before " + when_text(x->hourly_to) +
                                        " come from the hourly records (each hour's median and highest), as the per-second readings start at " +
                                        when_text(detail) + ".");
            // "hotter yesterday than last night" asks about the first; "this or that" asks which.
            if (r.compare_window) res.lead = first ? "Yes." : "No.";
            res.receipts.push_back(span(one) + "  " + std::to_string(one.n) + " readings");
            res.receipts.push_back(span(two) + "  " + std::to_string(two.n) + " readings");
            res.branch = "compare:" + std::string(unllm::intent_name(lead));
            return res;
        }
        if (metric && r.judge == unllm::Judge::Compare && r.compare_window) {
            // Paired hours: this window's hourly medians against the same
            // hours of day in the referent window.
            auto side = [&](int64_t a, int64_t b) {
                std::vector<std::pair<int, double>> out;
                for (const auto& row : query(db_, "SELECT hour_ts, med, 0 FROM samples_hourly WHERE hour_ts >= ? "
                                                  "AND hour_ts < ? AND metric = ? ORDER BY hour_ts",
                                             {a, b}, {metric}))
                    out.push_back({local_hour(row.t), row.a});
                return out;
            };
            auto cur = side(int64_t(now) - 86400, int64_t(now));
            auto ref = side(r.compare_window->from, r.compare_window->to);
            std::vector<double> diffs;
            int wins = 0, pairs = 0;
            double biggest = 0;
            int biggest_hour = -1;
            for (const auto& [h, v] : cur)
                for (const auto& [rh, rv] : ref)
                    if (h == rh) {
                        ++pairs;
                        diffs.push_back(v - rv);
                        if (v > rv) ++wins;
                        if (std::abs(v - rv) > std::abs(biggest)) { biggest = v - rv; biggest_hour = h; }
                        break;
                    }
            if (pairs < 4) {
                res.body = "Only " + unllm::number_word(pairs) + " overlapping " +
                           unllm::plural_noun("hour", pairs) + " of data between the two windows; I need four "
                           "to compare them honestly.";
                return res;
            }
            double gap = judge::median(diffs);
            const char* dir = gap > 0 ? (lead == Intent::Heat ? "hotter" : "higher")
                                      : (lead == Intent::Heat ? "cooler" : "lower");
            std::string strength = (pairs >= 6 && (wins <= 1 || wins >= pairs - 1)) ? "clearly " : "";
            if (std::abs(gap) < 1e-9) {
                res.body = std::string(noun) + " is about the same: no median gap across " +
                           unllm::number_word(pairs) + " overlapping hours.";
            } else {
                res.body = std::string(noun) + " runs " + strength + value_text(std::abs(gap)) + " " + dir +
                           " at the same hours: " + dir + " in " + unllm::number_word(gap > 0 ? wins : pairs - wins) +
                           " of " + unllm::number_word(pairs) + " overlapping hours.";
            }
            if (polar) res.lead = gap > 0 ? "Yes." : "No.";
            if (biggest_hour >= 0)
                res.receipts.push_back(fmt("biggest gap at %02.0f:00  ", double(biggest_hour)) + value_text(std::abs(biggest)));
            res.receipts.push_back("paired hourly medians, this window vs the named one");
            return res;
        }
        if (metric && r.judge == unllm::Judge::Trend) {
            auto days = daily();
            if (days.size() < 10) {
                res.body = "A trend needs at least ten days of records and I have " +
                           unllm::number_word(int64_t(days.size())) + " so far; ask again in a few days.";
                return res;
            }
            std::vector<double> series;
            for (const auto& [d, v] : days) series.push_back(v);
            // A single step reads as a trend to a slope test; check for the
            // step first and report the date instead.
            auto ch = judge::cusum(series);
            if (ch.found && ch.shift_units >= 3) {
                res.body = std::string(noun) + " changed on " + days[ch.index].first + ": the typical level moved by " +
                           fmt("%.1f", ch.shift_units) + " times the normal day-to-day variation. That is a step, "
                           "not a gradual trend.";
                res.receipts.push_back("daily medians, CUSUM change detection over " + std::to_string(days.size()) + " days");
                return res;
            }
            auto slope_text = [&](double v) {
                return (lead == Intent::Disk || lead == Intent::Network) ? human_bytes(v) + "/s" : fmt("%.1f", v) + unitw;
            };
            auto tr = judge::theil_sen_mk(series);
            if (tr.significant) {
                if (polar) res.lead = "Yes:";
                res.body = std::string(noun) + " has been " +
                           (tr.slope_per_step > 0 ? "rising" : "falling") + " about " +
                           slope_text(std::abs(tr.slope_per_step)) + " per day over " +
                           unllm::number_word(int64_t(days.size())) + " days (first week typical " +
                           value_text(judge::median({series.begin(), series.begin() + 7})) + ", last week " +
                           value_text(judge::median({series.end() - 7, series.end()})) + ").";
            } else {
                res.body = "No clear trend: the slope is " + slope_text(std::abs(tr.slope_per_step)) +
                           " per day, but across " + unllm::number_word(int64_t(days.size())) +
                           " days the data does not separate that from noise.";
            }
            res.receipts.push_back("daily medians over " + std::to_string(days.size()) + " days, Theil-Sen slope, Mann-Kendall test");
            return res;
        }
        if (metric && r.judge == unllm::Judge::Changed && r.compare_window) {
            int64_t split = r.compare_window->from;
            auto days = daily();
            std::vector<double> before, after;
            for (const auto& [d, v] : days)
                (d < local_date(split) ? before : after).push_back(v);
            if (before.size() < 5 || after.size() < 5) {
                res.body = "Comparing across that date needs five days of records on each side; I have " +
                           unllm::number_word(int64_t(before.size())) + " before and " +
                           unllm::number_word(int64_t(after.size())) + " after.";
                return res;
            }
            double mb = judge::median(before), ma = judge::median(after);
            double sr = judge::mad_sigma(before);
            double units = sr > 0 ? std::abs(ma - mb) / sr : 0;
            std::string verdict = sr <= 0 ? "the level before was essentially constant, so any movement stands out: "
                                : units >= 3 ? "that is a real change."
                                : units >= 1.5 ? "that is somewhat different, within the louder end of normal variation."
                                               : "that is no clear change.";
            res.body = "Since " + day_text(split) + ", " + std::string(noun) + " runs at a typical " + value_text(ma) +
                       " versus " + value_text(mb) + " before" +
                       (sr > 0 ? ", " + fmt("%.1f", units) + " times the normal day-to-day variation: " : ": ") + verdict;
            if (polar) res.lead = units >= 3 ? "Yes." : "No.";
            res.receipts.push_back("daily medians, " + std::to_string(before.size()) + " days before vs " +
                                   std::to_string(after.size()) + " after");
            return res;
        }
        // No metric topic (or no referent): fall through to normal routing.
    }

    // ---- live: measured this second, not read from the records ----
    // "Am i on battery", "how much battery life do i have left", "how long
    // will the battery last": the battery is measured, not recalled.
    // ("when did it last wake" is a history question, never a live one.)
    bool live_battery = lead == Intent::Battery && !r.window.stated && !when_frame &&
                        (polar || r.wants == unllm::Wants::Duration ||
                         has_word(tokens, {"left", "remaining", "remain", "life"}));
    // present: no time named and present tense, unless the question asks when or
    // describes a condition ("how hot does it get when blender is running"): those are history.
    const bool present_now = r.window.present && !when_frame && r.frame != unllm::Frame::Condition;
    // Only topics with a live reading go live; trust, crashes, config, sharing,
    // devices and startup answer from their own records even when asked about now.
    const bool has_live = lead == Intent::Heat || lead == Intent::Gpu || lead == Intent::Memory ||
                          lead == Intent::Battery || lead == Intent::Disk || lead == Intent::Network ||
                          lead == Intent::Slow || lead == Intent::Story;
    if (has_live && (r.window.live || present_now || live_battery)) {
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        SnapshotPtr s = sampler_->sample();
        // Rates need two looks: the first call has nothing to compare with. For the disk the second scans the processes,
        // whose own disk counters it starts when the disks are busy.
        if (lead == Intent::Slow || lead == Intent::Disk || lead == Intent::Network || lead == Intent::Story ||
            lead == Intent::Gpu) {
            usleep(600 * 1000);
            s = sampler_->sample(lead == Intent::Disk);
        }
        res.branch = std::string("live:") + (lead == Intent::Story ? "slow" : unllm::intent_name(lead));
        card_.moment = now;
        std::vector<Clause> clauses;
        // A named program ("is ollama using the gpu", "is firefox using too much cpu"):
        // that program's own share, or that it isn't running.
        if (!program.empty() && (lead == Intent::Gpu || lead == Intent::Memory || lead == Intent::Slow ||
                                 lead == Intent::Heat || lead == Intent::Story)) {
            double cpu = 0, rss = 0, vram = 0;
            int n = 0;
            for (const auto& p : s->procs)
                if (is_program(program, p.name, p.exe, p.cmdline)) {
                    ++n;
                    cpu += p.cpu;
                    rss += double(p.rss);
                    vram += p.gpu_mem.or_else(0);
                }
            res.branch += ":program";
            card_.subject = program;
            card_.subjects_named = {program};
            if (n == 0) {
                res.body = program + " isn't running right now." + last_run_text(db_, program, now);
            } else if (lead == Intent::Gpu) {
                res.body = vram > 0 ? program + " is using " + human_bytes(vram) + " of video memory on the GPU right now."
                                    : program + " is running but has nothing on the GPU right now.";
            } else if (lead == Intent::Memory) {
                res.body = program + " holds " + human_bytes(rss) + " of memory right now" +
                           (s->mem.total > 0 ? fmt(", %.0f%% of the machine's", 100.0 * rss / double(s->mem.total)) : std::string()) + ".";
            } else {
                res.body = program + " is using " + fmt("%.0f", cpu) + "% of the CPU right now" +
                           fmt(" (the whole machine is at %.0f%%)", s->cpu.total) + ".";
            }
            if (n > 1) res.body += " That's " + std::to_string(n) + " processes together.";
            // "Why is firefox slow": its memory too, and whether the machine as a whole is short of memory or swapping.
            if (n > 0 && r.wants == unllm::Wants::Cause && (lead == Intent::Slow || lead == Intent::Story)) {
                res.body += " It holds " + human_bytes(rss) + " of memory" +
                            (s->mem.percent() >= 85 ? fmt(", and the machine's memory is at %.0f%%, which slows everything", s->mem.percent())
                             : s->mem.swap_total > 0 && s->mem.swap_total - s->mem.swap_free > s->mem.swap_total / 2
                                 ? ", and half the swap is in use, which slows programs whose pages were pushed out"
                                 : "") +
                            ".";
            }
            if (polar) res.lead = n > 0 ? "Yes." : "No.";
            // "whats that node thing": what the processes are, longest running first: the script an interpreter runs
            // (else the command), the unit it runs in and since when.
            if (n > 0 && !polar && r.judge == unllm::Judge::None && r.wants != unllm::Wants::Quantity) {
                std::vector<const ProcessInfo*> mine;
                for (const auto& p : s->procs)
                    if (is_program(program, p.name, p.exe, p.cmdline)) mine.push_back(&p);
                std::sort(mine.begin(), mine.end(), [](auto a, auto b) { return a->started_epoch < b->started_epoch; });
                std::vector<std::string> what;
                for (const ProcessInfo* p : mine) {
                    if (what.size() == 4) break;
                    const Fact* f = facts_.lookup(p->name);
                    if (!f || f->category != "runtime") f = facts_.lookup(p->exe.substr(p->exe.rfind('/') + 1));
                    std::string it = p->cmdline.substr(0, 60);
                    if (f && f->category == "runtime") {
                        // The script: the first word after the interpreter that is not an option, or the command's
                        // own name when it was started as a script ("npm exec ...", "pyright-langserver --stdio").
                        std::istringstream words(p->cmdline);
                        std::string w;
                        words >> w;
                        const std::string first = w.substr(w.rfind('/') + 1);
                        if (is_program(first, p->name, p->exe, "")) {
                            while (words >> w && w[0] == '-') continue;
                            if (!w.empty() && w[0] != '-') it = w.substr(w.rfind('/') + 1);
                        } else if (!first.empty()) {
                            it = first;
                        }
                    }
                    it += " (PID " + std::to_string(p->key.pid) + (p->unit.empty() ? "" : ", in " + fold_unit(p->unit)) +
                          (p->started_epoch > 0 ? ", since " + when_text(int64_t(p->started_epoch)) : "") +
                          (p->cpu_seconds >= 60 ? ", " + duration_text(int64_t(p->cpu_seconds)) + " of CPU so far" : "") + ")";
                    what.push_back(it);
                }
                if (mine.size() > what.size()) what.push_back(unllm::number_word(long(mine.size() - what.size())) + " more");
                res.body = unllm::realize({{{program, true}, "run", unllm::join_list(what), Tense::Present}}) + " " + res.body;
            }
            // "is firefox using too much cpu": against its own minutes in the records (the minutes it was
            // among the busiest programs, so its typical figure there runs high, not low).
            if (n > 0 && r.judge == unllm::Judge::Normal && (lead == Intent::Slow || lead == Intent::Memory || lead == Intent::Story)) {
                const bool mem = lead == Intent::Memory;
                const double cores = std::max(1, s->cpu.logical);
                std::vector<double> hist;
                for (const auto& row : query(db_, "SELECT minute, SUM(cpu_seconds), SUM(peak_rss) FROM minute_top WHERE minute >= ? AND is_program(?, name, '', cmdline) GROUP BY minute",
                                             {now - 30 * 86400}, {program}))
                    hist.push_back(mem ? row.b : 100.0 * row.a / (60.0 * cores));
                const double nowv = mem ? rss : cpu;
                auto show = [&](double v) { return mem ? human_bytes(v) : fmt("%.0f%%", v); };
                if (hist.size() >= 10) {
                    const double typical = judge::median(hist), hi = judge::percentile(hist, 0.95);
                    const bool above = nowv > hi;
                    const bool yes = r.excess ? above : !above;
                    res.lead = yes ? "Yes:" : "No:";
                    res.body += " In the " + std::to_string(hist.size()) +
                                " recorded minutes when it was among the busiest programs, it typically used " + show(typical) +
                                " and rarely more than " + show(hi) + (above ? ", so this is high for it." : ", so this is within its usual.");
                } else {
                    res.notes.push_back("I have too few records of " + program + " to say what is usual for it.");
                }
            }
            return res;
        }
        switch (lead) {
        case Intent::Heat: {
            double t = s->cpu.temp_c.or_else(s->guard.temp.or_else(-1));
            // Whether that is hot: against the level the recorder counts as hot (a yes or no says it in front instead).
            const double hot = Triggers().temp;
            clauses.push_back({{"the CPU", true}, "be", fmt("at %.0f C right now", t) + (polar || t < 0 ? "" : t >= hot ? ", which is hot" : ", which is not hot"),
                               Tense::Present});
            for (const auto& f : s->power.fans)
                if (f.percent.ok()) {
                    clauses.push_back({{"the fan", true}, "be", fmt("at %.0f%%", double(f.percent.get())), Tense::Present});
                    break;
                }
            if (s->guard.margin.ok() && s->guard.margin.get() >= 0 && s->guard.margin.get() < 15)
                clauses.push_back({{"it", true}, "be", fmt("within %.0f C of the shutdown point", s->guard.margin.get()), Tense::Present});
            for (const auto& th : s->thermals)
                if (res.receipts.size() < 6)
                    res.receipts.push_back(th.chip + " " + th.label + "  " + fmt("%.0f C", th.celsius));
            // A why: what is working the machine, and any fan that stays still while it is hot.
            if (r.wants == unllm::Wants::Cause) {
                auto procs = s->procs;
                std::sort(procs.begin(), procs.end(), [](const auto& a, const auto& b) { return a.cpu > b.cpu; });
                if (!procs.empty() && procs[0].cpu >= 3)
                    clauses.push_back({{procs[0].name, true}, "be", fmt("the biggest load at %.0f%% of the CPU", procs[0].cpu), Tense::Present});
                else
                    clauses.push_back({{"no program", true}, "be", "working the CPU hard", Tense::Present});
                for (const auto& f : s->guard.dead_fans)
                    clauses.push_back({{"the fan '" + f + "'", true}, "be", "not spinning while the machine is warm, which traps the heat", Tense::Present});
                // Not hot now: the last time it was, from the recorder's events, and what was working it then.
                if (t >= 0 && t < hot) {
                    auto last = query(db_, "SELECT CAST(start AS INTEGER), peak_temp, 0, summary FROM events WHERE peak_temp >= ?1 ORDER BY start DESC LIMIT 1",
                                      {int64_t(hot)});
                    if (!last.empty()) {
                        const std::string who = top_culprit(db_, last[0].t - 60, last[0].t + 1800, KindHeat | KindCpu, "");
                        clauses.push_back({{"the last hot spell", true}, "be", "on " + moment_text(last[0].t) + fmt(", when the CPU reached %.0f C", last[0].a) +
                                                                                 (who.empty() ? "" : ", mostly from " + who), Tense::Past});
                        res.receipts.push_back(when_text(last[0].t) + "  " + last[0].s1.substr(0, 120));
                    }
                }
            }
            card_.metric = "temperature";
            card_.unit = "C";
            card_.value = t;
            card_.subject = "the CPU";
            break;
        }
        case Intent::Gpu: {
            if (s->gpus.empty()) {
                clauses.push_back({{"no graphics card", true}, "be", "visible to the recorder", Tense::Present});
                break;
            }
            // "What percentage of VRAM is in use": the video memory first, as a share, and its receipts are the memory.
            if (kind_ == "gpu.value.now.vram") {
                double total = 0;
                bool full = false;
                for (const auto& g : s->gpus) {
                    const std::string name = g.name.empty() ? "the GPU" : g.name;
                    if (g.mem_used.ok() && g.mem_total.ok() && g.mem_total.get() > 0) {
                        const double pct = 100.0 * g.mem_used.get() / g.mem_total.get();
                        total = std::max(total, double(g.mem_total.get()));
                        full = full || pct >= 90;
                        clauses.push_back({{name, true}, "have", fmt("%.0f%% of its video memory in use: ", pct) + human_bytes(g.mem_used.get()) +
                                                                     " of " + human_bytes(g.mem_total.get()) +
                                                                     (pct >= 90 ? ", which is nearly full" : pct >= 70 ? ", which is well used" : ", so it is not full"),
                                           Tense::Present});
                        res.receipts.push_back(name + "  " + human_bytes(g.mem_used.get()) + " of " + human_bytes(g.mem_total.get()) + " video memory");
                    } else {
                        res.receipts.push_back(name + "  no video memory figure");
                    }
                }
                if (clauses.empty()) clauses.push_back({{"no graphics card", true}, "report", "its video memory", Tense::Present});
                // Who holds it now, by program, and the fullest it has been in the last week when it is not full now.
                std::map<std::string, double> by;
                for (const auto& p : s->procs)
                    if (p.gpu_mem.ok() && p.gpu_mem.get() > 0) by[p.name] += p.gpu_mem.get();
                std::vector<std::pair<std::string, double>> holders(by.begin(), by.end());
                std::sort(holders.begin(), holders.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                if (!holders.empty()) {
                    std::vector<std::string> next;
                    for (std::size_t i = 1; i < holders.size() && i < 3; ++i) next.push_back(holders[i].first + " " + human_bytes(holders[i].second));
                    clauses.push_back({{holders[0].first, true}, "hold", "the most of it, " + human_bytes(holders[0].second) +
                                                                            (next.empty() ? "" : ", then " + unllm::join_list(next)), Tense::Present});
                    card_.subjects_named = {holders[0].first};
                } else if (total > 0) {
                    clauses.push_back({{"no program", true}, "have", "video memory of its own on it: the rest is the driver's and the desktop's", Tense::Present});
                }
                if (!full && total > 0) {
                    auto hi = query(db_, "SELECT CAST(t AS INTEGER), gpu_mem FROM samples WHERE gpu_mem IS NOT NULL AND t >= ?1 ORDER BY gpu_mem DESC LIMIT 1",
                                    {now - 7 * 86400});
                    if (!hi.empty() && hi[0].a >= 0.5 * total) {
                        const std::string who = top_culprit(db_, hi[0].t - 600, hi[0].t + 600, KindGpu, "");
                        clauses.push_back({{"the fullest it has been in the last week", true}, "be",
                                           human_bytes(hi[0].a) + " at " + when_text(hi[0].t) + (who.empty() ? "" : ", with " + who + " on it"), Tense::Past});
                    }
                }
                if (polar) res.lead = full ? "Yes." : "No.";
                card_.subject = "the GPU";
                break;
            }
            for (const auto& g : s->gpus) {
                std::string name = g.name.empty() ? "the GPU" : g.name;
                if (g.asleep) {
                    clauses.push_back({{name, true}, "be", "asleep right now (powered down because nothing needs it), so it "
                                                           "is idle", Tense::Present});
                } else {
                    std::string rest = fmt("at %.0f%% load", g.util.or_else(0));
                    if (g.mem_used.ok() && g.mem_total.ok() && g.mem_total.get() > 0)
                        rest += ", with " + human_bytes(g.mem_used.get()) + " of " + human_bytes(g.mem_total.get()) +
                                " video memory in use";
                    if (g.temp_c.ok()) rest += fmt(", at %.0f C", g.temp_c.get());
                    clauses.push_back({{name, true}, "be", rest, Tense::Present});
                }
                res.receipts.push_back(name + (g.asleep ? "  asleep" : "  " + fmt("%.0f%% load", g.util.or_else(0))));
            }
            card_.subject = "the GPU";
            break;
        }
        case Intent::Memory: {
            clauses.push_back({{"memory use", true}, "be",
                               fmt("at %.0f%%", s->mem.percent()) + ", with " + human_bytes(double(s->mem.available)) + " free",
                               Tense::Present});
            // The biggest holders, ask's own process aside (it holds the sentence model only while it answers).
            std::vector<ProcessInfo> procs;
            for (const auto& p : s->procs)
                if (!p.self) procs.push_back(p);
            std::sort(procs.begin(), procs.end(), [](const auto& a, const auto& b) { return a.rss > b.rss; });
            if (!procs.empty() && procs[0].rss > 0) {
                std::vector<std::string> next;
                for (std::size_t i = 1; i < procs.size() && i < 3; ++i)
                    if (procs[i].rss > 0) next.push_back(procs[i].name + " " + human_bytes(double(procs[i].rss)));
                clauses.push_back({{procs[0].name, true}, "hold", "the most, " + human_bytes(double(procs[0].rss)) +
                                                                  (next.empty() ? "" : ", then " + unllm::join_list(next)), Tense::Present});
                card_.subject = procs[0].name;
                card_.subjects_named.clear();
                for (std::size_t i = 0; i < procs.size() && i < 3; ++i)
                    if (procs[i].rss > 0) card_.subjects_named.push_back(procs[i].name);  // "which one": these, in order
                card_.metric = "memory";
                card_.value = double(procs[0].rss);
            }
            for (const auto& p : procs)
                if (res.receipts.size() < 5 && p.rss > 0)
                    res.receipts.push_back(p.name + "  " + human_bytes(double(p.rss)));
            // The swap, and for a why or a "so much": the day's peak and what drove it, from the per-second readings.
            if (s->mem.swap_total > 0 && s->mem.swap_total > s->mem.swap_free)
                clauses.push_back({{"swap", true}, "hold", "another " + human_bytes(double(s->mem.swap_total - s->mem.swap_free)), Tense::Present});
            if (r.wants == unllm::Wants::Cause || r.excess) {
                auto mem = series(db_, "mem_pct", "mem_pct", now - 86400, now);
                const Row* hi = nullptr;
                for (const auto& x : mem)
                    if (!hi || x.a > hi->a) hi = &x;
                if (hi) {
                    const std::string who = top_culprit(db_, hi->t - 600, hi->t + 600, KindMemory, "");
                    clauses.push_back({{"the day's peak", true}, "be", fmt("%.0f%% at ", hi->a) + when_text(hi->t) + (who.empty() ? "" : ", mostly from " + who),
                                       Tense::Past});
                }
            }
            break;
        }
        case Intent::Battery: {
            bool on_ac = s->power.on_ac.or_else(true);
            double pct = s->power.battery_pct.or_else(-1);
            std::string state = pct >= 0 ? fmt("at %.0f%%", pct) : "";
            const bool charging = s->power.battery_state == "Charging";
            std::string minutes;
            if (s->power.battery_minutes.ok())
                minutes = duration_text(int64_t(s->power.battery_minutes.get() * 60)) +
                          (charging ? " to full" : " left at this rate");
            if (on_ac) {
                Clause c{{"it", true}, "be", "plugged in" + (state.empty() ? "" : ", " + state) +
                                                 (charging && !minutes.empty() ? ", about " + minutes : ""),
                         Tense::Present};
                res.body = unllm::realize({c});
            } else {
                double w = s->power.battery_w.or_else(-1);
                std::string rest = "on battery" + (state.empty() ? "" : ", " + state);
                if (w > 0) rest += fmt(", drawing %.0f W", w);
                if (!minutes.empty()) rest += ": about " + minutes;
                Clause c{{"it", true}, "be", rest, Tense::Present};
                res.body = unllm::realize({c});
            }
            // A yes or no is about one side, on battery or on the charger: the one the question is nearer by meaning, by
            // the recorder's titles for it and an everyday sentence ("is my laptop plugged in" asks about the charger).
            if (polar) {
                const int side = asked_test(r, {{"Unplugged from power", "it is running on battery"},
                                                {"Plugged into power", "it is plugged into the charger"}});
                if (side < 0) res.undecided = true;
                else res.lead = (side == 0 ? !on_ac : on_ac) ? "Yes." : "No.";
            }
            // A worn battery is named before any software gets blamed.
            if (s->power.battery_health.ok() && s->power.battery_health.get() < 80)
                res.body += " The battery holds only " + fmt("%.0f%%", s->power.battery_health.get()) +
                            " of its design capacity: shorter runtimes are the battery aging, not software.";
            res.receipts.push_back("battery: " + s->power.battery_state +
                                   (pct >= 0 ? "  " + fmt("%.0f%%", pct) : ""));
            return res;
        }
        case Intent::Disk: {
            double rd = 0, wr = 0;
            for (const auto& d : s->disks) { rd += d.read_bps; wr += d.write_bps; }
            // Who is doing it: each process's own counters, read only for my own processes and only in a process scan
            // while the disks move over 512 KB/s (Sampler::sample); the second look above was such a scan and started
            // them, one more gives their rates.
            const bool counted = rd + wr > 512e3;
            if (counted) {
                usleep(600 * 1000);
                s = sampler_->sample(true);
                rd = wr = 0;
                for (const auto& d : s->disks) { rd += d.read_bps; wr += d.write_bps; }
            }
            const NP disks{"the disks", true, -1, "", true};
            if (rd + wr < 1e5)
                clauses.push_back({disks, "be", "quiet right now", Tense::Present});
            else
                clauses.push_back({disks, "be",
                                   "reading " + human_bytes(rd) + "/s and writing " + human_bytes(wr) + "/s", Tense::Present});
            if (counted && rd + wr > 512e3) {
                std::vector<const ProcessInfo*> busy;
                for (const auto& p : s->procs)
                    if (!p.self && p.read_bps.ok() && p.read_bps.get() + p.write_bps.or_else(0) >= 1e4) busy.push_back(&p);
                std::sort(busy.begin(), busy.end(), [](auto a, auto b) {
                    return a->read_bps.get() + a->write_bps.or_else(0) > b->read_bps.get() + b->write_bps.or_else(0);
                });
                auto rates = [](const ProcessInfo* p) {
                    return "reading " + human_bytes(p->read_bps.get()) + "/s and writing " + human_bytes(p->write_bps.or_else(0)) + "/s";
                };
                if (busy.empty()) {
                    clauses.push_back({{"none of your own programs", true}, "be", "reading or writing much, so it is the system's own work, and the "
                                                                                   "system's programs' disk use can't be read without administrator rights",
                                       Tense::Present});
                } else {
                    std::vector<std::string> next;
                    for (std::size_t i = 1; i < busy.size() && next.size() < 2; ++i) next.push_back(busy[i]->name + " (" + rates(busy[i]) + ")");
                    clauses.push_back({{busy[0]->name, true}, "be", "doing the most, " + rates(busy[0]) + (next.empty() ? "" : "; then " + unllm::join_list(next)),
                                       Tense::Present});
                    card_.subject = busy[0]->name;
                    card_.subjects_named = {busy[0]->name};
                }
            }
            for (const auto& d : s->disks)
                if (res.receipts.size() < 5)
                    res.receipts.push_back(d.name + "  read " + human_bytes(d.read_bps) + "/s  write " + human_bytes(d.write_bps) + "/s");
            // "Disk usage" means how busy to some and how full to others: both are given.
            for (const auto& f : real_filesystems())
                if (f.mount == "/") {
                    res.notes.push_back(space_line(f) + ".");
                    break;
                }
            break;
        }
        case Intent::Network: {
            // "what's using my bandwidth atm": how much each program moves isn't measured; who is connected is, beside the
            // machine's own rates.
            if (const QuestionKind* qk = kinds_.find(kind_); r.asks_doer || r.wants == unllm::Wants::Name || (qk && qk->form == "which thing"))
                return answer_records("network.which.now", r, now);
            double rx = 0, tx = 0;
            for (const auto& n : s->nets)
                if (n.kind != "Loopback") { rx += n.rx_bps; tx += n.tx_bps; }
            {
                std::vector<std::string> up;  // the links that are connected, so "am i online" gets its answer
                for (const auto& n : s->nets)
                    if (n.up && n.kind != "Loopback" && n.kind != "Virtual") up.push_back(n.name);
                if (up.empty())
                    clauses.push_back({{"no network link", true}, "be", "up right now", Tense::Present});
                else
                    clauses.push_back({{"it", true}, "be", "online through " + unllm::join_list(up), Tense::Present});
            }
            clauses.push_back({{"the network", true}, "be",
                               "moving " + human_bytes(rx) + "/s down and " + human_bytes(tx) + "/s up", Tense::Present});
            for (const auto& n : s->nets)
                if (n.up && n.kind != "Loopback" && res.receipts.size() < 5)
                    res.receipts.push_back(n.name + "  down " + human_bytes(n.rx_bps) + "/s  up " + human_bytes(n.tx_bps) + "/s");
            break;
        }
        default: {  // Slow, and the plain "what is happening right now"
            clauses.push_back({{"the CPU", true}, "be", fmt("at %.0f%% right now", s->cpu.total), Tense::Present});
            auto procs = s->procs;
            std::sort(procs.begin(), procs.end(), [](const auto& a, const auto& b) { return a.cpu > b.cpu; });
            if (!procs.empty() && procs[0].cpu >= 3) {
                clauses.push_back({{procs[0].name, true}, "be", fmt("the biggest user at %.0f%%", procs[0].cpu), Tense::Present});
                card_.subject = procs[0].name;
                card_.subjects_named = {procs[0].name};
            } else {
                clauses.push_back({{"nothing", true}, "be", "working hard", Tense::Present});
            }
            // A why about slowness: memory and swap too, the other usual reasons.
            if (r.wants == unllm::Wants::Cause && s->mem.total > 0) {
                const double used = 100.0 * double(s->mem.total - s->mem.available) / double(s->mem.total);
                std::string m = fmt("at %.0f%%", used);
                if (s->mem.swap_total > 0)
                    m += ", with " + human_bytes(double(s->mem.swap_total - s->mem.swap_free)) + " in swap";
                clauses.push_back({{"memory", true}, "be", m, Tense::Present});
            }
            card_.metric = "cpu";
            card_.value = s->cpu.total;
            for (const auto& p : procs)
                if (res.receipts.size() < 5 && p.cpu >= 0.5)
                    res.receipts.push_back(p.name + "  " + fmt("%.1f%% cpu", p.cpu) + "  " + human_bytes(double(p.rss)));
            break;
        }
        }
        res.body = unllm::realize(clauses);
        if (polar && lead == Intent::Heat) {
            double thr = quantity_threshold(r, "C");
            double t = s->cpu.temp_c.or_else(s->guard.temp.or_else(-1));
            bool yes = thr >= 0 ? t >= thr : t >= Triggers().temp, settled = true;
            // With no level named, the test the question asks (kHeatTests): hot, the fan loud, the shutdown point near.
            if (thr < 0) {
                const int test = asked_test(r, kHeatTests);
                double fan = -1;
                for (const auto& f : s->power.fans)
                    if (f.percent.ok() && fan < 0) fan = double(f.percent.get());
                if (test == 1) settled = fan >= 0, yes = fan >= Triggers().fan;
                if (test == 2) settled = s->guard.margin.ok() && s->guard.margin.get() >= 0, yes = s->guard.margin.or_else(999) < Triggers().shutdown_margin;
            }
            if (settled) polar_answer(res, r, yes, clauses);
            else res.undecided = true;
        }
        // The level it is judged by, and what is usual here: the last day's readings' middle value (the hourly records'
        // over 30 days when the readings hold too few).
        if (lead == Intent::Heat && s->cpu.temp_c.or_else(s->guard.temp.or_else(-1)) >= 0) {
            auto usual = query(db_, "SELECT 0, temp FROM samples WHERE t >= ?1 AND temp >= 0 ORDER BY 2 LIMIT 1 OFFSET "
                                    "(SELECT COUNT(*) / 2 FROM samples WHERE t >= ?1 AND temp >= 0)", {now - 86400});
            std::string over = "over the last day";
            if (usual.empty()) {
                usual = query(db_, "SELECT 0, med FROM samples_hourly WHERE metric = 'temp' AND hour_ts >= ?1 ORDER BY 2 LIMIT 1 OFFSET "
                                   "(SELECT COUNT(*) / 2 FROM samples_hourly WHERE metric = 'temp' AND hour_ts >= ?1)", {now - 30 * 86400});
                over = "over the last 30 days";
            }
            res.body += " The recorder counts " + fmt("%.0f C", Triggers().temp) + " or more as hot" +
                        (usual.empty() ? std::string(".") : "; " + over + " the CPU typically ran at " + fmt("%.0f C", usual[0].a) + ".");
        }
        // "fans going crazy, what's causing it", "why is it so hot all the time": the fan winds down
        // slowly, so the last spike in the past 15 minutes names the cause; and a hardware finding
        // from the records (a fan that never spins, a worn battery) is the cause behind every day.
        if (lead == Intent::Heat && r.wants == unllm::Wants::Cause) {
            auto spike = query(db_, "SELECT CAST(start AS INTEGER), peak_temp FROM events WHERE (kinds & ?) != 0 AND \"end\" >= ? "
                                    "ORDER BY start DESC LIMIT 1", {int64_t(KindHeat | KindCpu), now - 900});
            if (!spike.empty()) {
                const std::string who = top_culprit(db_, spike[0].t, now, KindHeat | KindCpu, "");
                if (!who.empty()) res.body += " The last spike, at " + when_text(spike[0].t) + ", came mostly from " + who + ".";
            }
            Diagnose diag(db_);
            for (const auto& c : diag.run(now - 10 * 86400, now, now, s.get()))
                if (c.salience >= 3 && c.rule_id != "battery_wear") {
                    res.body += " " + c.sentence;
                    break;
                }
        }
        return res;
    }

    // ---- superlatives: the hottest, the most, the worst ----
    if (r.degree == Degree::Least) {
        res.body = "I record peaks, not quiet spells: ask for the most, the hottest or the worst instead.";
        return res;
    }
    if (r.degree == Degree::Most && r.form != Form::Count) {
        // An unstated window widens to everything the records keep.
        std::string reach = r.window.stated ? "in that window" : "in the 30 days of records";
        int64_t sfrom = r.window.stated ? from : now - 30 * 86400;
        // "was firefox open when the fan hit its peak": the peak's moments, then the program at each.
        if (polar && !program.empty()) {
            const char* column = lead == Intent::Heat ? "temp" : lead == Intent::Slow ? "cpu" : lead == Intent::Memory ? "mem_pct"
                                 : lead == Intent::Gpu ? "gpu" : "";
            const char* noun = lead == Intent::Heat ? "the CPU temperature" : lead == Intent::Slow ? "CPU use"
                               : lead == Intent::Memory ? "memory use" : "GPU load";
            std::vector<std::string> words;
            for (const auto& t : tokens) words.push_back(t.lower);
            const std::string a = peak_answer(db_, words, column, noun, program, sfrom, to, now, &res.receipts, &res.lead);
            if (!a.empty()) {
                res.body = a;
                res.branch = "most:peak";
                card_.subject = program;
                card_.subjects_named = {program};
                return res;
            }
        }
        // "what was the busiest hour yesterday", "which day was the hottest": a span of time named outside the time phrase
        // (an hour or a day, by the time reader's own units) asks which one, ranked by the topic's reading: the hourly
        // records' median for each hour the machine was awake at least half of (the per-second readings for the hours
        // not rolled up yet), a day by the median of its hours.
        {
            std::set<std::size_t> timed;
            unllm::find_time(tokens, now, &timed);
            int64_t unit = 0;
            for (std::size_t i = 0; i < tokens.size() && !unit; ++i)
                if (!timed.count(i))
                    if (const int64_t u = unllm::span_before(unllm::tokenize("the " + tokens[i].lower), 2); u == 3600 || u == 86400) unit = u;
            const char* metric = lead == Intent::Heat ? "temp" : lead == Intent::Memory ? "mem_pct" : lead == Intent::Gpu ? "gpu" : "cpu";
            const std::string noun = lead == Intent::Heat ? "the CPU temperature" : lead == Intent::Memory ? "memory use"
                                     : lead == Intent::Gpu ? "GPU load" : "CPU use";
            auto value = [&](double v) { return lead == Intent::Heat ? fmt("%.0f C", v) : fmt("%.0f%%", v); };
            struct Span {
                int64_t t;
                double med, top;
                int64_t n;
            };
            std::vector<Span> hours;
            if (unit && (lead == Intent::Heat || lead == Intent::Memory || lead == Intent::Gpu || lead == Intent::Slow || lead == Intent::Story)) {
                for (const auto& x : query(db_, "SELECT hour_ts, med, max, n FROM samples_hourly WHERE hour_ts >= ?1 AND hour_ts < ?2 AND metric = ?3 "
                                                "ORDER BY hour_ts", {sfrom, to}, {metric}))
                    hours.push_back({x.t, x.a, x.b, std::atoll(x.s1.c_str())});
                auto last = query(db_, "SELECT MAX(hour_ts) + 3600 FROM samples_hourly WHERE metric = ?1", {}, {metric});
                for (int64_t h = std::max(sfrom - sfrom % 3600, last.empty() ? sfrom : last[0].t); h < to; h += 3600) {
                    std::vector<double> v;
                    for (const auto& x : query(db_, "SELECT 0, " + std::string(metric) + " FROM samples WHERE t >= ?1 AND t < ?2 AND " + metric + " >= 0",
                                               {h, h + 3600}))
                        v.push_back(x.a);
                    if (!v.empty()) hours.push_back({h, judge::median(v), *std::max_element(v.begin(), v.end()), int64_t(v.size())});
                }
            }
            if (!hours.empty()) {
                int64_t full = 0;
                for (const auto& x : hours) full = std::max(full, x.n);
                std::vector<Span> whole, part;
                for (const auto& x : hours) (2 * x.n >= full ? whole : part).push_back(x);
                std::vector<Span> spans = whole;
                if (unit == 86400) {
                    std::map<std::string, std::vector<const Span*>> days;
                    for (const auto& x : whole) days[local_date(x.t)].push_back(&x);
                    spans.clear();
                    for (const auto& [d, hs] : days) {
                        std::vector<double> meds;
                        Span s{hs[0]->t, 0, 0, 0};
                        for (const Span* x : hs) meds.push_back(x->med), s.top = std::max(s.top, x->top), s.n += x->n;
                        s.med = judge::median(meds);
                        spans.push_back(s);
                    }
                }
                std::stable_sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.med > b.med; });
                auto name = [&](const Span& s) { return unit == 86400 ? day_text(s.t) : run_span(s.t, s.t + 3600); };
                if (!spans.empty()) {
                    std::vector<std::string> next;
                    for (std::size_t i = 1; i < spans.size() && next.size() < 2; ++i) next.push_back(name(spans[i]) + " (" + value(spans[i].med) + ")");
                    res.body = "The " + std::string(unit == 86400 ? "day" : "hour") + " with the highest " + noun.substr(noun.rfind("the ", 0) == 0 ? 4 : 0) +
                               " " + reach + " was " + name(spans[0]) + ": typically " + value(spans[0].med) + ", up to " + value(spans[0].top) +
                               (next.empty() ? "." : "; next came " + unllm::join_list(next) + ".");
                    // An hour the machine was awake for only part of, that ran higher, is said rather than ranked.
                    const Span* higher = nullptr;
                    for (const auto& x : part)
                        if (unit == 3600 && x.med > spans[0].med && (!higher || x.med > higher->med)) higher = &x;
                    if (higher)
                        res.body += " The " + duration_text(higher->n * 3600 / full) + " recorded in the hour from " + when_text(higher->t) +
                                    " ran higher, typically " + value(higher->med) + ", but that is not a whole hour.";
                    res.receipts.push_back(std::to_string(hours.size()) + " hours of readings, each hour's median");
                    res.branch = "most:span";
                    card_.moment = spans[0].t;
                    return res;
                }
            }
        }
        // "which python scripts ran the longest today": an interpreter named in a superlative ranks the scripts it ran (the
        // names module reads them from the command lines) by the time each ran inside the window, with the longest run
        // started there and the CPU each used there.
        if (!program.empty() && !polar) {
            names_.gather(sfrom, to);
            std::set<std::string> scripts;
            for (const auto& c : names_.candidates())
                if (!c.script.empty() && is_program(program, c.name, c.exe, "")) scripts.insert(c.script);
            struct Script {
                std::string name;
                int64_t inside = 0, since = 0, run_from = 0, run_to = 0;
                long procs = 0;
                double cpu = 0;
            };
            std::vector<Script> all;
            // The program's processes and its CPU per command line, read once; each script picks its own.
            const auto procs = scripts.empty() ? std::vector<Row>{}
                                               : query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, ?3), 0, name, exe || char(10) || cmdline "
                                                            "FROM procs WHERE started < ?2 AND IFNULL(ended, ?3) > ?1 AND is_program(?4, name, exe, cmdline) "
                                                            "ORDER BY started", {sfrom, to, now}, {program});
            const auto cpu = scripts.empty() ? std::vector<Row>{}
                                             : with_runs(db_, minute_cpu(db_, sfrom, to, "is_program(?4, mt.name, '', mt.cmdline)", {program}, "mt.cmdline", 100000),
                                                         sfrom, to, now, "is_program(?4, name, exe, cmdline)", {program}, "cmdline", 100000);
            for (const auto& s : scripts) {
                Script x{s};
                std::vector<std::pair<int64_t, int64_t>> ran;
                for (const auto& p : procs) {
                    const std::size_t nl = p.s2.find('\n');
                    if (!is_program(s, p.s1, p.s2.substr(0, nl), nl == std::string::npos ? "" : p.s2.substr(nl + 1))) continue;
                    ++x.procs;
                    const int64_t a = std::max(p.t, sfrom), b = std::min(int64_t(p.a), to);
                    if (!ran.empty() && a <= ran.back().second) ran.back().second = std::max(ran.back().second, b);
                    else ran.push_back({a, b});
                    if (!x.since || p.t < x.since) x.since = p.t;
                    if (p.t >= sfrom && int64_t(p.a) - p.t > x.run_to - x.run_from) x.run_from = p.t, x.run_to = int64_t(p.a);
                }
                for (const auto& [a, b] : ran) x.inside += b - a;
                for (const auto& c : cpu)
                    if (is_program(s, "", "", c.s1)) x.cpu += c.a;
                if (x.procs) all.push_back(x);
            }
            if (!all.empty()) {
                std::stable_sort(all.begin(), all.end(), [](const Script& a, const Script& b) { return a.inside > b.inside; });
                std::vector<std::string> longest;
                for (std::size_t i = 0; i < all.size() && i < 3; ++i) {
                    const Script& x = all[i];
                    longest.push_back(x.name + " (" + (x.procs > 1 ? unllm::number_word(x.procs) + " processes, " : "") +
                                      (x.since < sfrom ? "running since " + when_text(x.since) + ", " : "") + duration_text(x.inside) + ")");
                }
                res.body = "By the time they ran " + reach + ", " + program + "'s longest-running scripts were " + unllm::join_list(longest) + ".";
                const Script* started = nullptr;
                for (const auto& x : all)
                    if (x.run_to > x.run_from && (!started || x.run_to - x.run_from > started->run_to - started->run_from)) started = &x;
                if (started)
                    res.body += " Of the runs that started in it, the longest was " + started->name + ", " + run_span(started->run_from, started->run_to) +
                                " (" + duration_text(started->run_to - started->run_from) + ").";
                std::stable_sort(all.begin(), all.end(), [](const Script& a, const Script& b) { return a.cpu > b.cpu; });
                std::vector<std::string> busy;
                for (std::size_t i = 0; i < all.size() && busy.size() < 3; ++i)
                    if (all[i].cpu >= 1) busy.push_back(all[i].name + " " + thousands(all[i].cpu) + " s");
                if (!busy.empty()) res.body += " By CPU there: " + unllm::join_list(busy) + ".";
                res.branch = "most:scripts";
                card_.subject = program;
                card_.subjects_named = {program};
                return res;
            }
        }
        if (lead == Intent::Heat) {
            // Ground truth per second for the last day, event peaks beyond.
            if (r.window.stated && sfrom >= now - 24 * 3600) {
                auto rows = query(db_, "SELECT t, temp FROM samples WHERE t >= ? AND t < ? AND temp >= 0 "
                                       "ORDER BY temp DESC LIMIT 3", {sfrom, to});
                if (!rows.empty()) {
                    res.body = unllm::realize({{{"the CPU", true}, "reach", fmt("%.0f C", rows[0].a) +
                                                " at its hottest " + reach + ", at " + when_text(rows[0].t), Tense::Past}});
                    // What was working then: the busiest programs in the minutes around it.
                    std::vector<std::string> busy;
                    for (const auto& x : minute_cpu(db_, rows[0].t - 60, rows[0].t + 60, "1", {}, "mt.name", 3))
                        if (x.a >= 1) busy.push_back(shown_name(x.s1, x.s2));
                    if (!busy.empty()) res.body += " The busiest programs then: " + unllm::join_list(busy) + ".";
                    for (const auto& row : rows) res.receipts.push_back(when_text(row.t) + "  " + fmt("%.0f C", row.a));
                    return res;
                }
            }
            auto rows = query(db_, "SELECT start, peak_temp, id, summary FROM events WHERE start >= ? AND start < ? "
                                   "AND peak_temp > 0 ORDER BY peak_temp DESC, start LIMIT 3", {sfrom, to});
            if (rows.empty()) {
                res.body = unllm::realize({{{"heat event", false, 0}, "be", "recorded " + reach, Tense::Past}});
                return res;
            }
            res.body = unllm::realize({{{"the CPU", true}, "reach",
                                        fmt("%.0f C", rows[0].a) + " at its hottest " + reach + ", on " + day_text(rows[0].t),
                                        Tense::Past}});
            // When it was at that peak (each such event's own readings, a minute or less apart being one moment; the
            // event's span when it kept none), and what was working then: each one's main culprits, never the 'unseen'
            // rows (a parent's finished children, booked to it again).
            std::vector<std::string> moments, working;
            for (const auto& row : rows) {
                if (row.a < rows[0].a) break;
                std::vector<Row> at;
                for (const auto& x : query(db_, "SELECT CAST(t AS INTEGER), temp FROM event_samples WHERE event_id = ?1 "
                                                "AND temp >= (SELECT peak_temp FROM events WHERE id = ?1) ORDER BY t", {int64_t(row.b)}))
                    at.push_back(x);
                std::string when;
                for (const auto& s : spells(at, rows[0].a))
                    if (when.empty()) when = s.to - s.from < 60 ? clock_text(s.from).substr(0, 5) : clock_text(s.from).substr(0, 5) + " to " + clock_text(s.to).substr(0, 5);
                if (when.empty()) {
                    auto span = query(db_, "SELECT CAST(start AS INTEGER), \"end\" FROM events WHERE id = ?1", {int64_t(row.b)});
                    if (!span.empty()) when = clock_text(span[0].t).substr(0, 5) + " to " + clock_text(int64_t(span[0].a)).substr(0, 5);
                }
                moments.push_back(when);
                std::vector<std::string> who;
                for (const auto& c : query(db_, "SELECT 0, share, 0, name, cmdline FROM event_culprits WHERE event_id = ?1 AND IFNULL(kind, '') <> 'unseen' "
                                                "ORDER BY share DESC LIMIT 3", {int64_t(row.b)}))
                    who.push_back(shown_name(c.s1, c.s2));
                if (!who.empty()) working.push_back(unllm::join_list(who) + " at " + when);
            }
            if (!moments.empty()) res.body += " It was at that peak at " + unllm::join_list(moments) + ".";
            if (!working.empty()) {
                std::string list;
                for (const auto& w : working) list += (list.empty() ? "" : "; ") + w;
                res.body += " Working hardest then: " + list + ".";
            }
            for (const auto& row : rows) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
            card_.subject = "the CPU";
            card_.moment = rows[0].t;
            card_.metric = "temperature";
            card_.unit = "C";
            card_.value = rows[0].a;
            res.branch = "most:heat";
            return res;
        }
        if (lead == Intent::Memory || lead == Intent::Slow) {
            const bool mem = lead == Intent::Memory;
            // Processor time is what was used inside the window (the per-minute lists, the runs before them), not the
            // whole lives of the processes that touched it.
            auto rows = mem ? query(db_, "SELECT MIN(started), MAX(peak_rss), 0, name, MAX(cmdline) FROM procs WHERE started < ? AND IFNULL(ended, ?) >= ? "
                                         "GROUP BY name ORDER BY 2 DESC LIMIT 3", {to, now, sfrom})
                            : with_runs(db_, minute_cpu(db_, sfrom, to, "1", {}, "mt.name", 3), sfrom, to, now, "1", {}, "name", 3);
            if (rows.empty() || rows[0].s1.empty()) {
                res.body = unllm::realize({{{"record", false, 0}, "match", reach, Tense::Past}});
                return res;
            }
            std::string what = mem ? "the most memory " + reach + ": " + human_bytes(rows[0].a) + " at its peak"
                                   : "the most processor time " + reach + ": " + duration_text(int64_t(rows[0].a));
            // The machine's own peak and when, and who held the most at that moment, come first: "the highest memory use
            // and which program caused it" is answered in that order.
            std::string peak;
            if (mem) {
                // The per-second readings reach back days, the hourly summaries months: the higher of the two peaks.
                auto pk = query(db_, "SELECT CAST(t AS INTEGER), mem_pct FROM samples WHERE t >= ?1 AND t < ?2 AND mem_pct > 0 ORDER BY mem_pct DESC, t LIMIT 1", {sfrom, to});
                auto ph = query(db_, "SELECT hour_ts, max FROM samples_hourly WHERE metric = 'mem_pct' AND hour_ts >= ?1 AND hour_ts < ?2 AND max > 0 ORDER BY max DESC, hour_ts LIMIT 1", {sfrom, to});
                const bool hourly = !ph.empty() && (pk.empty() || ph[0].a > pk[0].a + 0.5);
                if (hourly) pk = ph;
                if (!pk.empty()) {
                    auto held = hourly ? query(db_, "SELECT 0, MAX(peak_rss), 0, name, MAX(cmdline) FROM minute_top WHERE minute >= ?1 AND minute < ?1 + 3600 AND peak_rss > 0 GROUP BY name ORDER BY 2 DESC LIMIT 1", {pk[0].t})
                                       : query(db_, "SELECT 0, MAX(peak_rss), 0, name, MAX(cmdline) FROM minute_top WHERE minute >= ?1 - 60 AND minute <= ?1 + 60 AND peak_rss > 0 GROUP BY name ORDER BY 2 DESC LIMIT 1", {pk[0].t});
                    peak = "Memory use peaked at " + fmt("%.0f%%", pk[0].a) + (hourly ? " in the hour from " : " at ") + when_text(pk[0].t) +
                           (held.empty() ? std::string() : ", when " + shown_name(held[0].s1, held[0].s2) + " held the most, " + human_bytes(held[0].a)) + ". ";
                }
            }
            res.body = peak + unllm::realize({{{rows[0].s1, true}, "use", what, Tense::Past}});
            std::vector<std::string> next;
            for (std::size_t i = 1; i < rows.size(); ++i)
                if (rows[i].a > 0) next.push_back(shown_name(rows[i].s1, rows[i].s2) + " with " + (mem ? human_bytes(rows[i].a) : duration_text(int64_t(rows[i].a))));
            if (!next.empty()) {
                res.body.pop_back();
                res.body += "; next came " + unllm::join_list(next) + ".";
            }
            for (const auto& row : rows)
                res.receipts.push_back(row.s1 + "  " + (mem ? human_bytes(row.a) : duration_text(int64_t(row.a))));
            card_.subject = rows[0].s1;
            card_.subjects_named = {rows[0].s1};
            card_.metric = mem ? "memory" : "cpu time";
            card_.value = rows[0].a;
            card_.window.from = sfrom;
            card_.window.to = to;
            res.branch = mem ? "most:memory" : "most:slow";
            return res;
        }
        if (lead == Intent::Story) {
            // -1 means "no reading": it must never look like a margin.
            auto rows = query(db_, "SELECT MIN(start), COUNT(*), IFNULL(MIN(CASE WHEN min_margin >= 0 THEN min_margin END), 999), "
                                   "date(start, 'unixepoch', 'localtime') FROM events WHERE start >= ? AND start < ? "
                                   "GROUP BY 4 ORDER BY 2 DESC, 3 ASC LIMIT 3", {sfrom, to});
            if (rows.empty()) {
                res.body = unllm::realize({{{"event", false, 0}, "be", "recorded " + reach, Tense::Past}});
                return res;
            }
            std::string what = day_text(rows[0].t) + ", with " + unllm::number_word(int64_t(rows[0].a)) + " " +
                               unllm::plural_noun("event", int64_t(rows[0].a));
            if (rows[0].b < 15) what += " and a shutdown margin of only " + fmt("%.0f C", rows[0].b);
            res.body = unllm::realize({{{"the worst day", true}, "be", what + " " + reach, Tense::Past}});
            for (const auto& row : rows)
                res.receipts.push_back(row.s2 + "  " + fmt("%.0f events", row.a));
            return res;
        }
        // Other topics fall through: their normal handlers already rank.
    }

    std::vector<Clause> clauses;
    unsigned mask = 0;
    const char* metric = nullptr;
    NP subject;
    std::string unit;
    switch (lead) {
    case Intent::Heat: mask = 16; metric = "peak_temp"; subject = {"the CPU", true}; unit = " C"; break;
    case Intent::Slow: mask = 1; metric = "peak_cpu"; subject = {"the CPU", true}; unit = "%"; break;
    case Intent::Memory: mask = 2; metric = "peak_mem"; subject = {"memory use", true}; unit = "%"; break;
    case Intent::Disk: mask = 4; metric = "peak_disk"; subject = {"disk traffic", true}; break;
    case Intent::Gpu: mask = 8; metric = "peak_gpu"; subject = {"GPU load", true}; unit = "%"; break;
    default: break;
    }

    // "Yes," or "No," only when the evidence speaks to what was
    // asked: peaks answer "did it get hot", not "is it broken".
    static const std::set<std::string> answers = {
        "hot", "heat", "overheat", "reach", "hit", "exceed", "over", "high", "warm", "cook", "burn",
        "scream", "loud", "noisy", "fan", "spin", "shut", "shutdown", "down", "slow", "lag", "freeze",
        "busy", "spike", "stutter", "hang", "swap", "leak", "full", "fill", "hammer", "write", "read",
        "temperature", "cpu", "memory", "ram", "disk", "gpu", "degree", "percent", "c"};
    const bool decidable = r.predicate.empty() || answers.count(r.predicate) ||
                           quantity_threshold(r, lead == Intent::Heat ? "C" : "%") >= 0;
    // "is ollama still running since it got killed": a yes or no about a program that no peak decides is about the
    // program itself (below).
    if (polar && !decidable && !program.empty()) mask = 0;

    // "was blender running on wednesday, and for how long", "how long was blender running": the program's runs in the
    // window (its processes that overlap or follow within a minute joined), each with its times, and their time together.
    if (r.wants == unllm::Wants::Duration && !program.empty()) {
        std::vector<std::pair<int64_t, int64_t>> runs;
        for (const auto& x : query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, ?3) FROM procs WHERE started < ?2 AND IFNULL(ended, ?3) > ?1 "
                                        "AND is_program(?4, name, exe, cmdline) ORDER BY started", {from, to, now}, {program})) {
            const int64_t a = std::max(x.t, from), b = std::min(int64_t(x.a), to);
            if (!runs.empty() && a <= runs.back().second + 60) runs.back().second = std::max(runs.back().second, b);
            else runs.push_back({a, b});
        }
        int64_t total = 0;
        for (const auto& [a, b] : runs) total += b - a;
        if (total > 0) {
            std::vector<std::string> said;
            for (const auto& [a, b] : runs)
                if (said.size() < 4) said.push_back(run_span(a, b) + " (" + duration_text(b - a) + ")");
            if (runs.size() > said.size()) said.push_back(unllm::number_word(long(runs.size() - said.size())) + " more");
            std::string body = program + " ran for " + duration_text(total) + " in that window, in " +
                               (runs.size() == 1 ? std::string("one run") : unllm::number_word(long(runs.size())) + " runs") + ": " + unllm::join_list(said) + ".";
            if (polar) settle(res, r, true);
            body[0] = char(std::toupper(static_cast<unsigned char>(body[0])));
            res.body = body;
            add_receipts("SELECT started,0,0,name || '  pid ' || pid FROM procs WHERE started < ? AND IFNULL(ended, ?) >= ? AND is_program(?, name, exe, cmdline) ORDER BY started DESC",
                         {to, now, from}, {program});
        } else {
            Clause c{{program, true}, "run", "in that window", Tense::Past};
            c.negated = true;
            if (polar) polar_answer(res, r, false, {c});
            else res.body = unllm::realize({c});
        }
        res.branch = "program:duration";
        card_.subject = program;
        card_.subjects_named = {program};
        return res;
    }

    if (mask) {
        // "How often does the fan max out" with no window means: over
        // everything the records keep, and the answer says so.
        std::string reach = "";
        if ((r.form == Form::Count || when_frame) && !r.window.stated) {
            from = now - 30 * 86400;
            reach = " in the 30 days of records";
        }
        // "...when blender is running": only the events that program was in,
        // over the whole history when no window was stated.
        std::string cond;
        std::vector<std::string> cond_tb;
        if (!r.condition.empty()) {
            cond = " AND e.id IN (SELECT event_id FROM event_culprits WHERE is_program(?4, name, '', cmdline))";
            cond_tb = {r.condition};
            if (!r.window.stated) {
                from = now - 30 * 86400;
                reach = " in the 30 days of records";
            }
            reach = " while " + r.condition + " was running" + reach;
        }
        // "did something mine crypto on my laptop today": a yes or no about an unnamed program's work on the CPU that no
        // peak decides. What the records can say: whether a process held the recorder's heavy level (a share of the whole
        // machine) for half an hour or more without a break, its per-minute figures held to what the readings say the
        // machine used; and whether the heaviest programs are ones the facts know or that ran here before the window.
        if (lead == Intent::Slow && polar && !decidable && program.empty() && r.condition.empty()) {
            auto last = query(db_, "SELECT 0, 0, 0, cores FROM samples WHERE IFNULL(cores, '') <> '' ORDER BY t DESC LIMIT 1", {});
            const int64_t cores = last.empty() ? int64_t(std::max(1u, std::thread::hardware_concurrency()))
                                               : 1 + std::count(last[0].s1.begin(), last[0].s1.end(), ',');
            const double heavy = Triggers().proc_cpu / 100.0 * 60.0 * double(cores);  // CPU seconds a minute
            struct Run {
                int64_t pid = 0, ticks = 0, from = 0, to = 0;
                std::string name, cmdline;
            };
            std::vector<Run> runs;
            for (const auto& x : query(db_, "SELECT minute, pid, start_ticks, name, cmdline FROM minute_top mt WHERE minute >= ?1 AND minute < ?2 AND "
                                            "MIN(cpu_seconds, IFNULL((SELECT AVG(s.cpu) * 0.6 * ?3 FROM samples s WHERE s.t >= mt.minute AND s.t < mt.minute + 60), "
                                            "cpu_seconds)) >= ?4 ORDER BY pid, start_ticks, minute",
                                       {from, to, cores, int64_t(std::ceil(heavy))})) {
                const int64_t pid = int64_t(x.a), ticks = int64_t(x.b);
                if (!runs.empty() && runs.back().pid == pid && runs.back().ticks == ticks && x.t - runs.back().to == 60) runs.back().to = x.t;
                else runs.push_back({pid, ticks, x.t, x.t, x.s1, x.s2});
            }
            std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.to - a.from > b.to - b.from; });
            // Whether a program is placed: the facts know it by name or by its executable, or it ran here before the window.
            auto known = [&](const std::string& name) -> const Fact* {
                if (const Fact* f = facts_.lookup(name)) return f;
                auto exe = query(db_, "SELECT 0, 0, 0, exe FROM procs WHERE name = ? AND IFNULL(exe, '') <> '' ORDER BY started DESC LIMIT 1", {}, {name});
                if (exe.empty()) return nullptr;
                const Fact* f = facts_.lookup(exe[0].s1.substr(exe[0].s1.rfind('/') + 1));
                return f ? f : facts_.lookup(exe[0].s1);
            };
            auto first_seen = [&](const std::string& name) {
                auto f = query(db_, "SELECT CAST(MIN(started) AS INTEGER) FROM procs WHERE name = ?", {}, {name});
                return f.empty() ? int64_t(0) : f[0].t;
            };
            const std::string level = fmt("%.0f%%", Triggers().proc_cpu) + " of the CPU";
            const auto top = minute_cpu(db_, from, to, "1", {}, "mt.name", 5);
            std::vector<std::string> heaviest;
            std::vector<const Row*> unplaced;
            for (const auto& x : top) {
                if (x.a < 60) continue;
                heaviest.push_back(shown_name(x.s1, x.s2) + " (" + duration_text(int64_t(x.a)) + " of CPU)");
                if (x.a >= 600 && !known(x.s1) && first_seen(x.s1) >= from) unplaced.push_back(&x);
            }
            const std::string listed = heaviest.empty() ? "" : " The heaviest programs were " + unllm::join_list(heaviest) + ".";
            if (!runs.empty() && runs[0].to - runs[0].from >= 29 * 60) {
                const Run& w = runs[0];
                const Fact* f = known(w.name);
                const int64_t seen = first_seen(w.name);
                res.lead = "Maybe.";
                res.body = shown_name(w.name, w.cmdline) + " used " + level + " or more for " + duration_text(w.to + 60 - w.from) +
                           " without a break, from " + run_span(w.from, w.to + 60) + "; " +
                           (f ? "it is a program I know, " + f->explanation + "."
                              : seen < from ? "it isn't a program I know, but it has run here since " + when_text(seen) + "."
                                            : "it isn't a program I know, and it first ran " + when_text(seen) + ".") +
                           listed;
                res.body[0] = char(std::toupper(static_cast<unsigned char>(res.body[0])));
            } else {
                const std::string longest = runs.empty() ? " (none did for a whole minute)"
                                                         : " (the longest stretch was " + duration_text(runs[0].to + 60 - runs[0].from) + ", " +
                                                               shown_name(runs[0].name, runs[0].cmdline) + " from " + when_text(runs[0].from) + ")";
                if (unplaced.empty()) {
                    res.body = "No sign of it between " + span_text(from, to, " and ") + ": no process used " + level + " for half an hour without a break" +
                               longest + ", and the heaviest programs were ones I know or that ran here before: " + unllm::join_list(heaviest) + ".";
                    if (heaviest.empty()) res.body = "No sign of it between " + span_text(from, to, " and ") + ": no process used " + level +
                                                     " for half an hour without a break" + longest + ".";
                } else {
                    std::vector<std::string> named;
                    for (const Row* x : unplaced)
                        named.push_back(shown_name(x->s1, x->s2) + " (first ran " + when_text(first_seen(x->s1)) + ", " + duration_text(int64_t(x->a)) + " of CPU)");
                    res.body = "No long steady load between " + span_text(from, to, " and ") + ": no process used " + level + " for half an hour without a break" +
                               longest + ". But " + unllm::join_list(named) + (named.size() == 1 ? " is new here and isn't a program I know." : " are new here and aren't programs I know.") +
                               listed;
                }
            }
            res.branch = "mask:slow:steady";
            return res;
        }
        // "how hard was the cpu working while haruna was open": the program's runs are the window, and the readings
        // in them say how hard (their average and peak) where the per-second readings reach. A "while ..." whose
        // words named no program gets the whole window, saying so.
        const char* column = lead == Intent::Heat ? "temp" : lead == Intent::Slow ? "cpu" : lead == Intent::Memory ? "mem_pct"
                             : lead == Intent::Gpu ? "gpu" : nullptr;
        // The whole window is told here only where the per-second readings reach its start; before them the answer
        // below (from the events too) tells it, led by the clause's note (ask_once).
        bool whole = column && r.condition.empty() && !condition_tried_.empty();
        if (whole) {
            const auto first = query(db_, "SELECT CAST(MIN(t) AS INTEGER) FROM samples WHERE t >= ?1 AND t < ?2", {from, to});
            whole = !first.empty() && first[0].t > 0 && first[0].t <= from + 900;
        }
        if (column && (!r.condition.empty() || whole) && r.wants != unllm::Wants::Name && r.wants != unllm::Wants::Cause) {
            std::vector<std::pair<int64_t, int64_t>> spans;
            if (!r.condition.empty()) {
                // Its processes' lifetimes, joined where they overlap or touch (a command typed while its service ran is
                // the same time), so each reading counts once.
                for (const auto& x : query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, ?3) FROM procs WHERE started < ?1 AND "
                                                "IFNULL(ended, ?3) >= ?2 AND is_program(?4, name, exe, cmdline) ORDER BY started",
                                           {to, from, now}, {r.condition})) {
                    const int64_t a = std::max(x.t, from), b = std::min(int64_t(x.a), to);
                    if (!spans.empty() && a <= spans.back().second + 60) spans.back().second = std::max(spans.back().second, b);
                    else spans.push_back({a, b});
                }
            } else {
                spans.push_back({from, to});
            }
            const Stats st = sample_stats(db_, column, spans);
            if (st.n > 0) {
                auto value = [&](double v) { return lead == Intent::Heat ? fmt("%.0f C", v) : fmt("%.1f%%", v); };
                const std::string who = lead == Intent::Heat ? "the CPU temperature" : lead == Intent::Slow ? "the CPU"
                                        : lead == Intent::Memory ? "memory use" : "GPU load";
                std::string text;
                if (!r.condition.empty()) {
                    // The runs the per-second readings reach, the latest last ("since" for one still running); the readings
                    // begin later than the window when they do, and that is said.
                    auto begun = query(db_, "SELECT CAST(MIN(t) AS INTEGER) FROM samples WHERE " + std::string(column) + " >= 0 AND t >= ?1 AND t < ?2", {from, to});
                    const int64_t detail = begun.empty() || begun[0].t == 0 ? from : begun[0].t;
                    std::vector<std::string> when;
                    long reached = 0;
                    for (const auto& [a, b] : spans)
                        if (b > detail) {
                            ++reached;
                            if (when.size() < 3) when.push_back(b >= std::min(to, now) - 60 ? "since " + when_text(a) : run_span(a, b));
                        }
                    if (reached > long(when.size())) when.push_back(unllm::number_word(reached - long(when.size())) + " more runs");
                    text = "While " + r.condition + " was running (" + unllm::join_list(when) +
                           (detail > from + 900 ? "; the per-second readings begin " + when_text(detail) : std::string()) + "), " + who;
                } else {
                    std::vector<std::string> quoted;
                    for (const auto& w : condition_tried_) quoted.push_back("\"" + w + "\"");
                    text = "I couldn't tell which program \"" + r.condition_thing + "\" was: no program that ran then is described "
                           "that way" + (quoted.size() > 1 ? " (I tried " + unllm::join_list(quoted) + ")" : std::string()) +
                           ". Over the whole window, " + who;
                }
                text += " averaged " + value(st.avg) + " and peaked at " + value(st.peak) + " at " + when_text(st.peak_t) + ".";
                if (!r.condition.empty() && lead == Intent::Slow) {
                    double own = 0;
                    for (const auto& [a, b] : spans)
                        for (const auto& x : minute_cpu(db_, a, b, "is_program(?4, mt.name, '', mt.cmdline)", {r.condition}, "''", 1))
                            own += x.a;
                    if (own >= 60)
                        text += " " + unllm::realize({{{r.condition, true}, "use", duration_text(int64_t(own)) + " of CPU time itself", Tense::Past}});
                }
                text[0] = char(std::toupper(static_cast<unsigned char>(text[0])));
                res.body = text;
                res.receipts.push_back(std::to_string(st.n) + " per-second readings in " + unllm::number_word(long(spans.size())) + " " +
                                       unllm::plural_noun("span", long(spans.size())));
                res.branch = std::string("mask:") + unllm::intent_name(lead) + ":while";
                return res;
            }
        }
        // "how much cpu did ollama use": the program's CPU seconds in the window from the per-minute lists, with the
        // runners it starts, rather than a spike's peak.
        if (lead == Intent::Slow && !program.empty() && r.wants == unllm::Wants::Quantity) {
            const auto parts = program_cpu(db_, program, from, to, now);
            double total = 0;
            int64_t guessed = 0;
            for (const auto& x : parts) total += x.a, guessed += x.t;
            if (total > 0) {
                std::vector<std::string> each;
                for (const auto& x : parts)
                    if (each.size() < 4)
                        each.push_back(shown_name(x.s1, x.s2) + " " + thousands(x.a) + " s" +
                                       (x.b > 1 ? " in " + unllm::number_word(long(x.b)) + " processes" : ""));
                // Runs whose recording stopped before they ended lost their own figures: what the spikes put on them is said apart.
                const std::string reach = r.window.stated ? "in that window" : "in the last day";
                res.body = unllm::realize({{{program, true}, "use",
                                            guessed < 1 ? "about " + thousands(total) + " CPU seconds " + reach + ", " + duration_text(int64_t(total)) +
                                                              " of one core's time" + (parts.size() > 1 ? ": " + unllm::join_list(each) : "")
                                                        : "about " + thousands(total - double(guessed)) + " CPU seconds " + reach + " in its recorded runs, and " +
                                                              "about " + thousands(double(guessed)) + " more by estimate for runs whose recording stopped before they " +
                                                              "ended (a crash, a power loss or a restart; from the spikes they were in): about " + thousands(total) + " in all, " +
                                                              duration_text(int64_t(total)) + " of one core's time" +
                                                              (parts.size() > 1 ? " (" + unllm::join_list(each) + ")" : ""),
                                            Tense::Past}});
                res.receipts.push_back("per-minute lists: each minute keeps only its busiest programs, so this can run a little low");
                if (const int64_t detail = lists_start(db_, to); from < detail)
                    res.receipts.push_back("before " + when_text(detail) + ", when the per-minute lists begin: each run's CPU shared out by the "
                                           "time the machine was awake, a run cut off by a crash counted from the spikes it was in");
                res.branch = "mask:slow:program";
                card_.subject = program;
                card_.subjects_named = {program};
                card_.metric = "cpu time";
                card_.value = total;
                return res;
            }
        }
        // "how much memory was llama-server using yesterday morning": the program's processes in the window and the peak each
        // reached, the highest with its run's times, rather than the memory events. The records keep each process's peak,
        // not its memory minute by minute.
        if (lead == Intent::Memory && !program.empty() && r.wants == unllm::Wants::Quantity) {
            auto runs = query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, ?3), peak_rss FROM procs WHERE started < ?2 AND IFNULL(ended, ?3) > ?1 "
                                   "AND is_program(?4, name, exe, cmdline) AND peak_rss > 0 ORDER BY peak_rss DESC, started", {from, to, now}, {program});
            if (!runs.empty()) {
                std::vector<double> peaks;
                for (const auto& x : runs) peaks.push_back(x.b);
                std::vector<std::string> next;
                for (std::size_t i = 1; i < runs.size() && next.size() < 2; ++i)
                    next.push_back(human_bytes(runs[i].b) + " (" + run_span(runs[i].t, int64_t(runs[i].a)) + ")");
                std::string text = program + " ran " + (runs.size() == 1 ? std::string("once") : unllm::number_word(long(runs.size())) + " times") +
                                   " in that window; its memory peaked at " + human_bytes(runs[0].b) + " in the run from " +
                                   run_span(runs[0].t, int64_t(runs[0].a)) + (next.empty() ? "." : ", then " + unllm::join_list(next) + ".");
                if (runs.size() >= 3) text += " Half of its runs peaked at " + human_bytes(judge::median(peaks)) + " or less.";
                text[0] = char(std::toupper(static_cast<unsigned char>(text[0])));
                res.body = text;
                res.notes.push_back("The records keep each process's peak, not its memory minute by minute.");
                res.branch = "mask:memory:program";
                card_.subject = program;
                card_.subjects_named = {program};
                card_.metric = "memory";
                card_.value = runs[0].b;
                card_.moment = runs[0].t;
                return res;
            }
        }
        // "how often does it hit 95 degrees": how often the reading reached that level: the hours whose highest reading did
        // (the hourly records, over every day they hold), the days with one, and the events that peaked there; how long it
        // stayed there, from the per-second readings where they reach.
        const double level = quantity_threshold(r, lead == Intent::Heat ? "C" : "%");
        // "how long was the cpu above 80% today": the time at or over the level named, from the readings, not the span of
        // the events.
        if (r.wants == unllm::Wants::Duration && level >= 0 && column && program.empty() && r.condition.empty()) {
            const std::string told = above_text(db_, column, column, lead == Intent::Heat ? "The CPU temperature" : lead == Intent::Slow ? "CPU use"
                                                                     : lead == Intent::Memory ? "Memory use" : "GPU load",
                                                [&](double v) { return lead == Intent::Heat ? fmt("%.0f C", v) : fmt("%.0f%%", v); }, level, from, to);
            if (!told.empty()) {
                res.body = told;
                res.branch = std::string("mask:") + unllm::intent_name(lead) + ":above";
                return res;
            }
        }
        if (r.form == Form::Count && level >= 0 && column && program.empty() && r.condition.empty()) {
            auto hours = query(db_, "SELECT hour_ts, max FROM samples_hourly WHERE hour_ts >= ?1 AND hour_ts < ?2 AND metric = ?3 ORDER BY hour_ts",
                               {from, to}, {column});
            if (!hours.empty()) {
                auto value = [&](double v) { return lead == Intent::Heat ? fmt("%.0f C", v) : fmt("%.0f%%", v); };
                std::set<std::string> days, hot_days;
                long hot = 0;
                for (const auto& x : hours) {
                    days.insert(local_date(x.t));
                    if (x.a >= level) ++hot, hot_days.insert(local_date(x.t));
                }
                auto evs = query(db_, "SELECT COUNT(*) FROM events WHERE start >= ?1 AND start < ?2 AND " + std::string(metric) + " >= ?3",
                                 {from, to, int64_t(std::ceil(level))});
                const long events = evs.empty() ? 0 : long(evs[0].t);
                const std::string at = value(level) + " or more";
                std::string text = std::string(lead == Intent::Heat ? "The CPU temperature" : lead == Intent::Slow ? "CPU use"
                                               : lead == Intent::Memory ? "Memory use" : "GPU load") +
                                   " reached " + at + " in " + unllm::number_word(hot) + " of the " + unllm::number_word(long(hours.size())) +
                                   " hours on record (on " + unllm::number_word(long(hot_days.size())) + " of the " +
                                   unllm::number_word(long(days.size())) + " days), and " + unllm::number_word(events) + " " +
                                   unllm::plural_noun("event", events) + " peaked at " + at + (reach.empty() ? " in that window" : reach) + ".";
                auto rows = query(db_, "SELECT CAST(t AS INTEGER), " + std::string(column) + " FROM samples WHERE t >= ?1 AND t < ?2 AND " + column +
                                       " >= 0 ORDER BY t", {from, to});
                if (!rows.empty()) {
                    const auto s = spells(rows, level);
                    int64_t secs = 0;
                    for (const auto& x : s) secs += x.to - x.from;
                    text += " In the per-second readings (from " + when_text(rows.front().t) + ") it was at " + at +
                            (s.empty() ? " not once." : " for " + duration_text(secs) + " in all, " +
                                                            "in " + (s.size() == 1 ? std::string("one spell") : unllm::number_word(long(s.size())) + " spells") + ".");
                }
                res.body = text;
                res.receipts.push_back(std::to_string(hours.size()) + " hourly records from " + when_text(hours.front().t) + ", each hour's highest reading");
                res.branch = std::string("mask:") + unllm::intent_name(lead) + ":often";
                return res;
            }
        }
        // "how has the cpu temperature been over the past hour", "what was the gpu doing last night": the window told from
        // its readings where the per-second samples reach it, in place of a count of spike events (a calm hour has none,
        // and a night of short spikes is mostly idle).
        std::string told;
        if (column && program.empty() && r.condition.empty() && !polar && r.form != Form::Count &&
            (r.wants == unllm::Wants::Fact || r.wants == unllm::Wants::Quantity)) {
            const Triggers trig;
            // A word naming another reading the records keep ("memory pressure yesterday": the samples' pressure column)
            // is that reading, where the answer has words for it.
            static const std::map<std::string, std::tuple<const char*, const char*, double>> worded = {
                {"pressure", {"Memory pressure (the share of time programs waited for memory)", "%.1f%%", Triggers().pressure}}};
            std::string named;
            for (const auto& c : query(db_, "SELECT 0, 0, 0, name FROM pragma_table_info('samples') WHERE type = 'REAL' AND name <> 't'", {}))
                for (const auto& tk : tokens)
                    if (tk.lower == c.s1 && worded.count(c.s1)) named = c.s1;
            if (!named.empty()) {
                const auto& [what, form, mark] = worded.at(named);
                told = readings_text(db_, named, what, [f = form](double v) { return fmt(f, v); }, mark, from, to);
            } else {
                told = readings_text(db_, column, lead == Intent::Heat ? "The CPU temperature" : lead == Intent::Slow ? "CPU use"
                                                  : lead == Intent::Memory ? "Memory use" : "GPU load",
                                     [&](double v) { return lead == Intent::Heat ? fmt("%.0f C", v) : fmt("%.0f%%", v); },
                                     lead == Intent::Heat ? trig.temp : lead == Intent::Slow ? trig.cpu_total : lead == Intent::Gpu ? trig.gpu : -1, from, to);
            }
        }
        // The part of each event inside the window: its peak there (from its own readings) and its count.
        const char* reading = lead == Intent::Heat ? "temp" : lead == Intent::Slow ? "cpu" : lead == Intent::Memory ? "mem_pct"
                              : lead == Intent::Disk ? "disk_bps" : "gpu";
        const std::string inside = "CASE WHEN p.part >= 1 THEN e." + std::string(metric) + " ELSE IFNULL((SELECT MAX(s." + reading +
                                   ") FROM event_samples s WHERE s.event_id = e.id AND s.t >= ?1 AND s.t < ?2), e." + metric + ") END";
        const std::string in_window = std::string(kEventParts) + "SELECT CAST(e.start AS INTEGER), e.id, 0, e.summary FROM p "
                                      "JOIN events e ON e.id = p.id WHERE p.part > 0" + cond;
        // -1 means "no reading": it must never look like a margin.
        auto agg = query(db_,
                         std::string(kEventParts) + "SELECT COUNT(*), MAX(CASE WHEN " + inside + " >= 0 THEN " + inside +
                             " END), IFNULL(MIN(CASE WHEN e.min_margin >= 0 THEN e.min_margin END), 999) "
                             "FROM p JOIN events e ON e.id = p.id WHERE p.part > 0" + cond,
                         {from, to, int64_t(mask)}, cond_tb);
        int64_t n = agg.empty() ? 0 : agg[0].t;
        if (n > 0) {
            double peak = agg[0].a;
            std::string peak_text = lead == Intent::Disk ? human_bytes(peak) + "/s" : fmt("%.0f", peak) + unit;
            std::string rest = peak_text + " during " + unllm::number_word(n) + " " +
                               unllm::plural_noun("event", n) + reach;
            clauses.push_back({subject, "reach", rest, Tense::Past});
            if (lead == Intent::Heat && agg[0].b >= 0 && agg[0].b < 15)
                clauses.push_back({{"it", true}, "come", "within " + fmt("%.0f", agg[0].b) + " C of the shutdown point", Tense::Past});
            // Who: "what was eating the cpu" ranks the per-minute CPU over the whole window, not its spikes alone;
            // otherwise the events' culprits by the kind's own measure. A program named gets its own figure, and
            // the top is named as it is.
            const QuestionKind* qk = kinds_.find(kind_);
            const bool ranking = r.wants == unllm::Wants::Name || (qk && qk->form == "which thing");
            const bool by_minute = lead == Intent::Slow && ranking && r.condition.empty();
            std::vector<Row> top = by_minute ? minute_cpu(db_, from, to, "1", {}, "mt.name", 3) : std::vector<Row>{};
            const bool minute_figures = !top.empty();
            if (top.empty()) top = ranked_culprits(db_, from, to, mask, "", 3);
            auto figure = [&](double v) {
                if (minute_figures) return thousands(v) + " CPU seconds";
                if (mask == 4) return human_bytes(v) + " read and written";
                if (mask == 8) return human_bytes(v) + " of video memory";
                if (mask == 2) return human_bytes(v) + " of memory";
                return duration_text(int64_t(v)) + " of CPU time";
            };
            std::string culprit, who;
            if (!top.empty()) {
                culprit = top[0].s1;
                const std::string first = shown_name(top[0].s1, top[0].s2);
                std::vector<std::string> next;
                for (std::size_t i = 1; i < top.size(); ++i)
                    if (top[i].a > 0) next.push_back(shown_name(top[i].s1, top[i].s2) + " with " + figure(top[i].a));
                if (program.empty() || is_program(program, top[0].s1, "", top[0].s2)) {
                    who = unllm::realize({{{first, true}, minute_figures ? "use" : "be",
                                           minute_figures ? "the most CPU in that window, about " + figure(top[0].a)
                                                          : "the main cause" + (top[0].a > 0 ? " (" + figure(top[0].a) + ")" : std::string()),
                                           Tense::Past}});
                    if (!next.empty()) {
                        who.pop_back();
                        who += "; next came " + unllm::join_list(next) + ".";
                    }
                } else {
                    double own = 0;
                    // The processes it starts in a unit it heads are its own ("ollama" with its llama-server runners): their
                    // part counts with its, each named.
                    std::vector<Row> mine;
                    if (minute_figures)
                        for (const auto& x : program_cpu(db_, program, from, to, now)) own += x.a;
                    else
                        mine = ranked_culprits(db_, from, to, mask, program, 8, headed_units(db_, program, from, to, now));
                    std::vector<std::string> runners, each;
                    for (const auto& x : mine) {
                        own += x.a;
                        if (!is_program(program, x.s1, "", x.s2)) runners.push_back(x.s1);
                        if (x.a > 0) each.push_back(shown_name(x.s1, x.s2) + " " + (mask == 1 || mask == 16 ? duration_text(int64_t(x.a)) : human_bytes(x.a)));
                    }
                    const std::string with = runners.empty() ? "" : ", with the " + unllm::join_list(runners) + " processes it starts,";
                    if (!runners.empty() && own >= top[0].a) {
                        std::vector<std::string> rest;
                        for (const auto& x : top)
                            if (x.a > 0 && std::find_if(mine.begin(), mine.end(), [&](const Row& m) { return m.s1 == x.s1; }) == mine.end())
                                rest.push_back(shown_name(x.s1, x.s2) + " with " + figure(x.a));
                        who = program + with + " was the main cause (" + figure(own) + ": " + unllm::join_list(each) + ")" +
                              (rest.empty() ? "." : "; next came " + unllm::join_list(rest) + ".");
                    } else {
                        who = (own > 0 ? program + "'s part" + with + " was " + figure(own) + "; the main cause was "
                                       : program + " wasn't among the culprits; the main cause was ") +
                              first + " (" + figure(top[0].a) + ").";
                    }
                }
            }
            res.branch = std::string("mask:") + unllm::intent_name(lead);
            card_.subject = culprit;
            if (!culprit.empty()) card_.subjects_named = {culprit};
            card_.metric = metric;
            card_.value = peak;
            card_.window.from = from;
            card_.window.to = to;
            {
                auto top_event = query(db_, in_window + " ORDER BY " + inside + " DESC LIMIT 1", {from, to, int64_t(mask)}, cond_tb);
                if (!top_event.empty()) {
                    card_.moment = top_event[0].t;
                    card_.event_id = int64_t(top_event[0].a);
                }
            }
            // "how long was it hot": the events' combined span, not their peak.
            if (r.wants == unllm::Wants::Duration) {
                auto dur = query(db_, std::string(kEventParts) + "SELECT 0, SUM(p.part * (e.\"end\" - e.start)), 0 FROM p "
                                      "JOIN events e ON e.id = p.id WHERE p.part > 0" + cond,
                                 {from, to, int64_t(mask)}, cond_tb);
                double secs = dur.empty() ? 0 : dur[0].a;
                if (secs > 0) {
                    res.body = unllm::realize({{subject, "be", "in that state for " + duration_text(int64_t(secs)) +
                                                ", across " + unllm::number_word(n) + " " +
                                                unllm::plural_noun("event", n) + reach, Tense::Past}});
                    add_receipts(in_window + " ORDER BY e.start DESC", {from, to, int64_t(mask)}, cond_tb);
                    return res;
                }
            }
            if (polar && decidable) {
                bool yes = true;
                double thr = quantity_threshold(r, lead == Intent::Heat ? "C" : "%");
                if (thr >= 0) yes = peak >= thr;
                // A heat event opens on a quick rise, the fan and the shutdown margin as well as the hot level, so an event
                // is no yes for heat: the test the question asks is, against the recorder's own level for it.
                const Triggers trig;
                std::string no;  // the reason a no is a no
                bool settled = true;
                if (thr < 0 && lead == Intent::Heat) {
                    const int test = asked_test(r, kHeatTests);
                    if (test == 0) {
                        yes = peak >= trig.temp;
                        no = "No: it stayed under " + fmt("%.0f C", trig.temp) + ", the level the recorder counts as hot.";
                    } else if (test == 1) {
                        auto fan = query(db_, std::string(kEventParts) + "SELECT COUNT(*), IFNULL(MAX(e.peak_fan), -1) FROM p JOIN events e ON e.id = p.id "
                                              "WHERE p.part > 0" + cond, {from, to, int64_t(mask)}, cond_tb);
                        const double top = fan.empty() ? -1 : fan[0].a;
                        settled = top >= 0;
                        yes = top >= trig.fan;
                        no = "No: the fan stayed under " + fmt("%.0f%%", trig.fan) + ", the level the recorder counts as loud.";
                    } else if (test == 2) {
                        settled = agg[0].b >= 0 && agg[0].b < 999;
                        yes = agg[0].b < trig.shutdown_margin;
                        no = "No: it came no closer than " + fmt("%.0f C", agg[0].b) + " to the shutdown point.";
                    }
                }
                if (settled) {
                    polar_answer(res, r, yes, clauses);
                    if (res.lead == "No." && !no.empty()) res.lead = no;
                } else {
                    res.undecided = true;
                    res.body = unllm::realize(clauses);
                }
            } else if (polar) {
                res.undecided = true;
                res.body = unllm::realize(clauses);
            } else if (!told.empty()) {
                // The readings in place of the count of events; the closest approach to the shutdown point stays.
                res.body = told + (clauses.size() > 1 ? " " + unllm::realize(std::vector<Clause>(clauses.begin() + 1, clauses.end())) : std::string());
            } else {
                res.body = unllm::realize(clauses);
            }
            // When it was worst, and the fan, beyond the per-second readings: the events at the window's highest temperature
            // (a couple of minutes apart being one stretch) with the fan's peak in them, and the stretches the fan ran at or
            // over the recorder's level for it.
            if (lead == Intent::Heat && told.empty()) {
                auto evs = query(db_, in_window + " ORDER BY e.start", {from, to, int64_t(mask)}, cond_tb);
                std::vector<std::pair<int64_t, int64_t>> worst, loud;
                double top = -1, fan = -1;
                std::vector<std::tuple<int64_t, int64_t, double, double>> each;  // start, end, peak temperature, peak fan
                for (const auto& x : evs)
                    for (const auto& e : query(db_, "SELECT CAST(start AS INTEGER), \"end\", peak_temp, IFNULL(peak_fan, -1) FROM events WHERE id = ?1",
                                               {int64_t(x.a)})) {
                        each.emplace_back(e.t, int64_t(e.a), e.b, std::atof(e.s1.c_str()));
                        top = std::max(top, e.b);
                    }
                auto join = [](std::vector<std::pair<int64_t, int64_t>>& v, int64_t a, int64_t b) {
                    if (!v.empty() && a <= v.back().second + 120) v.back().second = std::max(v.back().second, b);
                    else v.push_back({a, b});
                };
                for (const auto& [a, b, temp, f] : each) {
                    if (temp >= top) join(worst, a, b), fan = std::max(fan, f);
                    if (f >= Triggers().fan) join(loud, a, b);
                }
                auto spans = [](const std::vector<std::pair<int64_t, int64_t>>& v) {
                    std::vector<std::string> out;
                    for (const auto& [a, b] : v)
                        if (out.size() < 3) out.push_back((b - a < 60 ? "at " : "from ") + run_span(a, b));
                    if (v.size() > out.size()) out.push_back(unllm::number_word(long(v.size() - out.size())) + " more times");
                    return unllm::join_list(out);
                };
                if (top > 0 && !worst.empty())
                    res.body += " It was hottest " + spans(worst) + " (" + fmt("%.0f C", top) +
                                (fan >= 0 ? ", with the fan at up to " + fmt("%.0f%%", fan) : std::string()) + ").";
                if (!loud.empty()) res.body += " The fan was at " + fmt("%.0f%%", Triggers().fan) + " or more " + spans(loud) + ".";
            }
            // "why" and "which program" want the culprit first, then the evidence.
            if (!who.empty())
                res.body = (r.wants == unllm::Wants::Cause || r.wants == unllm::Wants::Name || ranking) && !polar
                               ? who + " " + res.body
                               : res.body + " " + who;
            // Reading against writing, from the per-second readings where they reach.
            if (lead == Intent::Disk) {
                auto rw = query(db_, "SELECT CAST(MIN(t) AS INTEGER), SUM(disk_r * dt), SUM(disk_w * dt) FROM (SELECT t, disk_r, disk_w, "
                                     "MIN(IFNULL(LEAD(t) OVER (ORDER BY t) - t, 1), 60) AS dt FROM samples WHERE t >= ?1 AND t < ?2)",
                                 {from, to});
                if (!rw.empty() && rw[0].t > 0)
                    res.body += (rw[0].t > from + 900 ? " From " + when_text(rw[0].t) + ", as far back as the per-second readings go,"
                                                      : std::string(" In that window")) +
                                " the disks read " + human_bytes(rw[0].a) + " and wrote " + human_bytes(rw[0].b) +
                                (rw[0].a > 4 * rw[0].b ? ": almost all of it reading." : rw[0].b > 4 * rw[0].a ? ": almost all of it writing." : ".");
                // Per program the recorder keeps one figure (events.cpp: read plus write), and a parent's holds its
                // finished children's too: no program's reads can be told from its writes.
                if (!who.empty())
                    res.body += " Each program's figure is its reads and writes together, a parent's with its finished child processes', "
                                "so the records can't say which program wrote the most or read the most.";
            }
            // The rule book gets a word: a "why" answer leads with its
            // conclusion; hardware findings are appended even unasked.
            if (card_.event_id) {
                Diagnose diag(db_);
                auto cons = diag.for_event(card_.event_id, now);
                if (!cons.empty() && cons[0].rule_id != "swap_parked") {
                    // A hardware fault or a recent change leads a why answer; a workload finding (a leak
                    // elsewhere) follows the evidence instead of standing in for the cause.
                    const bool off_topic = cons[0].rule_id == "leak" && lead != Intent::Memory;  // a leak explains memory, not heat
                    if (off_topic) {
                    } else if (r.wants == unllm::Wants::Cause && cons[0].salience < 2) {
                        res.body += " " + cons[0].sentence;
                    } else if (r.wants == unllm::Wants::Cause) {
                        res.lead = cons[0].sentence;
                        res.receipts.insert(res.receipts.begin(), cons[0].receipts.begin(), cons[0].receipts.end());
                    } else if (cons[0].salience >= 2.5) {
                        res.body += " " + cons[0].sentence;
                    }
                }
            }
            // Memory running out is what freezes this kind of machine: a why answer names the kills it caused.
            if (r.wants == unllm::Wants::Cause && (lead == Intent::Slow || lead == Intent::Memory)) {
                std::size_t total = 0;
                auto kills = oom_kills(db_, from, to, 2, &total);
                if (total > kills.size()) kills.push_back(std::to_string(total - kills.size()) + " more");
                if (!kills.empty()) res.body += " Memory ran out: the out-of-memory killer stopped " + unllm::join_list(kills) + ".";
            }
            add_receipts(in_window + " ORDER BY " + inside + " DESC", {from, to, int64_t(mask)}, cond_tb);
            return res;
        }
        if (polar || r.form == Form::Count) {
            res.body = unllm::realize({{{"matching event", false, 0}, "be", "recorded" +
                                        (reach.empty() ? std::string(" in that window") : reach), Tense::Past}});
            return res;
        }
        if (!told.empty()) {
            res.body = told;
            res.branch = std::string("mask:") + unllm::intent_name(lead) + ":readings";
            return res;
        }
        // The disks with no event in the window: what the per-second readings add up to and the busiest moment; whose it
        // was where the records can say (the per-minute lists keep the reads and writes of the recorder's own account's
        // programs); and the programs of other accounts that started then, whose disk use can't be read.
        if (lead == Intent::Disk) {
            auto rw = query(db_, "SELECT COUNT(*), SUM(disk_r * dt), SUM(disk_w * dt) FROM (SELECT t, disk_r, disk_w, "
                                 "MIN(IFNULL(LEAD(t) OVER (ORDER BY t) - t, 1), 60) AS dt FROM samples WHERE t >= ?1 AND t < ?2)", {from, to});
            auto top = query(db_, "SELECT CAST(t AS INTEGER), disk_r + disk_w FROM samples WHERE t >= ?1 AND t < ?2 ORDER BY 2 DESC, t LIMIT 1", {from, to});
            if (!rw.empty() && rw[0].t > 0 && !top.empty()) {
                const double busy = Triggers().disk_mbps * 1e6;
                std::string text = "Between " + span_text(from, to, " and ") + " the disks read " + human_bytes(rw[0].a) + " and wrote " + human_bytes(rw[0].b) +
                                   ", at most " + human_bytes(top[0].a) + "/s (at " + when_text(top[0].t) + ")" +
                                   (top[0].a < busy ? ", under the " + human_bytes(busy) + "/s the recorder counts as busy, so it kept no disk event." : ".");
                // The programs running at the busiest moment that had started in the window: the burst's likeliest owners.
                std::vector<std::string> at_peak;
                for (const auto& x : query(db_, "SELECT MIN(started), MAX(IFNULL(ended, ?3)), 0, name, MAX(cmdline) FROM procs WHERE started >= ?1 "
                                                "AND started <= ?2 + 60 AND IFNULL(ended, ?3) >= ?2 - 60 AND cpu_seconds >= 0.5 GROUP BY name "
                                                "ORDER BY 1 DESC LIMIT 2", {from, top[0].t, now}))
                    at_peak.push_back(shown_name(x.s1, x.s2) + ", which ran from " + clock_text(x.t) + " to " + clock_text(int64_t(x.a)));
                if (!at_peak.empty()) text += " The burst at " + clock_text(top[0].t) + " lines up with " + unllm::join_list(at_peak) + ".";
                std::vector<std::string> each;
                for (const auto& x : query(db_, "SELECT 0, SUM(io_bytes), 0, name, MAX(cmdline) FROM minute_top WHERE minute > ?1 - 60 AND minute < ?2 "
                                                "AND io_bytes > 0 GROUP BY name ORDER BY 2 DESC LIMIT 3", {from, to}))
                    each.push_back(shown_name(x.s1, x.s2) + " read and wrote " + human_bytes(x.a));
                text += each.empty() ? " None of your own programs, the only ones whose disk use the recorder can read, read or wrote much then"
                                     : " Of your own programs, the only ones whose disk use the recorder can read, " + unllm::join_list(each);
                text += "; the system's programs' disk use can't be read without administrator rights.";
                auto owner = query(db_, "SELECT 0, 0, 0, p.user FROM minute_top m JOIN procs p ON p.pid = m.pid AND p.started <= m.minute + 60 "
                                        "AND IFNULL(p.ended, m.minute) >= m.minute WHERE m.io_bytes > 0 GROUP BY p.user ORDER BY COUNT(*) DESC LIMIT 1", {});
                if (!owner.empty()) {
                    std::vector<std::string> others;
                    for (const auto& x : query(db_, "SELECT 0, 0, 0, name FROM procs WHERE started >= ?1 AND started < ?2 AND user <> ?3 AND cpu_seconds >= 0.5 "
                                                    "GROUP BY name ORDER BY MIN(started) LIMIT 4", {from, to}, {owner[0].s1}))
                        if (std::none_of(at_peak.begin(), at_peak.end(), [&](const std::string& p) { return p.rfind(x.s1, 0) == 0; })) others.push_back(x.s1);
                    if (!others.empty()) text += " Programs of the system and other accounts that started then: " + unllm::join_list(others) + ".";
                }
                res.body = text;
                res.branch = "mask:disk:readings";
                return res;
            }
        }
        res.body = unllm::realize({{subject, "stay", "quiet in that window: no matching events were recorded", Tense::Past}});
        return res;
    }

    std::size_t lead_rows = 0;  // the lines log_lead found
    auto log_lead =[&](const char* category, const char* noun) {
        // A when/last question reads the whole history and names the moment.
        int64_t lfrom = when_frame ? 0 : from, lto = when_frame ? now : to;
        auto rows = query(db_, std::string("SELECT t,0,0,title,detail FROM log WHERE category = '") + category +
                              "' AND t >= ? AND t < ? ORDER BY t DESC",
                          {lfrom, lto});
        if (!program.empty()) {
            std::vector<Row> kept;
            for (auto& row : rows)
                if (row.s1.find(program) != std::string::npos) kept.push_back(row);
            rows = kept;
        }
        // Sleep questions are about the wake and sleep lines only.
        const bool waking = has_word(tokens, {"wake", "woke"});
        const bool sleeping = has_word(tokens, {"suspend", "hibernate", "hibernation"}) ||
                              (has_word(tokens, {"sleep"}) && !waking);
        const bool lid = has_word(tokens, {"lid"});
        if (waking || sleeping || lid) {
            std::vector<Row> kept;
            for (auto& row : rows)
                if ((waking && row.s1.rfind("Woke", 0) == 0) || (sleeping && row.s1.rfind("Went to sleep", 0) == 0) ||
                    (lid && row.s1.rfind("Lid", 0) == 0))
                    kept.push_back(row);
            rows = kept;
        }
        lead_rows = rows.size();
        if (when_frame) {
            if (rows.empty()) {
                res.body = unllm::realize({{{noun, false, 0}, "be", "in the records", Tense::Present}});
            } else {
                res.body = "The last " + std::string(noun) + " was " + moment_text(rows[0].t) + ": " + rows[0].s1 + ".";
                for (const auto& row : rows)
                    if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
            }
            card_.moment = rows.empty() ? 0 : rows[0].t;
            res.branch = std::string("log:") + category;
            return;
        }
        // "What time did it wake up today": the moments, oldest first.
        if (r.wants == unllm::Wants::Time && r.window.stated) {
            if (rows.empty() && waking) {
                auto last = query(db_, "SELECT MAX(t) FROM log WHERE category = 'session' AND title LIKE 'Woke%' AND t < ?",
                                  {to});
                res.body = "It hasn't woken from sleep in that window" +
                           (!last.empty() && last[0].t > 0 ? ": it has been awake since " + moment_text(last[0].t)
                                                           : std::string()) + ".";
                if (!last.empty() && last[0].t > 0) card_.moment = last[0].t;
            } else if (rows.empty()) {
                res.body = unllm::realize({{{noun, false, 0}, "be", "recorded in that window", Tense::Past}});
            } else {
                std::vector<std::string> times;
                for (auto it = rows.rbegin(); it != rows.rend() && times.size() < 8; ++it) {
                    std::time_t tt = it->t;
                    std::tm tm{};
                    localtime_r(&tt, &tm);
                    char hm[8];
                    std::strftime(hm, sizeof hm, "%H:%M", &tm);
                    times.push_back(hm);
                }
                std::string what = waking ? "It woke from sleep" : sleeping ? "It went to sleep" : lid ? "The lid moved" :
                                   "The " + std::string(noun) + "s were";
                res.body = what + " at " + unllm::join_list(times) + " in that window.";
                card_.moment = rows[0].t;
            }
            for (const auto& row : rows)
                if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
            res.branch = std::string("log:") + category;
            return;
        }
        // "Three crashes were recorded ..." or "No crashes were recorded ...";
        // a polar question gets its yes or no in front.
        // Software changes are told by their packages, each with who started it; a yes-or-no question about them ("did
        // anything install itself") is answered by that, not by a bare yes, when every one told was started by a command.
        const bool software = std::string(category) == "software";
        std::vector<std::string> changes;
        bool started = software && !rows.empty();
        for (std::size_t i = 0; software && i < rows.size() && i < 3; ++i) {
            bool s = false;
            changes.push_back(software_change(db_, rows[i].t, rows[i].s1, rows[i].s2, &s));
            started = started && s;
        }
        Clause c{{noun, false, long(rows.size())}, "be", "recorded in that window", Tense::Past};
        if (polar && !started) polar_answer(res, r, !rows.empty(), {c});
        else res.body = unllm::realize({c});
        // Say what they were, newest first, each different line once, not just how many.
        {
            std::vector<std::string> named = changes;
            std::set<std::string> seen;
            for (const auto& row : rows) {
                if (software || named.size() == 3) break;
                if (seen.insert(row.s1).second) named.push_back(row.s1 + " (" + when_text(row.t) + ")");
            }
            if (!named.empty()) {
                if (!res.body.empty() && res.body.back() == '.') res.body.pop_back();
                // A software change is a sentence of its own ("Wed 20:40, installed a and b; removed c (...)").
                std::string list = unllm::join_list(named);
                if (software) {
                    list.clear();
                    for (const auto& x : named) list += (list.empty() ? "" : ". ") + x;
                }
                res.body += std::string(rows.size() > named.size() ? ". The latest: " : ": ") + list + ".";
            }
        }
        for (const auto& row : rows)
            if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
        // The programs those lines name ("ktorrent crashed"), so "why did it
        // crash" can point at them, or ask which one when there are two.
        std::vector<std::string> named;
        for (const auto& row : rows) {
            std::string first = row.s1.substr(0, row.s1.find(' '));
            if (!first.empty() && std::find(named.begin(), named.end(), first) == named.end() &&
                !query(db_, "SELECT COUNT(*) FROM procs WHERE name = ?", {}, {first}).empty() &&
                query(db_, "SELECT COUNT(*) FROM procs WHERE name = ?", {}, {first})[0].t > 0)
                named.push_back(first);
        }
        card_.subjects_named = named;
        if (!named.empty()) card_.subject = named[0];
        if (!rows.empty()) card_.moment = rows[0].t;
        res.branch = std::string("log:") + category;
    };

    switch (lead) {
    case Intent::Crash: log_lead("problems", "crash"); return res;
    case Intent::Startup: log_lead("startup", "startup change"); return res;
    case Intent::Update: log_lead("software", "software change"); return res;
    case Intent::Battery: log_lead("session", "power event"); return res;
    case Intent::Sharing: log_lead("network", "network event"); return res;
    case Intent::Devices:
        log_lead("devices", "device event");
        // None there: what the device records hold, so a Bluetooth link or a screen is not taken for absent.
        if (lead_rows == 0)
            res.notes.push_back("The device records hold USB devices, serial ports and drives plugged in or out; Bluetooth "
                                "connections and screens are not recorded.");
        return res;
    case Intent::Flash: log_lead("flashing", "flashing event"); return res;
    case Intent::Network: {
        std::string sql = "SELECT first_seen,0,0,program || ' connected to ' || remote || ':' || port AS d, program "
                          "FROM connections WHERE first_seen >= ? AND first_seen < ?";
        if (!program.empty()) sql += " AND program = ?";
        sql += " ORDER BY first_seen DESC";
        auto rows = query(db_, sql, {from, to}, program.empty() ? std::vector<std::string>{} : std::vector<std::string>{program});
        Clause c{{"new connection", false, long(rows.size())}, "be", "recorded in that window", Tense::Past};
        if (polar) polar_answer(res, r, !rows.empty(), {c});
        else res.body = unllm::realize({c});
        for (const auto& row : rows)
            if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
        return res;
    }
    case Intent::Trust: {
        // The weird scanner judges where a program came from. A path in the question,
        // or a named program's file from the records, gets weird's provenance report;
        // with nothing named, the running processes are scanned (about 30 s).
        auto quote = [](const std::string& x) {
            std::string q = "'";
            for (char c : x) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
            return q + "'";
        };
        std::string target;
        for (std::size_t at = 0; at < question.size(); ++at)
            if ((question[at] == '/' || (question[at] == '~' && at + 1 < question.size() && question[at + 1] == '/')) &&
                (at == 0 || question[at - 1] == ' ')) {
                std::size_t end = question.find_first_of(" ?,", at);
                target = question.substr(at, end == std::string::npos ? std::string::npos : end - at);
                if (target[0] == '~') target = std::string(getenv("HOME") ? getenv("HOME") : "") + target.substr(1);
                break;
            }
        std::string name = target.empty() ? program : target.substr(target.rfind('/') + 1);
        // "the arduino cli thing": nothing on record by that name, but a program installed on the PATH that never ran
        // (names.h, installed()): checked as that file, and said never to have run.
        bool never_ran = false;
        if (target.empty() && program.empty())
            if (const std::string found = names_.installed(question, r, now); !found.empty()) {
                target = found;
                name = program = found.substr(found.rfind('/') + 1);
                never_ran = true;
            }
        std::string unit;
        if (target.empty() && !program.empty()) {
            // The program's own processes first: a command that only names it ("pacman -Qo .../fan-control.sh") is not it.
            auto rows = query(db_, "SELECT 0,0,0,exe,cmdline FROM procs WHERE is_program(?1, name, exe, cmdline) ORDER BY name = ?1 DESC, started DESC LIMIT 1",
                              {}, {program});
            auto units = query(db_, "SELECT 0,0,0,unit FROM procs WHERE is_program(?1, name, exe, cmdline) ORDER BY name = ?1 DESC, started DESC LIMIT 1",
                               {}, {program});
            const std::string exe = rows.empty() ? "" : rows[0].s1, cmd = rows.empty() ? "" : rows[0].s2;
            unit = units.empty() ? "" : units[0].s1;
            // The file to check: the executable when it is the program; else the file on its command line that is (a
            // script bash or python runs; a root process's executable can't be read), else what systemd starts for its unit.
            auto is_it = [&](std::string_view p) {
                if (const auto eq = p.find('='); eq != std::string_view::npos) p = p.substr(eq + 1);  // ExecStart's "path=/..."
                return !p.empty() && p[0] == '/' && is_program(program, std::string(p.substr(p.rfind('/') + 1)), "", "") ? std::string(p) : std::string();
            };
            target = is_it(exe);
            for (const auto w : split_ws(cmd))
                if (target.empty()) target = is_it(w);
            if (target.empty() && !unit.empty()) {
                const std::string start = run_out("systemctl show -p ExecStart --value " + quote(unit) + " 2>/dev/null; systemctl --user show -p ExecStart --value " +
                                                  quote(unit) + " 2>/dev/null");
                for (const auto w : split_ws(start))
                    if (target.empty()) target = is_it(w);
            }
            if (target.empty()) target = exe;
        }
        // Whether it runs now, as whom and in which unit. weird looks for the file as a running executable, so a
        // script a shell runs reads as "no process" there.
        std::string running;
        if (!program.empty()) {
            if (!sampler_) sampler_ = std::make_unique<Sampler>();
            const SnapshotPtr s = sampler_->sample();
            const ProcessInfo* live = nullptr;
            for (const auto& p : s->procs)
                if (is_program(program, p.name, p.exe, p.cmdline) && (!live || (p.name == program && live->name != program))) live = &p;
            if (live) {
                const std::string u = live->unit.empty() ? unit : live->unit;
                std::string about = u.empty() ? "" : trim(run_out("systemctl show -p Description --value " + quote(u) + " 2>/dev/null"));
                if (about.empty() || about == u) about = u.empty() ? "" : trim(run_out("systemctl --user show -p Description --value " + quote(u) + " 2>/dev/null"));
                running = " It is running now as " + (live->user.empty() ? "user " + std::to_string(live->uid) : live->user) +
                          (u.empty() ? "" : ", under " + u + (about.empty() || about == u ? "" : " (" + about + ")")) + ".";
                res.receipts.push_back("Running: pid " + std::to_string(live->key.pid) + " as " + live->user + (u.empty() ? "" : " in " + u));
            } else {
                running = " It isn't running now.";
            }
            auto net = query(db_, "SELECT COUNT(*), COUNT(DISTINCT remote) FROM connections WHERE is_program(?, program, IFNULL(exe, ''), '')", {}, {program});
            running += !net.empty() && net[0].t > 0 ? " The records show it connecting to " + std::to_string(int64_t(net[0].a)) +
                                                          (net[0].a == 1 ? " place." : " places.")
                                                    : " The records show no network connections from it.";
        }
        if (never_ran) {
            auto first = query(db_, "SELECT CAST(MIN(started) AS INTEGER) FROM procs", {});
            running = " It hasn't run on this machine as far as the records go" +
                      (first.empty() || first[0].t == 0 ? std::string(".") : ", back to " + moment_text(first[0].t) + ".") + running;
        }
        res.branch = "trust";
        // A folder ("the .AppImage in ~/Downloads"): the file in it whose name holds a word of the question.
        if (!target.empty() && std::filesystem::is_directory(target)) {
            std::vector<std::string> hits;
            for (const auto& f : list_dir(target)) {
                std::string low = f;
                for (auto& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
                for (const auto& t : tokens)
                    if (t.lower.size() >= 4 && low.find(t.lower) != std::string::npos && target.find(t.lower) == std::string::npos) {
                        hits.push_back(f);
                        break;
                    }
            }
            if (hits.size() == 1) {
                target += "/" + hits[0];
                name = hits[0];
            } else if (hits.size() > 1) {
                if (hits.size() > 5) hits.resize(5);
                res.body = "Which one? " + target + " has " + unllm::join_list(hits) + ". Ask about one by its path and I'll check where it came from.";
                res.branch = "trust:which-file";
                return res;
            }
        }
        // "is pid 2310407 weird": weird's report on that one process (its verdict and where it came from), or the records
        // on the number when nothing runs under it now.
        std::string pid_s;
        for (const auto& e : r.entities)
            if (e.type == "pid") pid_s = e.value;
        if (target.empty() && program.empty() && !pid_s.empty()) {
            const std::string out = run_out("weird --offline --no-prompt " + pid_s + " 2>/dev/null");
            // Paragraphs, their wrapped lines joined: the first is the verdict ("plasmashell (pid 2310407) checks out: ..."),
            // the one after the "Where it came from" heading says why.
            std::vector<std::string> paras;
            {
                std::istringstream in(out);
                std::string cur;
                for (std::string line; std::getline(in, line);) {
                    const std::string l = trim(line);
                    if (l.empty()) {
                        if (!cur.empty()) paras.push_back(cur);
                        cur.clear();
                    } else {
                        cur += (cur.empty() ? "" : " ") + l;
                    }
                }
                if (!cur.empty()) paras.push_back(cur);
            }
            std::string verdict, origin, signs;
            for (std::size_t i = 0; i < paras.size(); ++i) {
                if (i == 0 && paras[i].find("(pid " + pid_s + ")") != std::string::npos) verdict = paras[i];
                // A heading and its text share a paragraph ("Where it came from It comes from the ... package").
                if (const char* head = "Where it came from"; paras[i].rfind(head, 0) == 0) origin = trim(paras[i].substr(std::strlen(head)));
                if (const char* head = "Warning signs"; paras[i].rfind(head, 0) == 0) {
                    signs = trim(paras[i].substr(std::strlen(head)));
                    for (std::size_t at; (at = signs.find("- ")) != std::string::npos;) signs.erase(at, 2);  // the bullets
                }
            }
            if (verdict.empty()) {
                auto rec = query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, 0), 0, name, cmdline FROM procs WHERE pid = ?1 ORDER BY started DESC LIMIT 1",
                                 {std::atoll(pid_s.c_str())});
                res.body = "No process " + pid_s + " is running now for weird to check." +
                           (rec.empty() ? " The records have no process by that number either."
                                        : " The records show pid " + pid_s + " was " + rec[0].s1 + ", which ran " +
                                              (rec[0].a > 0 ? span_text(rec[0].t, int64_t(rec[0].a), " to ") : "from " + moment_text(rec[0].t)) + ".");
            } else {
                res.body = verdict + (signs.empty() ? "" : " " + signs) + (origin.empty() ? "" : " " + origin);
                res.notes.push_back("weird rates where a program came from, not what it does.");
            }
            res.receipts.push_back("weird " + pid_s);
            res.branch = "trust:pid";
            return res;
        }
        // "is this program safe" with nothing named: ask which, rather than scanning everything.
        if (target.empty() && program.empty() && kind_ == "trust.identity") {
            res.body = "Which program do you mean? Name it, or give its path (like \"is ~/Downloads/tool.AppImage safe\"), and I'll check where it came from with weird.";
            res.branch = "trust:which";
            return res;
        }
        if (!target.empty()) {
            const std::string out = run_out("weird --offline --no-prompt " + quote(target) + " 2>/dev/null");
            auto field = [&](const char* key) {
                const auto at = out.find(std::string(key) + ":");
                if (at == std::string::npos) return std::string();
                std::string v = out.substr(at + std::strlen(key) + 1, out.find('\n', at) - at - std::strlen(key) - 1);
                return v.substr(std::min(v.size(), v.find_first_not_of(' ')));
            };
            static const std::map<std::string, std::string> plain = {
                {"OFFICIAL", "from the distribution's official packages"}, {"OFFICIAL_REPO", "from the distribution's official packages"},
                {"THIRD_PARTY", "from a third-party package repository"}, {"THIRD_PARTY_REPO", "from a third-party package repository"},
                {"FLATPAK", "a Flatpak"}, {"SNAP", "a Snap"}, {"APPIMAGE", "an AppImage, not from any package"},
                {"USER_LOCAL", "a file put there by hand, not from any package"},
                {"USER_PKG", "installed by a language package manager rather than the system's"},
                {"LOCAL_BUILD", "built on this machine"}, {"CONTAINER", "inside a container"},
                {"USER_TRUSTED", "one you marked as trusted"}, {"KERNEL", "part of the kernel"},
                {"PUBLISHER_SIGNED", "signed by its publisher"}, {"PUBLISHER_HOSTED", "a match for its publisher's own download"},
                {"UNKNOWN", "of unknown origin"}};
            const std::string trust = field("Trust");
            if (trust.empty()) {
                res.body = "weird couldn't examine " + target + (out.empty() ? " (is weird installed?)." : ".") + running;
                return res;
            }
            auto it = plain.find(trust);
            res.body = name + " (" + target + ") is " + (it != plain.end() ? it->second : "rated " + trust) + ".";
            const std::string pkg = field("Package"), integ = field("Integrity");
            if (!pkg.empty() && pkg != "-") res.body += " Package: " + pkg + ".";
            if (integ.rfind("unavailable", 0) == 0) res.body += " Its files can't be checked against install-time hashes.";
            else if (!integ.empty()) res.body += " File check: " + integ + ".";
            res.body += running;
            res.notes.push_back("weird rates where a program came from, not what it does.");
            for (const char* k : {"Trust", "Package", "Manager", "Integrity", "Running"})
                if (!field(k).empty() && (program.empty() || std::string(k) != "Running")) res.receipts.push_back(std::string(k) + ": " + field(k));
            return res;
        }
        const std::string out = run_out("weird --offline --no-prompt --skip aur --skip persist --skip kernel --skip stale 2>/dev/null");
        if (out.empty()) {
            res.body = "The weird scanner didn't run (is it installed?).";
            return res;
        }
        // weird's report: a summary line, then under headings ("Act now", "Worth a look") each finding as a title line
        // indented two spaces and its explanation wrapped under it at four; the "Everything else" section closes it. The
        // first two findings are quoted whole, title and explanation.
        std::istringstream lines(out);
        std::string first, line;
        std::getline(lines, first);
        res.body = "I scanned the running programs with weird: " + first;
        std::vector<std::string> findings;
        std::string title, expl;
        bool in_rest = false;
        auto flush = [&] {
            if (!title.empty()) findings.push_back(title + (expl.empty() ? "" : ": " + expl));
            title.clear();
            expl.clear();
        };
        while (std::getline(lines, line)) {
            if (line.rfind("Everything else", 0) == 0) {
                flush();
                in_rest = true;
            }
            if (in_rest) {
                const auto at = line.find("Running programs");
                if (at != std::string::npos) res.receipts.push_back(trim(line.substr(at + 16)));
                else if (!trim(line).empty() && res.receipts.size() < 6 && line.find("weird -v") == std::string::npos) res.receipts.push_back(trim(line));
            } else if (trim(line).empty()) {
                flush();
            } else if (line.rfind("    ", 0) == 0) {
                expl += (expl.empty() ? "" : " ") + trim(line);
            } else if (line.rfind("  ", 0) == 0) {
                flush();
                title = trim(line);
            }
        }
        flush();
        for (std::size_t i = 0; i < findings.size() && i < 2; ++i) res.body += " " + findings[i] + (findings[i].back() == '.' ? "" : ".");
        if (findings.size() > 2) res.body += " " + std::to_string(findings.size() - 2) + " more in weird's own report.";
        return res;
    }
    case Intent::Config:
        return answer_records("config.list.window", r, now);
    default: break;
    }

    // "When did blender last run": the most recent start in the history.
    if (when_frame && !program.empty()) {
        auto rows = query(db_, "SELECT started,0,0,name FROM procs WHERE is_program(?, name, exe, cmdline) ORDER BY started DESC LIMIT 3",
                          {}, {program});
        if (rows.empty()) {
            Clause c{{program, true}, "run", "in the 30 days of records", Tense::Past};
            c.negated = true;
            res.body = unllm::realize({c});
        } else {
            res.body = unllm::realize({{{program, true}, "run", "last on " + moment_text(rows[0].t), Tense::Past}});
            for (const auto& row : rows) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
        }
        return res;
    }

    // "Did blender run yesterday" and "how many times?": the program's
    // existence, as a yes/no or a count.
    if ((polar || r.form == Form::Count) && !program.empty()) {
        // "is ollama still running since it got killed": the processes with no end in the records, and the event the
        // question named.
        if (polar && event_at_ && r.event_after && to >= now - 120) {
            auto alive = query(db_, "SELECT CAST(started AS INTEGER), pid, 0, name FROM procs WHERE ended IS NULL AND started < ?1 "
                                    "AND is_program(?2, name, exe, cmdline) ORDER BY started DESC",
                               {to}, {program});
            const std::string event = r.event_clause + " at " + clock_text(event_at_);
            if (!alive.empty()) {
                const Row& p = alive.front();
                settle(res, r, true);
                res.body = program + " is still running: " + p.s1 + " (PID " + std::to_string(int64_t(p.a)) + ") has run since " +
                           moment_text(p.t) +
                           (p.t < event_at_         ? ", from before " + event
                            : p.t - event_at_ < 60 ? ", " + std::to_string(p.t - event_at_) + " seconds after " + event
                                                   : ", " + duration_text(p.t - event_at_) + " after " + event) +
                           ".";
                if (alive.size() > 1) res.body += " " + std::to_string(alive.size() - 1) + " more of its processes are running too.";
                res.body[0] = char(std::toupper(static_cast<unsigned char>(res.body[0])));
            } else {
                auto last = query(db_, "SELECT CAST(MAX(ended) AS INTEGER) FROM procs WHERE is_program(?1, name, exe, cmdline)", {}, {program});
                settle(res, r, false);
                res.body = program + " isn't running" +
                           (!last.empty() && last[0].t > 0 ? ": its last process ended " + moment_text(last[0].t) : std::string()) +
                           " (" + event + ").";
                res.body[0] = char(std::toupper(static_cast<unsigned char>(res.body[0])));
            }
            add_receipts("SELECT started,0,0,name || '  pid ' || pid || CASE WHEN ended IS NULL THEN '  running' ELSE '  ended' END "
                         "FROM procs WHERE started < ? AND IFNULL(ended, ?) >= ? AND is_program(?, name, exe, cmdline) ORDER BY started DESC",
                         {to, now, from - 1}, {program});
            card_.subject = program;
            if (!alive.empty()) card_.moment = alive.front().t;
            res.branch = "program:running";
            return res;
        }
        auto rows = query(db_, "SELECT COUNT(*), 0, 0 FROM procs WHERE started < ? AND IFNULL(ended, ?) >= ? AND is_program(?, name, exe, cmdline)",
                          {to, now, from}, {program});
        int64_t n = rows.empty() ? 0 : rows[0].t;
        Clause c{{program, true}, "run", "", Tense::Past};
        if (n > 0) {
            c.rest = n == 1 ? "once in that window" : unllm::number_word(n) + " times in that window";
            if (polar) polar_answer(res, r, true, {c});
            else res.body = unllm::realize({c});
        } else {
            c.negated = true;
            c.rest = "in that window";
            if (polar) polar_answer(res, r, false, {c});
            else res.body = unllm::realize({c});
        }
        add_receipts("SELECT started,0,0,name || '  pid ' || pid FROM procs WHERE started < ? AND IFNULL(ended, ?) >= ? AND is_program(?, name, exe, cmdline) ORDER BY started DESC",
                     {to, now, from}, {program});
        return res;
    }

    // "What did blender do yesterday": the program's own story, from its
    // runs and the resource events it was part of.
    if (!program.empty()) {
        card_.subject = program;
        card_.subjects_named = {program};
        auto runs = query(db_, "SELECT COUNT(*), SUM(cpu_seconds), MAX(peak_rss) FROM procs "
                               "WHERE started < ? AND IFNULL(ended, ?) >= ? AND is_program(?, name, exe, cmdline)",
                          {to, now, from}, {program});
        auto evs = query(db_, "SELECT COUNT(*), MAX(e.peak_temp), MAX(e.start), e.summary FROM events e "
                              "JOIN event_culprits c ON c.event_id = e.id WHERE e.start < ? AND e.\"end\" >= ? AND is_program(?, c.name, '', c.cmdline)",
                         {to, from}, {program});
        int64_t n = runs.empty() ? 0 : runs[0].t;
        int64_t ne = evs.empty() ? 0 : evs[0].t;
        if (n > 0 || ne > 0) {
            std::vector<Clause> cl;
            if (n > 0) {
                std::string rest = (n == 1 ? std::string("once") : unllm::number_word(n) + " times") + " in that window";
                cl.push_back({{program, true}, "run", rest, Tense::Past});
                if (runs[0].a >= 60) cl.push_back({{program, true}, "use", duration_text(int64_t(runs[0].a)) +
                                                                                " of processor time", Tense::Past});
                if (runs[0].b > 0) cl.push_back({{program, true}, "peak", "at " + human_bytes(runs[0].b) + " of memory",
                                                 Tense::Past});
            }
            if (ne > 0) {
                std::string rest = "a culprit in " + unllm::number_word(ne) + " resource " + unllm::plural_noun("event", ne);
                if (evs[0].a > 0) rest += fmt(" (hottest %.0f C)", evs[0].a);
                cl.push_back({{program, true}, "be", rest, Tense::Past});
                card_.moment = int64_t(evs[0].b);
            }
            res.body = unllm::realize(cl);
            // An interpreter is only a vehicle ("the python thing"): its runs by the script they ran, as the names
            // module reads it from the command line, the most runs first, with when they ran.
            std::map<std::string, int64_t> by_script;
            names_.gather(from, to);
            for (const auto& c : names_.candidates())
                if (!c.script.empty() && is_program(program, c.name, c.exe, "")) by_script[c.script] += c.runs;
            std::vector<std::pair<int64_t, std::string>> order;
            for (const auto& [script, runs] : by_script) order.push_back({runs, script});
            std::sort(order.rbegin(), order.rend());
            std::vector<std::string> said;
            for (const auto& [runs, script] : order) {
                if (said.size() == 3) break;
                auto span = query(db_, "SELECT CAST(MIN(started) AS INTEGER), MAX(IFNULL(ended, ?1)), COUNT(*), MAX(cmdline) FROM procs "
                                       "WHERE started < ?2 AND IFNULL(ended, ?1) >= ?3 AND is_program(?4, name, exe, cmdline) "
                                       "AND is_program(?5, name, exe, cmdline)",
                                  {now, to, from}, {program, script});
                if (span.empty() || span[0].b <= 0) continue;
                double cpu = 0;
                for (const auto& x : minute_cpu(db_, from, to, "is_program(?4, mt.name, '', mt.cmdline) AND is_program(?5, mt.name, '', mt.cmdline)",
                                                {program, script}, "''", 1))
                    cpu += x.a;
                const int64_t count = int64_t(span[0].b);
                std::string line = script;
                if (said.empty()) {
                    std::istringstream words(span[0].s1);
                    std::string w, head;
                    for (int i = 0; i < 4 && words >> w; ++i) head += (head.empty() ? "" : " ") + w;
                    line += " (" + head + (head.size() < span[0].s1.size() ? " ..." : "") + ")";
                }
                const bool running = count == 1 && int64_t(span[0].a) >= now;
                line += ": " + (count == 1 ? std::string("one run") : unllm::number_word(long(count)) + " runs") + ", " +
                        (running ? "running since " + when_text(span[0].t)
                                 : (count == 1 ? "from " : "between ") + when_text(span[0].t) + (count == 1 ? " to " : " and ") +
                                       when_text(int64_t(span[0].a)).substr(4)) +
                        (cpu >= 60 ? ", " + duration_text(int64_t(cpu)) + " of CPU" : "");
                said.push_back(line);
            }
            if (!said.empty()) {
                std::string all;
                for (const auto& s : said) all += (all.empty() ? "" : "; ") + s;
                res.body = "That is " + program + " running " + (said.size() == 1 ? "" : "scripts: ") + all + ". " + res.body;
            }
            add_receipts("SELECT e.start,0,0,e.summary FROM events e JOIN event_culprits c ON c.event_id = e.id "
                         "WHERE e.start < ? AND e.\"end\" >= ? AND is_program(?, c.name, '', c.cmdline) ORDER BY c.share DESC",
                         {to, from}, {program});
            if (res.receipts.empty())
                add_receipts("SELECT started,0,0,name || '  pid ' || pid FROM procs WHERE started < ? AND "
                             "IFNULL(ended, ?) >= ? AND is_program(?, name, exe, cmdline) ORDER BY started DESC",
                             {to, now, from}, {program});
            res.branch = "program:story";
            return res;
        }
    }

    // The story: whatever the records say, ranked by relevance. A summary
    // request skips straight to the paragraph.
    auto hits = r.frame == unllm::Frame::Summary ? std::vector<unllm::Hit>{} : index_.search(search_terms, from, to, 6);
    if (!hits.empty()) {
        res.body = unllm::realize({{{"record", false, long(hits.size())}, "match", "in that window", Tense::Past}});
        for (const auto& h : hits) res.receipts.push_back(when_text(h.t) + "  " + h.text);
        return res;
    }
    // The tell-everything fallback, only for questions that named nothing
    // searchable ("what happened last night"): a question that named
    // something gets an honest no instead of a data dump. The timeline view
    // joins events, log, programs and connections; resource events outrank
    // the rest so a near-shutdown never hides under program starts.
    if (!search_terms.empty() && r.frame != unllm::Frame::Summary) {
        res.body = unllm::realize({{{"record", false, 0}, "match", "that question in that window", Tense::Past}});
        return res;
    }
    auto rows = query(db_,
                      "SELECT t,0,0,title,kind FROM v_timeline WHERE t >= ? AND t < ? "
                      "ORDER BY CASE WHEN kind = 'resource event' THEN 0 WHEN kind LIKE 'log:%' THEN 1 "
                      "WHEN kind = 'first connection' THEN 2 ELSE 3 END, t DESC LIMIT 6",
                      {from, to});
    if (rows.empty()) {
        res.body = unllm::realize({{{"record", false, 0}, "match", "that question in that window", Tense::Past}});
        return res;
    }
    // The story as a paragraph: what kinds of things happened, the worst of
    // each, and the top finding, with the timeline rows as receipts.
    std::vector<std::string> extra;
    res.body = story_summary(from, to, now, &extra);
    for (const auto& row : rows)
        if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
    res.branch = "story:summary";
    return res;
}

std::string Ask::story_summary(int64_t from, int64_t to, int64_t now, std::vector<std::string>* receipts) {
    struct Kind { unsigned mask; const char* noun; const char* metric; const char* unit; };
    static const Kind kinds[] = {
        {16, "heat event", "peak_temp", " C"}, {1, "slowdown", "peak_cpu", "%"},
        {2, "memory event", "peak_mem", "%"},  {4, "disk event", "peak_disk", ""},
        {8, "GPU event", "peak_gpu", "%"},
    };
    std::vector<std::string> parts;
    for (const auto& k : kinds) {
        auto agg = query(db_, std::string("SELECT COUNT(*), MAX(CASE WHEN ") + k.metric + " >= 0 THEN " + k.metric +
                                  " END), 0 FROM events WHERE start < ? AND \"end\" >= ? AND (kinds & ?) != 0",
                         {to, from, int64_t(k.mask)});
        int64_t n = agg.empty() ? 0 : agg[0].t;
        if (n <= 0) continue;
        std::string worst = k.mask == 4 ? human_bytes(agg[0].a) + "/s" : fmt("%.0f", agg[0].a) + k.unit;
        std::string culprit = top_culprit(db_, from, to, k.mask, "");
        parts.push_back(unllm::number_word(n) + " " + unllm::plural_noun(k.noun, n) + " (worst " + worst +
                        (culprit.empty() ? "" : ", mostly " + culprit) + ")");
    }
    // Stops without a clean shutdown (the recorder's shutdown events, from how each boot's log ended), by their times.
    {
        std::vector<std::string> at;
        for (const auto& e : query(db_, "SELECT CAST(start AS INTEGER) FROM events WHERE start >= ? AND start < ? AND (kinds & ?) != 0 ORDER BY start",
                                   {from, to, int64_t(KindShutdown)}))
            at.push_back(when_text(e.t));
        if (!at.empty())
            parts.push_back((at.size() == 1 ? std::string("a stop") : unllm::number_word(int64_t(at.size())) + " stops") + " without a clean shutdown (" +
                            unllm::join_list(at) + ")");
    }
    // The log's rows, named rather than counted: rows of one shape (their numbers aside) are one entry with its
    // count, times and the range of each number; a failed unit is named from its journal line, as a person reads it.
    // A row the logbook folded ("ask crashed 2 times between 14:38:27 and 14:38:32") follows the row it repeats, so it
    // adds its count less one. A session row (the power button, the lid, sleep, logging in and out, the charger: what
    // the person did) is said by its head ("Logged in: 'x' with class 'greeter' ..." is "Logged in"), and the rows no
    // more than a minute apart around it are one moment. At most 20 entries, by category in this order, then told in
    // time order.
    struct Entry {
        std::string key, text;
        int64_t first = 0, last = 0, n = 0;
        std::vector<std::pair<std::string, std::string>> nums;  // each number's lowest and highest
        std::size_t rank = 0;                                    // its category's place in the order
    };
    auto numbers = [](const std::string& s) {
        std::vector<std::string> out;
        for (std::size_t i = 0, j = 0; i < s.size(); i = std::max(i + 1, j)) {
            for (j = i; j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])); ++j) {}
            if (j > i) out.push_back(s.substr(i, j - i));
        }
        return out;
    };
    auto below = [](const std::string& a, const std::string& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; };
    static const char* const order[] = {"problems", "services", "software", "devices", "flashing", "session", "startup"};
    const std::size_t session = 5;  // order[5]
    auto shape = [](const std::string& head, const std::string& text) {
        std::string key = head + ":";
        for (std::size_t i = 0; i < text.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(text[i]))) key += text[i];
            else if (i == 0 || !std::isdigit(static_cast<unsigned char>(text[i - 1]))) key += '#';
        return key;
    };
    struct Line {
        int64_t t = 0, n = 1;
        std::size_t rank = 0;
        std::string text, key;
    };
    auto line_of = [&](std::size_t rank, const Row& row) {
        Line l{row.t, 1, rank, row.s1, ""};
        if (const auto at = l.text.find(" times between "); at != std::string::npos && at > 0)
            if (const auto sp = l.text.rfind(' ', at - 1); sp != std::string::npos) {
                l.n = std::max<int64_t>(0, std::atoll(l.text.c_str() + sp + 1) - 1);
                l.text = l.text.substr(0, sp);
            }
        if (std::strcmp(order[rank], "services") == 0) {
            // "<unit> failed: Failed with result 'oom-kill'." -> "<unit> failed (oom-kill)"
            const auto q1 = l.text.find('\''), q2 = q1 == std::string::npos ? q1 : l.text.find('\'', q1 + 1);
            l.text = unit_name(failed_unit(row.s1, row.s2)) + " failed" + (q2 == std::string::npos ? "" : " (" + l.text.substr(q1 + 1, q2 - q1 - 1) + ")");
        } else if (rank == session) {
            l.text = l.text.substr(0, std::min(l.text.find(':'), l.text.find(" (")));
        }
        l.key = shape(order[rank], l.text);
        return l;
    };
    // Routine rows are left out: a shape logged on at least half of the days the log holds in the two weeks before
    // the window (when it holds four days or more), such as Wi-Fi rewriting resolv.conf or a program updating itself.
    // The session's rows stay: what the person did frames the moments.
    std::set<std::string> routine;
    {
        std::set<std::string> days;
        std::map<std::string, std::set<std::string>> on;
        for (std::size_t c = 0; c < std::size(order); ++c)
            for (const auto& row : query(db_, std::string("SELECT CAST(t AS INTEGER), 0, 0, title, detail FROM log WHERE category = '") + order[c] +
                                                  "' AND t >= ? AND t < ?", {from - 14 * 86400, from})) {
                days.insert(local_date(row.t));
                if (c != session) on[line_of(c, row).key].insert(local_date(row.t));
            }
        if (days.size() >= 4)
            for (const auto& [key, d] : on)
                if (2 * d.size() >= days.size()) routine.insert(key);
    }
    std::vector<Line> lines;
    for (std::size_t c = 0; c < std::size(order); ++c)
        for (const auto& row : query(db_, std::string("SELECT CAST(t AS INTEGER), 0, 0, title, detail FROM log WHERE category = '") + order[c] +
                                              "' AND t >= ? AND t < ? ORDER BY t", {from, to}))
            if (Line l = line_of(c, row); !routine.count(l.key)) lines.push_back(l);
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.t < b.t; });
    std::vector<Entry> entries;
    auto add = [&](const std::string& key, const std::string& text, int64_t first, int64_t last, int64_t n, std::size_t rank) {
        const auto nums = numbers(text);
        auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.key == key; });
        if (it == entries.end()) {
            entries.push_back({key, text, first, last, n, {}, rank});
            for (const auto& x : nums) entries.back().nums.emplace_back(x, x);
        } else {
            it->last = last, it->n += n;
            for (std::size_t k = 0; k < nums.size() && k < it->nums.size(); ++k) {
                if (below(nums[k], it->nums[k].first)) it->nums[k].first = nums[k];
                if (below(it->nums[k].second, nums[k])) it->nums[k].second = nums[k];
            }
        }
    };
    for (std::size_t i = 0, j = 0; i < lines.size(); i = j) {
        bool moment = false;
        for (j = i; j < lines.size() && (j == i || lines[j].t - lines[j - 1].t <= 60); ++j) moment = moment || lines[j].rank == session;
        if (!moment) {
            for (std::size_t k = i; k < j; ++k) add(lines[k].key, lines[k].text, lines[k].t, lines[k].t, lines[k].n, lines[k].rank);
            continue;
        }
        // The moment's heads in order, each once, without their particulars ("X failed (exit-code)" is "X failed").
        // Two-word heads that share their first word join on it ("logged out", "logged in": "logged out and in"),
        // those that share their last word join on that ("X failed", "Y failed": "X and Y failed").
        struct Group {
            std::string head, a, b, shared;
            std::vector<std::string> parts;
            int by = 0;  // 0: one head; 1: the first word shared; 2: the last
        };
        std::vector<Group> groups;
        std::set<std::string> seen;
        for (std::size_t k = i; k < j; ++k) {
            std::string h = lines[k].text.substr(0, lines[k].text.find(" ("));
            if (h.empty() || !seen.insert(h).second) continue;
            h[0] = char(std::tolower(static_cast<unsigned char>(h[0])));
            const auto sp = h.find(' ');
            const bool two = sp != std::string::npos && h.find(' ', sp + 1) == std::string::npos;
            const std::string a = two ? h.substr(0, sp) : "", b = two ? h.substr(sp + 1) : "";
            bool joined = false;
            for (auto& g : groups) {
                if (!two || joined) break;
                if ((g.by == 1 && g.shared == a) || (g.by == 0 && !g.a.empty() && g.a == a)) {
                    if (g.by == 0) g.by = 1, g.shared = a, g.parts = {g.b};
                    g.parts.push_back(b);
                    joined = true;
                } else if ((g.by == 2 && g.shared == b) || (g.by == 0 && !g.b.empty() && g.b == b)) {
                    if (g.by == 0) g.by = 2, g.shared = b, g.parts = {g.a};
                    g.parts.push_back(a);
                    joined = true;
                }
            }
            if (!joined) groups.push_back({h, a, b, "", {}, 0});
        }
        std::string text;
        for (const auto& g : groups)
            text += (text.empty() ? "" : ", then ") +
                    (g.by == 1 ? g.shared + " " + unllm::join_list(g.parts) : g.by == 2 ? unllm::join_list(g.parts) + " " + g.shared : g.head);
        add(shape("moment", text), text, lines[i].t, lines[j - 1].t, 1, session);
    }
    // An entry's text with each number that varied shown as its range ("at 12-100% battery").
    auto shown = [](const Entry& e) {
        std::string out;
        std::size_t k = 0;
        for (std::size_t i = 0; i < e.text.size();) {
            std::size_t j = i;
            while (j < e.text.size() && std::isdigit(static_cast<unsigned char>(e.text[j]))) ++j;
            if (j == i) {
                out += e.text[i++];
                continue;
            }
            const auto& [lo, hi] = e.nums[k++];
            out += lo == hi ? lo : lo + "-" + hi;
            i = j;
        }
        return out;
    };
    const std::size_t more = entries.size() > 20 ? entries.size() - 20 : 0;
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.rank < b.rank; });
    entries.resize(entries.size() - more);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.first < b.first; });
    auto clock = [](int64_t t) {
        std::time_t tt = t;
        std::tm tm{};
        localtime_r(&tt, &tm);
        char buf[8];
        std::strftime(buf, sizeof buf, "%H:%M", &tm);
        return std::string(buf);
    };
    auto times_text = [&](int64_t first, int64_t last) {
        const bool same_day = local_date(first) == local_date(last);
        return " (" + when_text(first) + (same_day && clock(last) == clock(first) ? "" : " to " + (same_day ? clock(last) : when_text(last))) + ")";
    };
    std::string logged;
    for (const auto& e : entries) logged += (logged.empty() ? "" : "; ") + shown(e) + (e.n > 1 ? " " + std::to_string(e.n) + " times" : "") + times_text(e.first, e.last);
    if (more) logged += "; and " + std::to_string(more) + " more";
    // Programs that kept starting again: a run of a long-lived program (its runs in the week before lasted an hour or
    // more, half of them) that began within a minute of the end of the one before it in the same unit, three times
    // or more. Programs started again at the same moments are told together.
    struct Restart {
        std::string names;
        int64_t n = 0, first = 0, last = 0;
    };
    std::vector<Restart> restarts;
    {
        std::map<std::pair<std::string, std::string>, Restart> by;
        for (const auto& x : query(db_, "SELECT CAST(started AS INTEGER), prev, 0, name, unit FROM (SELECT name, unit, started, "
                                        "LAG(IFNULL(ended, ?3)) OVER (PARTITION BY name, unit ORDER BY started) AS prev FROM procs "
                                        "WHERE started < ?2 AND IFNULL(ended, ?3) >= ?1 - 60 AND IFNULL(unit, '') != '') "
                                        "WHERE started >= ?1 AND prev IS NOT NULL AND ABS(started - prev) <= 60 ORDER BY started",
                                   {from, to, now})) {
            Restart& r = by[{x.s1, x.s2}];
            if (r.n++ == 0) r.names = x.s1, r.first = x.t;
            r.last = x.t;
        }
        std::vector<Restart> kept;
        for (const auto& [who, r] : by) {
            if (r.n < 3) continue;
            std::vector<double> runs;
            for (const auto& x : query(db_, "SELECT 0, ended - started FROM procs WHERE name = ?3 AND unit = ?4 AND ended IS NOT NULL AND ended <= ?1 "
                                            "AND started >= ?2", {from, from - 7 * 86400}, {who.first, who.second}))
                runs.push_back(x.a);
            std::sort(runs.begin(), runs.end());
            if (!runs.empty() && runs[runs.size() / 2] >= 3600) kept.push_back(r);
        }
        std::sort(kept.begin(), kept.end(), [](const Restart& a, const Restart& b) { return a.first < b.first; });
        for (const auto& r : kept) {
            auto same = std::find_if(restarts.begin(), restarts.end(), [&](const Restart& o) {
                return o.n == r.n && std::llabs(o.first - r.first) <= 60 && std::llabs(o.last - r.last) <= 60;
            });
            if (same != restarts.end()) same->names += " and " + r.names;
            else restarts.push_back(r);
        }
    }
    std::vector<std::string> restarted;
    for (const auto& r : restarts) restarted.push_back(r.names + " restarted " + std::to_string(r.n) + " times" + times_text(r.first, r.last));
    // Memory at its limit: the swap at its size (a reading within 0.1% of it).
    std::string full;
    {
        double size = swap_size(db_, from, to, 0);  // the size recorded with the readings; else the live one, if it is the same
        if (size <= 0 && !query(db_, "SELECT 1 FROM samples WHERE t >= ? AND t < ? AND swap_used > 0 LIMIT 1", {from, to}).empty()) {
            if (!sampler_) sampler_ = std::make_unique<Sampler>();
            const SnapshotPtr s = sampler_->sample();
            size = swap_size(db_, from, to, s ? double(s->mem.swap_total) : 0);
        }
        if (size > 0) {
            auto cap = query(db_, "SELECT COUNT(*), MIN(t), MAX(t) FROM samples WHERE t >= ? AND t < ? AND swap_used >= ?", {from, to, int64_t(0.999 * size)});
            if (!cap.empty() && cap[0].t > 0)
                full = "The swap was full at " + human_bytes(size) + " for about " + duration_text(cap[0].t) + " in all" +
                       times_text(int64_t(cap[0].a), int64_t(cap[0].b)) + ".";
        }
    }
    if (parts.empty() && logged.empty() && restarted.empty() && full.empty())
        return "A quiet window: no resource events, crashes or changes were recorded.";
    std::string text = "In that window: " + (parts.empty() ? "the log has " + logged + "."
                                                           : unllm::join_list(parts) + "." + (logged.empty() ? "" : " The log has " + logged + "."));
    if (parts.empty() && logged.empty()) text = "In that window:";
    if (!restarted.empty()) text += " Programs that kept starting again: " + unllm::join_list(restarted) + ".";
    if (!full.empty()) text += " " + full;
    Diagnose diag(db_);
    for (const auto& c : diag.run(from, to, now, nullptr))
        if (c.salience >= 2.5) {
            text += " Top finding: " + c.sentence;
            if (receipts) receipts->insert(receipts->end(), c.receipts.begin(), c.receipts.end());
            break;
        }
    return text;
}

namespace {

// One mounted filesystem's size, from statvfs. Pseudo filesystems and second
// mounts of the same device (btrfs subvolumes) are left out.
std::vector<Space> real_filesystems() {
    std::vector<Space> out;
    std::set<std::string> seen;
    std::ifstream mounts("/proc/self/mounts");
    std::string dev, mnt, type, rest;
    while (mounts >> dev >> mnt >> type && std::getline(mounts, rest)) {
        if (dev.rfind("/dev/", 0) != 0 || type == "squashfs" || type == "iso9660") continue;
        if (mnt.rfind("/snap", 0) == 0 || mnt.rfind("/var/lib/docker", 0) == 0) continue;
        struct statvfs v {};
        if (statvfs(mnt.c_str(), &v) != 0 || v.f_blocks == 0) continue;
        const std::string key = dev;
        if (!seen.insert(key).second) continue;  // the first (shortest) mount of a device stands for it
        out.push_back({mnt, double(v.f_blocks) * double(v.f_frsize), double(v.f_bavail) * double(v.f_frsize)});
    }
    return out;
}

std::string space_line(const Space& s) {
    const int used = int(std::lround(100.0 * (1.0 - s.free / s.total)));
    return s.mount + " has " + human_bytes(s.free) + " free of " + human_bytes(s.total) + " (" + std::to_string(used) + "% used)";
}

// How the free space on / moved over the readings rows (t, percent free; the hourly medians before the per-second
// readings begin): where it began and ended, and its biggest fall and rise, each a run of hour-to-hour steps the same
// way of a gigabyte or more ("Wed 18:00 to Wed 21:00, about 10.8 GB"), with a package change made inside the fall.
std::string space_change(sqlite3* db, const std::vector<Row>& rows, double total) {
    if (rows.size() < 2) return "";
    const double a = rows.front().a, b = rows.back().a;
    const double lost = (a - b) / 100.0 * total;
    std::string text = "Free space on / went from " + fmt("%.0f", a) + "% (about " + human_bytes(a / 100.0 * total) + ") to " + fmt("%.0f", b) +
                       "% between " + span_text(rows.front().t, rows.back().t, " and ") +
                       (std::abs(lost) < 0.5e9 ? ", about the same." : lost > 0 ? ", about " + human_bytes(lost) + " used up." : ", about " + human_bytes(-lost) + " freed.");
    // The last reading of each hour, then the runs.
    std::vector<Row> hours;
    for (const auto& x : rows)
        if (!hours.empty() && hours.back().t / 3600 == x.t / 3600) hours.back().a = x.a;
        else hours.push_back({x.t - x.t % 3600, x.a});
    struct Run { int64_t from = 0, to = 0; double bytes = 0; };
    Run fall, rise, cur;
    for (std::size_t i = 1; i < hours.size(); ++i) {
        const double step = (hours[i].a - hours[i - 1].a) / 100.0 * total;  // + freed, - used
        if (std::abs(step) < 1e9) { cur = {}; continue; }
        if (cur.from && (cur.bytes > 0) == (step > 0)) cur.to = hours[i].t, cur.bytes += step;
        else cur = {hours[i - 1].t, hours[i].t, step};
        if (cur.bytes < fall.bytes) fall = cur;
        if (cur.bytes > rise.bytes) rise = cur;
    }
    std::vector<std::string> said;
    if (fall.from) {
        std::string s = "the biggest fall was " + span_text(fall.from, fall.to, " to ") + ", about " + human_bytes(-fall.bytes);
        auto pk = query(db, "SELECT CAST(t AS INTEGER) FROM log WHERE category = 'software' AND t >= ? AND t < ? ORDER BY t", {fall.from, fall.to + 3600});
        if (!pk.empty()) s += ", with the package change at " + when_text(pk[0].t) + " in it";
        said.push_back(s);
    }
    if (rise.from) said.push_back(span_text(rise.from, rise.to, " to ") + " freed about " + human_bytes(rise.bytes));
    if (!said.empty()) {
        text.pop_back();
        text += ": " + said[0] + (said.size() > 1 ? "; " + said[1] : "") + ".";
    }
    return text;
}

}  // namespace

namespace {
bool answers_from_records(const std::string& kind) {
    static const std::set<std::string> k = {"disk.space.value.now", "disk.space.trend.window", "disk.space.which.now",
                                            "memory.swap.value.now", "memory.swap.value.window", "heat.fan.value.window",
                                            "battery.value.window", "startup.when.reboot", "startup.often.reboot",
                                            "startup.value.boottime", "network.value.window", "sharing.value.window",
                                            "network.which.window", "sharing.which.window", "network.which.now",
                                            "sharing.which.now", "memory.oom.list.window", "diagnose.logs.list.window",
                                            "crashes.yesno.window.system", "startup.uptime.value.now",
                                            "identity.cpu", "identity.gpu", "identity.ram", "identity.disk", "identity.kernel",
                                            "identity.os", "battery.identity.health", "battery.value.now.remaining",
                                            "startup.list.now", "network.wifi.value.now", "network.wifi.when.window",
                                            "config.list.window", "config.when.window", "diagnose.list.window",
                                            "network.address.value.now", "devices.bluetooth.now", "devices.mic.now",
                                            "gpu.throttle.now", "slow.load.value.now", "devices.list.now",
                                            "gpu.value.window", "diagnose.compare.window", "battery.value.now",
                                            "network.inbound.window", "disk.health.now", "identity.bios.flash",
                                            "story.often.window.process", "network.new.list.window", "slow.procs.value.window",
                                            "slow.above.value.window", "identity.list", "programs.list.now", "programs.installed.now", "programs.owner.list.now",
                                            "startup.cause.reboot", "programs.open.cause.now", "gpu.model.now", "disk.folder.list.now",
                                            "disk.find.which.now", "programs.pid.identity.now", "sound.value.now"};
    return k.count(kind) > 0 || from_logs(kind);
}

// Crashes and failed units, a program's restarts, startup changes, sleep and wake, the charger, the lid, logging in,
// commands run as administrator, first runs of programs, package changes and what they broke, whether the machine
// was used, devices plugged and unplugged, and firmware flashed: the log and the boot records.
bool from_logs(const std::string& kind) {
    static const std::set<std::string> k = {"crashes.list.window", "crashes.when.window", "crashes.which.window", "startup.list.window",
                                            "startup.which.window", "startup.often.window", "battery.sleep.list.window",
                                            "battery.sleep.when.window", "battery.sleep.often.window", "updates.when.window",
                                            "updates.identity.package", "battery.charger.when.window", "battery.charger.list.window",
                                            "battery.lid.when.window", "battery.lid.list.window", "startup.login.when.window",
                                            "startup.login.list.window", "trust.admin.list.window", "trust.list.window",
                                            "updates.cause.window", "story.used.window", "devices.when.window", "flash.list.window",
                                            "crashes.cause.window"};
    return k.count(kind) > 0;
}
}  // namespace

namespace {

// One measure over a window: the per-second samples where they reach, the
// hourly medians before that. Each row: t, value.
std::vector<Row> series(sqlite3* db, const std::string& column, const std::string& hourly, int64_t from, int64_t to) {
    auto first = query(db, "SELECT CAST(MIN(t) AS INTEGER) FROM samples WHERE " + column + " IS NOT NULL", {});
    const int64_t detail = first.empty() || first[0].t == 0 ? to : first[0].t;
    std::vector<Row> out;
    if (!hourly.empty() && from < detail) {
        // query() binds the integers first, then the text: the metric goes last
        out = query(db, "SELECT hour_ts, med FROM samples_hourly WHERE hour_ts >= ? AND hour_ts < ? AND metric = ? ORDER BY hour_ts",
                    {from, std::min(to, detail)}, {hourly});
    }
    auto rows = query(db, "SELECT CAST(t AS INTEGER), " + column + " FROM samples WHERE " + column +
                          " IS NOT NULL AND t >= ? AND t < ? ORDER BY t", {std::max(from, detail), to});
    out.insert(out.end(), rows.begin(), rows.end());
    return out;
}

// A command's output, for the journal and systemd-analyze.
std::string run_out(const std::string& cmd) {
    std::string out;
    if (FILE* f = popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (size_t n = fread(buf, 1, sizeof buf, f)) out.append(buf, n);
        pclose(f);
    }
    return out;
}

// The journal's boots, oldest first: start and end, unix seconds, and the boot id.
struct Boot {
    int64_t first = 0, second = 0;
    std::string id;
};

// One field of a journal entry printed by journalctl -o json: a string, or a number as text.
std::string jfield(const std::string& obj, const std::string& key) {
    const std::string k = "\"" + key + "\":";
    std::size_t at = obj.find(k);
    if (at == std::string::npos) return "";
    at += k.size();
    if (at >= obj.size()) return "";
    if (obj[at] != '"') {
        const std::size_t e = obj.find_first_of(",}", at);
        return obj.substr(at, e == std::string::npos ? std::string::npos : e - at);
    }
    std::string out;
    for (++at; at < obj.size() && obj[at] != '"'; ++at) {
        if (obj[at] == '\\' && at + 1 < obj.size()) {
            ++at;
            out += obj[at] == 'n' ? ' ' : obj[at] == 't' ? ' ' : obj[at];
        } else {
            out += obj[at];
        }
    }
    return out;
}

// journalctl's entries as JSON objects, one per line.
std::vector<std::string> journal(const std::string& args) {
    std::vector<std::string> out;
    std::istringstream in(run_out("journalctl --no-pager -q -o json " + args + " 2>/dev/null"));
    for (std::string line; std::getline(in, line);)
        if (!line.empty() && line[0] == '{') out.push_back(line);
    return out;
}

std::vector<Boot> boots() {
    std::vector<Boot> out;
    std::istringstream in(run_out("journalctl --list-boots --no-pager -o json 2>/dev/null"));
    const std::string text = in.str();
    // one object per boot: {"index":..,"boot_id":"..","first_entry":usec,"last_entry":usec}
    for (std::size_t at = 0; (at = text.find("\"boot_id\"", at)) != std::string::npos;) {
        const std::size_t end = text.find('}', at);
        if (end == std::string::npos) break;
        const std::string obj = "{" + text.substr(at, end - at) + "}";
        Boot b;
        b.id = jfield(obj, "boot_id");
        b.first = std::atoll(jfield(obj, "first_entry").c_str()) / 1000000;
        b.second = std::atoll(jfield(obj, "last_entry").c_str()) / 1000000;
        if (b.first > 0) out.push_back(b);
        at = end;
    }
    std::sort(out.begin(), out.end(), [](const Boot& a, const Boot& b) { return a.first < b.first; });
    return out;
}

// The out-of-memory kills in a window, newest first, at most `keep` of them named: systemd's note on
// each unit that systemd-oomd or the kernel emptied, named by the biggest program recorded in that
// unit in the five minutes before. `total` gets the full count.
std::vector<std::string> oom_kills(sqlite3* db, int64_t from, int64_t to, std::size_t keep, std::size_t* total) {
    const std::string range = " --since=@" + std::to_string(from) + " --until=@" + std::to_string(to);
    auto kills = journal(range + " MESSAGE_ID=d989611b15e44c9dbf31e3c81256e4ed + MESSAGE_ID=fe6faa94e7774663a0da52717891d8ef");
    if (total) *total = kills.size();
    std::vector<std::string> named;
    for (auto it = kills.rbegin(); it != kills.rend() && named.size() < keep; ++it) {
        const int64_t t = std::atoll(jfield(*it, "__REALTIME_TIMESTAMP").c_str()) / 1000000;
        std::string unit = jfield(*it, "UNIT");
        if (unit.empty()) unit = jfield(*it, "USER_UNIT");
        const bool oomd = jfield(*it, "MESSAGE_ID") == "d989611b15e44c9dbf31e3c81256e4ed";
        auto top = query(db, "SELECT 0, peak_rss, 0, name, cmdline FROM minute_top WHERE minute BETWEEN ? AND ? AND unit = ? "
                             "ORDER BY peak_rss DESC LIMIT 1", {t - 300, t}, {unit});
        std::string who = unit.substr(0, unit.rfind('.'));
        if (!top.empty()) {
            std::string name = top[0].s1;
            if (name == "exe" && !top[0].s2.empty()) {  // a program run through /proc/self/exe: its command line names it
                name = top[0].s2.substr(0, top[0].s2.find(' '));
                name = name.substr(name.rfind('/') + 1);
            }
            who = name + " (" + human_bytes(top[0].a) + ", in " + unit + ")";
        }
        named.push_back(who + " at " + when_text(t) + (oomd ? "" : " by the kernel"));
    }
    return named;
}

// The Wi-Fi drops in a window, oldest first: wpa_supplicant's disconnect and the reconnect after it
// (end 0 while still down), leaving out the disconnects within a minute of the machine going to sleep.
struct Drop { int64_t from = 0, to = 0; };
std::vector<Drop> wifi_drops(int64_t from, int64_t to) {
    const std::string range = " --since=@" + std::to_string(from) + " --until=@" + std::to_string(to);
    std::vector<int64_t> sleeps;
    for (const auto& e : journal(range + " MESSAGE_ID=6bbd95ee977941e497c48be27c254128"))
        sleeps.push_back(std::atoll(jfield(e, "__REALTIME_TIMESTAMP").c_str()) / 1000000);
    std::vector<Drop> out;
    bool down = false;
    for (const auto& e : journal(range + " SYSLOG_IDENTIFIER=wpa_supplicant")) {
        const std::string m = jfield(e, "MESSAGE");
        const int64_t t = std::atoll(jfield(e, "__REALTIME_TIMESTAMP").c_str()) / 1000000;
        if (m.find("CTRL-EVENT-DISCONNECTED") != std::string::npos && !down) {
            bool asleep = false;
            for (int64_t z : sleeps) asleep = asleep || std::llabs(z - t) <= 60;
            if (asleep) continue;
            out.push_back({t, 0});
            down = true;
        } else if (m.find("CTRL-EVENT-CONNECTED") != std::string::npos && down) {
            out.back().to = t;
            down = false;
        }
    }
    return out;
}

// The wireless interface, if there is one.
// iw's link report for a Wi-Fi device: the network, the signal and its grade, the negotiated rates (Mbit/s, as text)
// and the band's frequency.
struct WifiLink {
    std::string ssid, signal, rx, tx, freq;
    double dbm = 0;
    const char* grade = "";  // strong, good, fair or weak
};
WifiLink wifi_link(const std::string& dev) {
    WifiLink w;
    std::istringstream in(run_out("iw dev " + dev + " link 2>/dev/null"));
    for (std::string line; std::getline(in, line);) {
        const std::string l = trim(line);
        auto after = [&](const char* key) { return l.rfind(key, 0) == 0 ? trim(l.substr(std::strlen(key))) : std::string(); };
        if (auto v = after("SSID:"); !v.empty()) w.ssid = v;
        if (auto v = after("signal:"); !v.empty()) w.signal = v;
        if (auto v = after("rx bitrate:"); !v.empty()) w.rx = v.substr(0, v.find(" MBit/s"));
        if (auto v = after("tx bitrate:"); !v.empty()) w.tx = v.substr(0, v.find(" MBit/s"));
        if (auto v = after("freq:"); !v.empty()) w.freq = v;
    }
    w.dbm = std::atof(w.signal.c_str());
    w.grade = w.dbm >= -55 ? "strong" : w.dbm >= -67 ? "good" : w.dbm >= -75 ? "fair" : "weak";
    return w;
}

std::string wifi_iface() {
    for (const auto& n : list_dir("/sys/class/net"))
        if (exists("/sys/class/net/" + n + "/wireless")) return n;
    return "";
}

std::string duration_text(double secs) {
    if (secs < 90) return fmt("%.0f seconds", secs);
    if (secs < 5400) return fmt("%.0f minutes", secs / 60);
    if (secs < 172800) return fmt("%.1f hours", secs / 3600);
    return fmt("%.1f days", secs / 86400);
}

// "is anything wrong right now": the live figures, led by what is acute by the recorder's own lines (Triggers: the
// shutdown margin, the temperature, programs waiting on memory) and the system's low battery level.
std::string health_now(const Snapshot& s, double low_battery) {
    std::vector<std::string> acute, figures;
    const Triggers line;
    const double temp = s.cpu.temp_c.or_else(s.guard.temp.or_else(-1));
    if ((s.guard.margin.ok() && s.guard.margin.get() < line.shutdown_margin) || temp >= line.temp) acute.push_back("the CPU is hot");
    if (s.mem.pressure_some10.or_else(0) >= line.pressure) acute.push_back("programs are waiting on memory");
    const bool on_battery = s.power.on_ac.ok() && !s.power.on_ac.get();
    if (on_battery && s.power.battery_pct.ok() && s.power.battery_pct.get() < low_battery) acute.push_back("the battery is low");
    figures.push_back("the CPU " + (temp >= 0 ? fmt("at %.0f C and ", temp) : std::string()) + fmt("%.0f%% busy", s.cpu.total));
    if (s.guard.margin.ok()) figures.push_back(fmt("the shutdown sensor %.0f C below its limit", s.guard.margin.get()));
    if (s.mem.total > 0) figures.push_back(fmt("memory %.0f%% used", s.mem.percent()));
    if (s.mem.swap_total > 0)
        figures.push_back(fmt("swap %.0f%% full", 100.0 * double(s.mem.swap_total - std::min(s.mem.swap_free, s.mem.swap_total)) / double(s.mem.swap_total)));
    if (s.root_free_pct.ok()) figures.push_back(fmt("%.0f%% free on /", s.root_free_pct.get()));
    if (s.power.battery_pct.ok())
        figures.push_back(fmt("the battery at %.0f%%", s.power.battery_pct.get()) + (on_battery ? ", on battery" : s.power.on_ac.ok() ? ", on the charger" : ""));
    return "Right now " + (acute.empty() ? std::string("nothing is acute") : unllm::join_list(acute)) + ": " + unllm::join_list(figures) + ".";
}

// How much of the swap is zram (/proc/swaps): compressed space in memory, not on disk.
std::string zram_text(const MemorySample& m) {
    if (m.zram_swap == 0 || m.swap_total == 0) return "";
    if (m.zram_swap >= m.swap_total) return " All of it is zram: compressed space in memory, not on disk.";
    return " " + human_bytes(double(m.zram_swap)) + " of it is zram, compressed in memory; the rest is on disk.";
}

// A command line as a reader knows it: the program's file name and its first arguments, paths by their last part
// ("/home/user/.cache/venv/bin/python -I rerank_4b50.py" -> "python -I rerank_4b50.py").
std::string command_text(const std::string& cmdline, std::size_t args = 2) {
    std::istringstream in(cmdline);
    std::string out;
    std::size_t n = 0;
    for (std::string w; in >> w && n <= args; ++n) {
        if (const auto slash = w.rfind('/'); slash != std::string::npos && slash + 1 < w.size()) w = w.substr(slash + 1);
        out += (out.empty() ? "" : " ") + w;
    }
    return out;
}

// What a program had open: the last part of the last path its command line names
// ("/usr/bin/haruna /home/user/Videos/clip.mp4" -> "clip.mp4"); "" for none.
std::string opened_text(const std::string& cmdline) {
    const auto sp = cmdline.find(' ');
    if (sp == std::string::npos) return "";
    const std::string args = cmdline.substr(sp + 1);
    const auto slash = args.rfind('/');
    return slash == std::string::npos || slash + 1 >= args.size() ? "" : args.substr(slash + 1);
}

// The machine's sleeps in the log, as spans: the logbook's "Went to sleep" row to the next "Woke from sleep"
// (logbook.cpp, classify_journal).
std::vector<std::pair<int64_t, int64_t>> sleeps(sqlite3* db, int64_t from, int64_t to) {
    std::vector<std::pair<int64_t, int64_t>> out;
    int64_t went = 0;
    for (const auto& x : query(db, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'session' AND t >= ? AND t < ? "
                                   "AND title IN ('Went to sleep', 'Woke from sleep') ORDER BY t", {from, to})) {
        if (x.s1 == "Went to sleep") went = x.t;
        else if (went) out.emplace_back(went, x.t), went = 0;
    }
    return out;
}

// The moments the charger went in or out: the readings' on_ac changes and the log's power rows (LogWatch::changes
// writes "Plugged into power at 75% battery" and "Unplugged from power at 98% battery"); one moment once, the log's
// time first. level -1: not known.
struct Plug {
    int64_t t = 0;
    bool in = false;
    double level = -1;
};
std::vector<Plug> plugs(sqlite3* db, int64_t from, int64_t to) {
    std::vector<Plug> out;
    for (const auto& x : query(db, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'session' AND t >= ? AND t < ? "
                                   "AND (title LIKE 'Plugged into power%' OR title LIKE 'Unplugged from power%') ORDER BY t", {from, to})) {
        const auto at = x.s1.find(" at ");
        out.push_back({x.t, x.s1.rfind("Plugged", 0) == 0, at == std::string::npos ? -1 : std::atof(x.s1.c_str() + at + 4)});
    }
    auto ac = query(db, "SELECT CAST(t AS INTEGER), on_ac, IFNULL(battery_pct, -1) FROM samples WHERE on_ac IS NOT NULL AND t >= ? AND t < ? ORDER BY t", {from, to});
    for (std::size_t i = 1; i < ac.size(); ++i) {
        if (ac[i].a == ac[i - 1].a) continue;
        const bool in = ac[i].a == 1;
        bool logged = false;
        for (const auto& p : out) logged = logged || (p.in == in && std::llabs(p.t - ac[i].t) <= 15);
        if (!logged) out.push_back({ac[i].t, in, ac[i].b});
    }
    std::sort(out.begin(), out.end(), [](const Plug& a, const Plug& b) { return a.t < b.t; });
    return out;
}

// "unplugged at Thu 08:37 (98%) and plugged back in at Thu 09:07 (75%)"; past six, the rest counted.
std::string plugs_text(const std::vector<Plug>& ps) {
    std::vector<std::string> said;
    for (std::size_t i = 0; i < ps.size() && said.size() < 6; ++i)
        said.push_back(std::string(!ps[i].in ? "unplugged" : i > 0 && !ps[i - 1].in ? "plugged back in" : "plugged in") + " at " + when_text(ps[i].t) +
                       (ps[i].level >= 0 ? " (" + fmt("%.0f", ps[i].level) + "%)" : ""));
    if (ps.size() > said.size()) said.push_back(unllm::number_word(int64_t(ps.size() - said.size())) + " more times in or out");
    return unllm::join_list(said);
}

// The battery level the system itself calls low: UPower's PercentageLow, from its configuration, else UPower's own
// default (20%).
double low_battery_level(const std::string& etc) {
    double level = 20;
    if (auto text = read_file(etc + "/UPower/UPower.conf")) {
        std::istringstream in(*text);
        for (std::string line; std::getline(in, line);)
            if (line.rfind("PercentageLow=", 0) == 0) level = std::atof(line.c_str() + 14);
    }
    return level;
}

// The battery's low points over [from, to), oldest first: the lowest per-second reading where they reach, before
// them each hourly record's lowest, and every plug-in the log or the readings hold (the level when the charger goes
// back in is the lowest of the spell on battery before it). An hour with a plug-in in it is told by the plug-in.
struct Low {
    int64_t t = 0;
    double level = 0;
    bool plug = false;
};
std::vector<Low> battery_lows(sqlite3* db, int64_t from, int64_t to, int64_t* readings_from) {
    std::vector<Low> out;
    auto first = query(db, "SELECT CAST(MIN(t) AS INTEGER) FROM samples WHERE battery_pct >= 0 AND t >= ? AND t < ?", {from, to});
    *readings_from = first.empty() ? 0 : first[0].t;
    if (*readings_from) {
        auto lo = query(db, "SELECT CAST(t AS INTEGER), battery_pct FROM samples WHERE battery_pct >= 0 AND t >= ? AND t < ? ORDER BY battery_pct, t LIMIT 1",
                        {from, to});
        if (!lo.empty()) out.push_back({lo[0].t, lo[0].a, false});
    }
    const auto ps = plugs(db, from, to);
    for (const auto& p : ps)
        if (p.in && p.level >= 0) out.push_back({p.t, p.level, true});
    for (const auto& h : query(db, "SELECT hour_ts, min FROM samples_hourly WHERE metric = 'battery_pct' AND min >= 0 AND hour_ts >= ? AND hour_ts < ?",
                               {from, *readings_from ? *readings_from : to})) {
        bool told = false;
        for (const auto& p : ps) told = told || (p.in && p.t >= h.t && p.t < h.t + 3600);
        if (!told) out.push_back({h.t, h.a, false});
    }
    std::sort(out.begin(), out.end(), [](const Low& a, const Low& b) { return a.t < b.t; });
    return out;
}

// The highest the battery got in [from, to) and the first moment it was there: the per-second readings where they
// reach, before them each hourly record's highest (its hour), and the level the log gives at each unplug (the charge it
// set out with; plug). t 0: nothing recorded.
Low battery_high(sqlite3* db, int64_t from, int64_t to, int64_t* readings_from) {
    Low top{0, -1, false};
    auto take = [&](int64_t t, double level, bool plug) {
        if (level > top.level || (level == top.level && t < top.t)) top = {t, level, plug};
    };
    auto first = query(db, "SELECT CAST(MIN(t) AS INTEGER) FROM samples WHERE battery_pct >= 0 AND t >= ? AND t < ?", {from, to});
    *readings_from = first.empty() ? 0 : first[0].t;
    if (*readings_from) {
        auto hi = query(db, "SELECT CAST(t AS INTEGER), battery_pct FROM samples WHERE battery_pct >= 0 AND t >= ? AND t < ? ORDER BY battery_pct DESC, t LIMIT 1",
                        {from, to});
        if (!hi.empty()) take(hi[0].t, hi[0].a, false);
    }
    for (const auto& h : query(db, "SELECT hour_ts, max FROM samples_hourly WHERE metric = 'battery_pct' AND max >= 0 AND hour_ts >= ? AND hour_ts < ?",
                               {from, *readings_from ? *readings_from : to}))
        take(h.t, h.a, false);
    for (const auto& p : plugs(db, from, to))
        if (!p.in && p.level >= 0) take(p.t, p.level, true);
    return top;
}

// One out-of-memory kill as the log holds it: the unit systemd says failed with result 'oom-kill', and the
// kernel's own line when its killer did it ("Out of memory: Killed process 443368 (blender) total-vm:12034096kB,
// anon-rss:4894212kB, file-rss:8928kB, shmem-rss:0kB, ...").
struct Kill {
    int64_t t = 0;
    std::string unit;     // "" when only the kernel's line is there
    std::string process;  // what the kernel killed; "" when systemd-oomd emptied the unit
    int64_t pid = 0;
    double rss = 0, vm = 0;  // the kernel's figures for that process, bytes
};
std::vector<Kill> recorded_kills(sqlite3* db, int64_t from, int64_t to) {
    std::vector<Kill> out;
    for (const auto& x : query(db, "SELECT CAST(t AS INTEGER), 0, 0, title, detail FROM log WHERE category = 'services' AND t >= ? AND t < ? "
                                   "AND title LIKE '%''oom-kill''%' ORDER BY t", {from, to}))
        out.push_back({x.t, failed_unit(x.s1, x.s2), "", 0, 0, 0});
    auto kb = [](const std::string& line, const char* field) {
        const auto at = line.find(field);
        return at == std::string::npos ? 0.0 : std::atof(line.c_str() + at + std::strlen(field)) * 1024;
    };
    for (const auto& x : query(db, "SELECT CAST(t AS INTEGER), 0, 0, title, detail FROM log WHERE category = 'problems' AND t >= ? AND t < ? "
                                   "AND detail LIKE '%Out of memory: Killed process%' ORDER BY t", {from, to})) {
        const auto at = x.s2.find("Killed process ");
        const auto open = x.s2.find('(', at), close = x.s2.find(')', at);
        Kill k{x.t, "", open != std::string::npos && close > open ? x.s2.substr(open + 1, close - open - 1) : "", std::atoll(x.s2.c_str() + at + 15),
               kb(x.s2, "anon-rss:") + kb(x.s2, "file-rss:") + kb(x.s2, "shmem-rss:"), kb(x.s2, "total-vm:")};
        // The unit that failed in the same seconds is this same kill.
        auto same = std::find_if(out.begin(), out.end(), [&](const Kill& o) { return o.process.empty() && std::llabs(o.t - k.t) <= 5; });
        if (same != out.end()) k.unit = same->unit, *same = k;
        else out.push_back(k);
    }
    std::sort(out.begin(), out.end(), [](const Kill& a, const Kill& b) { return a.t < b.t; });
    return out;
}

// What the machine was doing right before a moment: the last reading of its last two minutes (the per-second
// readings while kept, else the ones the event there kept), the busiest programs of that event with their command
// lines, and the hour's highs from the hourly records. "" when the records hold none of it.
std::string right_before(sqlite3* db, int64_t t) {
    // The event at that moment: the latest one starting or ending between two minutes before it and a minute after.
    auto ev = query(db, "SELECT id, start, 0, summary FROM events WHERE start BETWEEN ? AND ? OR \"end\" BETWEEN ? AND ? ORDER BY start DESC LIMIT 1",
                    {t - 120, t + 60, t - 120, t + 60});
    const int64_t end = ev.empty() ? t : std::max(t, int64_t(ev[0].a));
    std::vector<std::string> said;
    auto last = query(db, "SELECT CAST(t AS INTEGER), temp, cpu, fan, -1 FROM samples WHERE t BETWEEN ? AND ? ORDER BY t DESC LIMIT 1", {end - 120, end + 2});
    if (last.empty())
        last = query(db, "SELECT CAST(t AS INTEGER), temp, cpu, fan, guard FROM event_samples WHERE t BETWEEN ? AND ? ORDER BY t DESC LIMIT 1", {end - 120, end + 2});
    if (!last.empty()) {
        const double fan = last[0].s1.empty() ? -1 : std::atof(last[0].s1.c_str()), guard = last[0].s2.empty() ? -1 : std::atof(last[0].s2.c_str());
        std::string reading = "the last reading (" + when_text(last[0].t) + ") was ";
        if (last[0].a >= 0) reading += fmt("%.0f C", last[0].a) + (guard >= 0 ? fmt(" (%.0f C on the shutdown sensor)", guard) : "") + ", with ";
        reading += "the CPU " + fmt("%.0f", last[0].b) + "% busy" + (fan >= 0 ? " and the fan at " + fmt("%.0f", fan) + "%" : "");
        said.push_back(reading);
    }
    std::vector<std::string> busy;
    if (!ev.empty())
        for (const auto& c : query(db, "SELECT rank, share, 0, name, cmdline FROM event_culprits WHERE event_id = ? AND kind != 'unseen' ORDER BY rank LIMIT 3",
                                   {ev[0].t})) {
            const std::string cmd = command_text(c.s2);
            busy.push_back(c.s1 + " (" + (cmd.empty() || cmd == c.s1 ? "" : cmd + ", ") + fmt("%.0f", 100 * c.a) + "% of the CPU time)");
        }
    else
        for (const auto& c : query(db, "SELECT 0, SUM(cpu_seconds), 0, name, MAX(cmdline) FROM minute_top WHERE minute BETWEEN ? AND ? GROUP BY name "
                                       "ORDER BY 2 DESC LIMIT 3", {end - 120, end})) {
            const std::string cmd = command_text(c.s2);
            busy.push_back(c.s1 + (cmd.empty() || cmd == c.s1 ? "" : " (" + cmd + ")"));
        }
    if (!busy.empty()) said.push_back("the busiest programs were " + unllm::join_list(busy));
    // The hour's highs: the metric names are the hourly table's own.
    std::vector<std::string> highs;
    const int64_t hour = (end - 1) / 3600 * 3600;
    for (const auto& h : query(db, "SELECT 0, max, 0, metric FROM samples_hourly WHERE hour_ts = ? AND metric IN ('temp', 'fan', 'gpu', 'cpu') "
                                   "ORDER BY CASE metric WHEN 'temp' THEN 0 WHEN 'fan' THEN 1 WHEN 'gpu' THEN 2 ELSE 3 END", {hour}))
        highs.push_back(h.s1 == "temp" ? "the temperature " + fmt("%.0f C", h.a) : "the " + std::string(h.s1 == "cpu" ? "CPU" : h.s1 == "gpu" ? "GPU" : "fan") + " " + fmt("%.0f%%", h.a));
    if (!highs.empty()) said.push_back("in the hour from " + when_text(hour) + " " + unllm::join_list(highs) + " at most");
    if (said.empty()) return "";
    std::string text = "Right before " + when_text(end) + " " + said[0];
    for (std::size_t i = 1; i < said.size(); ++i) text += "; " + said[i];
    return text + ".";
}

// The minutes before a stop without a clean shutdown at t (its event's time: the log's last entry), from the records:
// when the recorder's own readings ended (they can run on past the log; the process rows it closed at its next start
// carry its last reading), whether it ran on mains or battery, the highs of its last two minutes and of the hour past
// them, the busiest programs with their command lines, and what the records say of the cause. "" when they hold
// nothing of it.
// "firefox isn't running right now": what it did last, from the records. Its latest run is the processes of it that
// ended within ten minutes of the last one: the span, the CPU time and peak memory they add up to, and how warm and busy
// the machine got meanwhile. A program with no end on record gets its last start.
std::string last_run_text(sqlite3* db, const std::string& program, int64_t now) {
    auto last = query(db, "SELECT CAST(MAX(ended) AS INTEGER) FROM procs WHERE ended IS NOT NULL AND is_program(?1, name, exe, cmdline)", {}, {program});
    if (last.empty() || last[0].t <= 0) {
        auto any = query(db, "SELECT CAST(MAX(started) AS INTEGER) FROM procs WHERE is_program(?1, name, exe, cmdline)", {}, {program});
        if (any.empty() || any[0].t <= 0) return " It hasn't run on this machine as far as the records go.";
        return " It last started on " + moment_text(any[0].t) + ", and the records don't show it ending.";
    }
    const int64_t end = std::min<int64_t>(last[0].t, now);
    auto run = query(db, "SELECT CAST(MIN(started) AS INTEGER), SUM(cpu_seconds), MAX(peak_rss) FROM procs WHERE ended > ?1 AND ended <= ?2 "
                         "AND is_program(?3, name, exe, cmdline)", {end - 600, end}, {program});
    const int64_t start = run.empty() || run[0].t <= 0 || run[0].t > end ? end : run[0].t;
    std::string s = " It last ran " + (local_date(start) == local_date(end) ? when_text(start) + " to " + clock_text(end) : span_text(start, end, " to ")) +
                    (end - start >= 60 ? " (" + duration_text(double(end - start)) + ")" : "");
    if (!run.empty() && (run[0].a > 0 || run[0].b > 0))
        s += ", using " + (run[0].a >= 60 ? duration_text(run[0].a) : fmt("%.0f s", run[0].a)) + " of CPU time" +
             (run[0].b > 0 ? " and up to " + human_bytes(run[0].b) + " of memory" : "");
    auto machine = query(db, "SELECT 0, MAX(temp), MAX(cpu) FROM samples WHERE t >= ?1 AND t <= ?2", {start, end});
    if (!machine.empty() && machine[0].a > 0)
        s += fmt("; the machine reached %.0f C", machine[0].a) + (machine[0].b > 0 ? fmt(" and %.0f%% CPU", machine[0].b) : "") + " meanwhile";
    return s + ".";
}

std::string stop_story(sqlite3* db, int64_t t) {
    std::vector<std::string> said;
    int64_t end = t;
    // The per-second readings while kept: the run of them on from t without a gap (the next boot's come after one).
    for (const auto& x : query(db, "SELECT CAST(t AS INTEGER) FROM samples WHERE t >= ? AND t < ? ORDER BY t", {t, t + 600})) {
        if (x.t - end > 30) break;
        end = x.t;
    }
    auto cut = query(db, "SELECT CAST(MAX(ended) AS INTEGER) FROM procs WHERE end_estimated = 1 AND ended > ? AND ended < ?", {t, t + 600});
    if (!cut.empty() && cut[0].t > end) end = cut[0].t;
    if (end > t)
        said.push_back("The recorder read on to " + clock_text(end) + ", " + std::to_string(end - t) + " s past the log's last entry, so it stopped after that.");
    // On mains or battery: the last reading's charger flag, else the log's last plug row, else its hour's time on battery.
    std::string power;
    bool mains = false;
    auto ac = query(db, "SELECT CAST(t AS INTEGER), on_ac, IFNULL(battery_pct, -1) FROM samples WHERE on_ac >= 0 AND t >= ? AND t <= ? ORDER BY t DESC LIMIT 1",
                    {end - 120, end});
    const auto ps = plugs(db, end - 7 * 86400, end);
    if (!ac.empty()) {
        mains = ac[0].a == 1;
        power = mains ? "on mains power" : "on battery" + (ac[0].b >= 0 ? " at " + fmt("%.0f", ac[0].b) + "%" : std::string());
    } else if (!ps.empty()) {
        mains = ps.back().in;
        power = std::string(mains ? "on mains power (plugged in since " : "on battery (unplugged since ") + when_text(ps.back().t) +
                (ps.back().level >= 0 ? ", at " + fmt("%.0f", ps.back().level) + "%" : "") + ")";
    } else if (auto h = query(db, "SELECT 0, on_battery_frac FROM samples_hourly WHERE hour_ts = ? AND metric = 'cpu' AND on_battery_frac IS NOT NULL",
                              {(end - 1) / 3600 * 3600});
               !h.empty() && (h[0].a == 0 || h[0].a == 1)) {
        mains = h[0].a == 0;
        power = mains ? "on mains power all that hour" : "on battery all that hour";
    }
    if (!power.empty()) said.push_back("It was " + power + (mains ? ", so the battery did not run out." : "."));
    // Its last two minutes: the per-second readings while kept, else the ones its event kept.
    auto ev = query(db, "SELECT id, IFNULL(guard_limit, -1), IFNULL(min_margin, -1) FROM events WHERE (kinds & ?) != 0 AND start BETWEEN ? AND ? "
                        "ORDER BY start DESC LIMIT 1", {int64_t(KindShutdown), t - 1, t + 1});
    const int64_t id = ev.empty() ? 0 : ev[0].t;
    double limit = ev.empty() ? -1 : ev[0].a;
    auto kept = query(db, "SELECT CAST(MAX(t) AS INTEGER), MAX(cpu), MAX(temp), MAX(fan), MAX(guard) FROM samples WHERE t >= ? AND t <= ?", {end - 120, end});
    if (kept.empty() || kept[0].t == 0)
        kept = query(db, "SELECT CAST(MAX(t) AS INTEGER), MAX(cpu), MAX(temp), MAX(fan), MAX(guard) FROM event_samples WHERE event_id = ?", {id});
    double cpu = -1, temp = -1, fan = -1, guard = -1;
    if (!kept.empty() && kept[0].t > 0) {
        cpu = kept[0].a, temp = kept[0].b;
        fan = kept[0].s1.empty() ? -1 : std::atof(kept[0].s1.c_str());
        guard = kept[0].s2.empty() ? -1 : std::atof(kept[0].s2.c_str());
        std::vector<std::string> highs;
        highs.push_back("the CPU up to " + fmt("%.0f", cpu) + "% busy" + (temp >= 0 ? " and " + fmt("%.0f C", temp) : ""));
        if (guard >= 0 && limit > 0)
            highs.push_back("the shutdown sensor up to " + fmt("%.0f C", guard) + " (" + fmt("%.0f C", std::max(0.0, limit - guard)) + " below its " +
                            fmt("%.0f C", limit) + " limit)");
        if (fan >= 0) highs.push_back("the fan up to " + fmt("%.0f", fan) + "%");
        said.push_back("Its last two minutes" + std::string(kept[0].t < end - 5 ? " (the readings kept, to " + clock_text(kept[0].t) + ")" : "") +
                       " had " + unllm::join_list(highs) + ".");
    }
    // The hour's record holds every reading of the hour, the ones after those kept too: its highs, where higher.
    const int64_t hour = (end - 1) / 3600 * 3600;
    std::vector<std::string> higher;
    for (const auto& h : query(db, "SELECT 0, max, 0, metric FROM samples_hourly WHERE hour_ts = ? AND metric IN ('temp', 'cpu', 'fan') "
                                   "ORDER BY CASE metric WHEN 'temp' THEN 0 WHEN 'cpu' THEN 1 ELSE 2 END", {hour})) {
        if (h.s1 == "temp" && h.a > temp) higher.push_back(fmt("%.0f C", h.a));
        if (h.s1 == "cpu" && h.a > cpu) higher.push_back("the CPU " + fmt("%.0f", h.a) + "% busy");
        if (h.s1 == "fan" && h.a > fan) higher.push_back("the fan at " + fmt("%.0f", h.a) + "%");
    }
    if (!higher.empty() && cpu >= 0)
        said.push_back("The hour's record, from " + clock_text(hour).substr(0, 5) + " to the stop, peaked higher: " + unllm::join_list(higher) + ".");
    std::vector<std::string> busy;
    for (const auto& c : query(db, "SELECT rank, share, 0, name, cmdline FROM event_culprits WHERE event_id = ? AND kind != 'unseen' ORDER BY rank LIMIT 3", {id})) {
        if (c.a < 0.05) continue;
        const std::string cmd = command_text(c.s2);
        busy.push_back(c.s1 + " (" + (cmd.empty() || cmd == c.s1 ? "" : cmd + ", ") + fmt("%.0f", 100 * c.a) + "% of the CPU time)");
    }
    if (!busy.empty())
        said.push_back(std::string(busy.size() == 1 ? "The busiest program in its last two minutes was " : "The busiest programs in its last two minutes were ") +
                       unllm::join_list(busy) + ".");
    // Nothing recorded names the cause; how near the shutdown point it ran (the recorder's own line for "near") is
    // what the records can add.
    const double margin = guard >= 0 && limit > 0 ? std::max(0.0, limit - guard) : ev.empty() ? -1 : ev[0].b;
    if (margin >= 0 && margin <= Triggers{}.shutdown_margin)
        said.push_back("The records don't say why it stopped; running within " + fmt("%.0f C", margin) +
                       " of the shutdown point just before makes heat the likely reason (the firmware cutting the power), though nothing recorded proves it.");
    else if (!said.empty())
        said.push_back("The records don't say why it stopped.");
    std::string text;
    for (const auto& s : said) text += (text.empty() ? "" : " ") + s;
    return text;
}

// The programs that ran in a stretch, busiest first: each process's CPU shared out over its run (procs keeps a
// lifetime total), with the runs, their first start and last end, and the biggest memory.
std::string programs_ran(sqlite3* db, int64_t from, int64_t to, int64_t now) {
    std::vector<std::string> named;
    for (const auto& p : query(db, "SELECT CAST(MIN(started) AS INTEGER), SUM(cpu_seconds * (MIN(IFNULL(ended, ?), ?) - MAX(started, ?)) / "
                                   "MAX(1.0, IFNULL(ended, ?) - started)), MAX(peak_rss), name, COUNT(*) || ' ' || CAST(MAX(MIN(IFNULL(ended, ?), ?)) AS INTEGER) "
                                   "FROM procs WHERE started < ? AND IFNULL(ended, ?) > ? GROUP BY name ORDER BY 2 DESC LIMIT 4",
                               {now, to, from, now, now, to, to, now, from})) {
        if (p.a < 1) continue;
        const int64_t runs = std::atoll(p.s2.c_str()), last = std::atoll(p.s2.c_str() + p.s2.find(' ') + 1);
        named.push_back(p.s1 + " (" + (runs > 1 ? unllm::number_word(runs) + " runs, " : "") + when_text(std::max(p.t, from)) + " to " + when_text(last) +
                        (p.b >= 1e8 ? ", up to " + human_bytes(p.b) : "") + ")");
    }
    return named.empty() ? "" : "The programs that ran most: " + unllm::join_list(named) + ".";
}

// A failed unit's name, from its log row's detail: the journal's own line ("systemd: <unit>: Failed with result
// 'oom-kill'."). A unit name may hold a colon but never ": " or a space. Rows written before the recorder kept names
// whole hold a cut name in the title ("dbus- failed: 1.2-org.kde.KSplash@2.service"); the detail is whole.
std::string failed_unit(const std::string& title, const std::string& detail) {
    std::string line = detail.substr(0, detail.find('\n'));
    if (const auto at = line.find(": "); at != std::string::npos) line = line.substr(at + 2);  // the journal's identifier
    const auto end = line.find(": ");
    if (end != std::string::npos && line.find(' ') > end) return line.substr(0, end);
    return title.substr(0, title.find(" failed"));
}

// How systemd says a unit failed: "oom-kill" in "... Failed with result 'oom-kill'.", "" when the line says nothing.
std::string unit_result(const std::string& detail) {
    const auto at = detail.find("result '");
    if (at == std::string::npos) return "";
    const auto end = detail.find('\'', at + 8);
    return detail.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8);
}

// A unit as a person reads it: a D-Bus activated unit by its bus name ("dbus-:1.2-org.kde.KSplash@2.service" is
// org.kde.KSplash@2.service).
std::string unit_name(const std::string& unit) {
    if (unit.rfind("dbus-:", 0) == 0)
        if (const auto dash = unit.find('-', 6); dash != std::string::npos) return unit.substr(dash + 1);
    return unit;
}

// The same name for every instance of a unit: its template ("@2" dropped), random ids folded ("vte-spawn-*.scope").
std::string unit_kind(const std::string& unit) {
    std::string u = unit_name(unit);
    if (const auto at = u.find('@'), dot = u.rfind('.'); at != std::string::npos && dot != std::string::npos && dot > at)
        u.erase(at, dot - at);
    return fold_unit(u);
}

// The crash records in a window, oldest first: the recorder's problems rows (core dumps and segfaults, the kernel's
// out-of-memory kills) and the units systemd reported failed, with systemd's result.
struct Mishap {
    int64_t t = 0;
    std::string who;      // the program ("ktorrent") or the unit as read ("ollama.service")
    std::string text;     // "ktorrent crashed", "ollama.service was killed when memory ran out"
    std::string unit;     // the failed unit as logged, "" for a crash
    bool memory = false;  // killed when memory ran out
    int count = 1;        // a burst folded into one row ("zsh crashed 8 times between ...") counts every crash in it
};
std::vector<Mishap> mishaps(sqlite3* db, int64_t from, int64_t to) {
    std::vector<Mishap> out;
    // The recorder's titles: "<program> crashed", "<program> crashed N times between A and B" (the first of the
    // burst has its own row), "Out of memory: the kernel killed <program>".
    for (const auto& row : query(db, "SELECT t, 0, 0, title FROM log WHERE category = 'problems' AND t >= ? AND t < ?", {from, to})) {
        Mishap m;
        m.t = row.t;
        m.text = row.s1;
        if (const auto at = row.s1.find(" crashed"); at != std::string::npos) {
            m.who = row.s1.substr(0, at);
            if (row.s1.find(" times between ", at) != std::string::npos) m.count = std::max(1, std::atoi(row.s1.c_str() + at + 9) - 1);
        } else if (const auto k = row.s1.find(" killed "); k != std::string::npos) {
            m.who = row.s1.substr(k + 8);
            m.memory = true;
        }
        out.push_back(m);
    }
    for (const auto& row : query(db, "SELECT t, 0, 0, title, detail FROM log WHERE category = 'services' AND t >= ? AND t < ?", {from, to})) {
        Mishap m;
        m.t = row.t;
        m.unit = failed_unit(row.s1, row.s2);
        m.who = unit_name(m.unit);
        const std::string result = unit_result(row.s2);
        m.memory = result == "oom-kill";
        m.text = m.who + (m.memory ? " was killed when memory ran out" : " failed" + (result.empty() ? std::string() : " (" + result + ")"));
        out.push_back(m);
    }
    std::stable_sort(out.begin(), out.end(), [](const Mishap& a, const Mishap& b) { return a.t < b.t; });
    return out;
}

// The sleeps the session log holds, oldest first: from "Went to sleep" to the "Woke from sleep" after it (0 while
// still asleep), and whether the lid was closed just before.
struct Sleep {
    int64_t went = 0, woke = 0;
    bool lid = false;
};
std::vector<Sleep> sleeps(sqlite3* db, int64_t now) {
    std::vector<Sleep> out;
    int64_t lid = 0;
    for (const auto& row : query(db, "SELECT t, 0, 0, title FROM log WHERE category = 'session' AND t <= ? AND "
                                     "(title = 'Went to sleep' OR title LIKE 'Woke%' OR title = 'Lid closed') ORDER BY t", {now})) {
        if (row.s1 == "Lid closed") lid = row.t;
        else if (row.s1 == "Went to sleep") out.push_back({row.t, 0, row.t - lid <= 60});
        else if (!out.empty() && out.back().woke == 0) out.back().woke = row.t;
    }
    return out;
}

// Every boot the records know, oldest first: the journal's boots and, for the weeks before the journal reaches, the
// recorder's own row for pid 1 (systemd starts as the machine boots). Starts within ten minutes are one boot.
std::vector<int64_t> boot_starts(sqlite3* db, int64_t now) {
    std::vector<int64_t> all;
    for (const auto& b : boots())
        if (b.first <= now) all.push_back(b.first);
    for (const auto& row : query(db, "SELECT CAST(started AS INTEGER) FROM procs WHERE pid = 1 AND started <= ?", {now})) all.push_back(row.t);
    std::sort(all.begin(), all.end());
    std::vector<int64_t> out;
    for (int64_t t : all)
        if (out.empty() || t - out.back() > 600) out.push_back(t);
    return out;
}

// What the session log shows in a window: the sleeps and wakes, then its other rows (the power button, logging out
// and in, the charger), each said once a minute.
std::string session_story(sqlite3* db, int64_t from, int64_t to, int64_t now) {
    std::vector<std::string> said;
    for (const auto& s : sleeps(db, now)) {
        if (s.went >= to || (s.woke && s.woke < from)) continue;
        if (s.went >= from) said.push_back("went to sleep at " + when_text(s.went) + (s.woke ? " and woke at " + when_text(s.woke) : ""));
        else said.push_back("woke from sleep at " + when_text(s.woke) + " (asleep since " + when_text(s.went) + ")");
    }
    std::map<std::string, int64_t> last;  // each row's head, when it was last said
    for (const auto& row : query(db, "SELECT t, 0, 0, title FROM log WHERE category = 'session' AND t >= ? AND t < ? ORDER BY t", {from, to})) {
        if (row.s1 == "Went to sleep" || row.s1.rfind("Woke", 0) == 0 || row.s1.rfind("Lid ", 0) == 0) continue;
        std::string head = row.s1.substr(0, std::min(row.s1.find(':'), row.s1.find(" (")));
        if (head.empty()) continue;
        head[0] = char(std::tolower(static_cast<unsigned char>(head[0])));
        if (auto it = last.find(head); it != last.end() && row.t - it->second < 60) continue;
        last[head] = row.t;
        said.push_back(head + " at " + when_text(row.t));
    }
    if (said.size() > 6) {
        const std::size_t more = said.size() - 5;
        said.resize(5);
        said.push_back(std::to_string(more) + " more rows");
    }
    return unllm::join_list(said);
}

std::string asleep_at(sqlite3* db, int64_t at, int64_t now) {
    for (const auto& s : sleeps(db, now))
        if (s.went <= at && (s.woke == 0 || s.woke > at))
            return "At " + when_text(at) + " it was asleep, from " + when_text(s.went) + (s.woke ? " to " + when_text(s.woke) : " on") + ".";
    return "";
}

// What the machine was doing at a moment ("at 3am"): the readings five minutes either side of it, whether an event was
// under way, and the busiest programs of those minutes; asleep when the session log says so. "" when the records hold
// neither. Quiet is below the recorder's own line for a CPU event, with no event under way.
std::string moment_story(sqlite3* db, int64_t at, int64_t now) {
    const int64_t a = at - 300, b = at + 300;
    auto agg = query(db, "SELECT COUNT(*), AVG(cpu), MAX(cpu), IFNULL(AVG(mem_pct), -1) || ' ' || IFNULL(AVG(temp), -1) || ' ' || IFNULL(AVG(fan), -1) "
                         "FROM samples WHERE cpu IS NOT NULL AND t >= ? AND t < ?", {a, b});
    if (agg.empty() || agg[0].t == 0) {
        for (const auto& s : sleeps(db, now))
            if (s.went <= at && (s.woke == 0 || s.woke > at))
                return "At " + when_text(at) + " it was asleep, from " + when_text(s.went) + (s.woke ? " to " + when_text(s.woke) : " on") + ".";
        return "";
    }
    double mem = -1, temp = -1, fan = -1;
    std::istringstream(agg[0].s1) >> mem >> temp >> fan;
    auto ev = query(db, "SELECT COUNT(*) FROM events WHERE start < ? AND \"end\" >= ?", {b, a});
    const bool calm = ev.empty() || ev[0].t == 0;
    std::string text = "At " + when_text(at) + " it was awake and " + (calm && agg[0].b < Triggers{}.cpu_total ? "quiet" : "busy") + ": from " +
                       clock_text(a).substr(0, 5) + " to " + clock_text(b).substr(0, 5) + " the CPU was " + fmt("%.0f", agg[0].a) + "% busy on average (at most " +
                       fmt("%.0f", agg[0].b) + "%)" + (mem >= 0 ? ", memory " + fmt("%.0f", mem) + "% used" : "") + (temp >= 0 ? ", " + fmt("%.0f C", temp) : "") +
                       (fan >= 0 ? ", the fan at " + fmt("%.0f", fan) + "%" : "") + "." + (calm ? " No event was under way." : "");
    std::vector<std::string> busy;
    for (const auto& x : query(db, "SELECT 0, SUM(cpu_seconds), 0, name FROM minute_top WHERE minute >= ? AND minute < ? GROUP BY name ORDER BY 2 DESC LIMIT 4",
                               {a / 60 * 60, b}))
        busy.push_back(x.s1 + fmt(" (%.0f s of CPU)", x.a));
    if (!busy.empty()) text += " The busiest programs then: " + unllm::join_list(busy) + ".";
    return text;
}

// The packages one software row lists: under each heading ("Upgraded:") a line "  name (version)" or
// "  name (old -> new)", as the recorder writes them from pacman's log.
struct PackageLine {
    std::string verb, name, version;
};
std::vector<PackageLine> package_lines(const std::string& detail) {
    std::vector<PackageLine> out;
    std::string verb;
    std::istringstream in(detail);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("  ", 0) == 0) {
            const auto open = line.find(" (");
            const std::string name = trim(line.substr(0, open));
            out.push_back({verb, name, open == std::string::npos ? "" : line.substr(open + 2, line.rfind(')') - open - 2)});
        } else if (!line.empty() && line.back() == ':') {
            verb = line.substr(0, line.size() - 1);
        }
    }
    return out;
}

std::string named_package(sqlite3* db, const unllm::Analyzer& an, const std::string& question) {
    std::set<std::string> known;
    for (const auto& row : query(db, "SELECT t, 0, 0, detail FROM log WHERE category = 'software'", {}))
        for (const auto& p : package_lines(row.s1)) known.insert(p.name);
    if (known.empty()) return "";
    const auto words = an.known_words(false);
    const std::set<std::string> own(words.begin(), words.end());
    std::string topical;  // a package that is also one of the analyzer's words ("cuda"), when no other is named
    for (const auto& t : unllm::tokenize(question)) {
        if (t.lower.size() < 2 || unllm::stop_word(t.lower) || !known.count(t.lower)) continue;
        if (!own.count(t.lower)) return t.lower;
        if (topical.empty()) topical = t.lower;
    }
    return topical;
}

// One software log row told: its packages by name when there are few (eight at most; the row's own title counts them
// otherwise), an upgrade's versions when there are three or fewer, and who started it: the command pacman logged, or
// the administrator command a person ran in the minute before ("user ran sudo pacman -Syy onnxruntime-cuda at
// 20:40"). started says whether either was found.
std::string software_change(sqlite3* db, int64_t t, const std::string& title, const std::string& detail, bool* started) {
    const auto lines = package_lines(detail);
    std::string text = title;
    if (!lines.empty() && lines.size() <= 8) {
        std::vector<std::string> verbs;
        for (const auto& p : lines)
            if (std::find(verbs.begin(), verbs.end(), p.verb) == verbs.end()) verbs.push_back(p.verb);
        text.clear();
        for (const auto& v : verbs) {
            std::vector<std::string> names;
            for (const auto& p : lines) {
                if (p.verb != v) continue;
                const auto arrow = p.version.find(" -> ");
                names.push_back(p.name + (lines.size() <= 3 && arrow != std::string::npos
                                              ? " from " + p.version.substr(0, arrow) + " to " + p.version.substr(arrow + 4) : ""));
            }
            std::string verb = v;
            for (auto& c : verb) c = char(std::tolower(static_cast<unsigned char>(c)));
            text += (text.empty() ? "" : "; ") + verb + " " + unllm::join_list(names);
        }
    }
    if (!text.empty()) text[0] = char(std::tolower(static_cast<unsigned char>(text[0])));
    text = when_text(t) + ", " + text;
    *started = false;
    if (const auto at = detail.find("Command: "); at != std::string::npos) {
        text += " (the command was " + command_text(detail.substr(at + 9, detail.find('\n', at) - at - 9), 8) + ")";
        *started = true;
    } else if (auto admin = query(db, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'admin' AND t >= ? AND t <= ? ORDER BY t DESC LIMIT 1",
                                  {t - 60, t});
               !admin.empty()) {
        // "Ran as administrator (sudo) by user: /usr/bin/pacman -Syy onnxruntime-cuda sentencepiece"
        const std::string& a = admin[0].s1;
        const auto open = a.find('('), close = a.find(')'), by = a.find(" by "), colon = a.find(": ");
        if (open != std::string::npos && close > open && by != std::string::npos && colon > by) {
            std::time_t tt = admin[0].t;
            std::tm tm{};
            localtime_r(&tt, &tm);
            char hm[8];
            std::strftime(hm, sizeof hm, "%H:%M", &tm);
            text += " (" + a.substr(by + 4, colon - by - 4) + " ran " + a.substr(open + 1, close - open - 1) + " " + command_text(a.substr(colon + 2), 8) +
                    " at " + hm + ")";
            *started = true;
        }
    }
    return text;
}

// The running kernel's history: the software log's changes to the package that owns its image (pacman's own answer),
// whether the version installed now is among them, and the kernel each boot the journal still holds ran (up to now).
std::string kernel_history(sqlite3* db, const std::string& release, int64_t now) {
    std::string text;
    const std::string pkg = trim(run_out("pacman -Qqo '/usr/lib/modules/" + release + "/vmlinuz' 2>/dev/null"));
    if (!pkg.empty() && pkg.find('\n') == std::string::npos) {
        std::vector<std::string> said;
        std::string last;
        for (const auto& row : query(db, "SELECT CAST(t AS INTEGER), 0, 0, detail FROM log WHERE category = 'software' AND t <= ? ORDER BY t", {now}))
            for (const auto& p : package_lines(row.s1)) {
                if (p.name != pkg) continue;
                std::string verb = p.verb;
                for (auto& c : verb) c = char(std::tolower(static_cast<unsigned char>(c)));
                const auto arrow = p.version.find(" -> ");
                said.push_back(verb + (arrow == std::string::npos ? " at " + p.version
                                                                  : " from " + p.version.substr(0, arrow) + " to " + p.version.substr(arrow + 4)) +
                               " on " + moment_text(row.t));
                last = arrow == std::string::npos ? p.version : p.version.substr(arrow + 4);
            }
        std::string installed = trim(run_out("pacman -Q '" + pkg + "' 2>/dev/null"));
        installed = installed.substr(installed.find(' ') + 1);
        if (said.empty()) {
            text += " The software log holds no change to its package, " + pkg + ".";
        } else {
            text += " The software log's changes to its package, " + pkg + ": " + unllm::join_list(said) + ".";
            if (!installed.empty() && installed != last)
                text += " The update to " + installed + ", the version installed now, isn't in the software log.";
        }
    }
    // "Linux version 7.2.9-2-cachyos (...)": the first kernel line of each boot.
    std::vector<std::pair<int64_t, std::string>> runs;  // each version's first boot, in order
    for (const auto& e : journal("_TRANSPORT=kernel --grep='^Linux version' --until=@" + std::to_string(now))) {
        const std::string m = jfield(e, "MESSAGE");
        if (m.size() <= 14) continue;
        const std::string v = m.substr(14, m.find(' ', 14) - 14);
        if (runs.empty() || runs.back().second != v) runs.push_back({std::atoll(jfield(e, "__REALTIME_TIMESTAMP").c_str()) / 1000000, v});
    }
    if (runs.size() == 1) {
        text += " Every boot the journal still holds, since " + moment_text(runs[0].first) + ", ran " + runs[0].second + ".";
    } else if (runs.size() > 1) {
        std::vector<std::string> v;
        for (const auto& [t, ver] : runs) v.push_back(ver + " from " + moment_text(t));
        text += " The boots the journal still holds ran " + unllm::join_list(v) + ".";
    }
    return text;
}

// The memory's sticks from udev's copy of the firmware's memory table (systemd reads it as root at boot and keeps it
// where anyone can read it): "two 8 GB DDR4 sticks running at 2933 MT/s (rated 3200 MT/s), filling both of its slots;
// it takes up to 32 GB". Empty when the table isn't there.
std::string memory_sticks(const std::string& udev_record) {
    std::map<std::string, std::string> v;
    for (const auto& line : split(read_file(udev_record).value_or(""), '\n'))
        if (line.rfind("E:MEMORY_", 0) == 0)
            if (const auto eq = line.find('='); eq != std::string::npos) v[line.substr(2, eq - 2)] = line.substr(eq + 1);
    struct Stick { double gb; std::string type; long speed, rated; };
    std::vector<Stick> sticks;
    for (int n = 0; v.count("MEMORY_DEVICE_" + std::to_string(n) + "_LOCATOR"); ++n) {
        auto f = [&](const char* key) { return v["MEMORY_DEVICE_" + std::to_string(n) + "_" + key]; };
        const double bytes = std::atof(f("SIZE").c_str());
        if (bytes > 0) sticks.push_back({bytes / double(1ull << 30), f("TYPE"), std::atol(f("CONFIGURED_SPEED_MTS").c_str()), std::atol(f("SPEED_MTS").c_str())});
    }
    if (sticks.empty()) return "";
    bool same = true;
    for (const auto& s : sticks) same = same && s.gb == sticks[0].gb && s.type == sticks[0].type;
    auto size = [](const Stick& s) { return fmt("%g GB", s.gb) + (s.type.empty() ? "" : " " + s.type); };
    std::string text;
    if (same) {
        text = sticks.size() == 1 ? "one " + size(sticks[0]) + " stick" : unllm::number_word(long(sticks.size())) + " " + size(sticks[0]) + " sticks";
    } else {
        std::vector<std::string> each;
        for (const auto& s : sticks) each.push_back(unllm::indefinite(size(s)));
        text = unllm::join_list(each) + " stick";
    }
    const long speed = sticks[0].speed > 0 ? sticks[0].speed : sticks[0].rated, rated = sticks[0].rated;
    if (speed > 0) text += " running at " + std::to_string(speed) + " MT/s" + (rated > speed ? " (rated " + std::to_string(rated) + " MT/s)" : "");
    const long slots = std::atol(v["MEMORY_ARRAY_NUM_DEVICES"].c_str());
    const double most = std::atof(v["MEMORY_ARRAY_MAX_CAPACITY"].c_str()) / double(1ull << 30);
    if (slots > 0 && long(sticks.size()) == slots)
        text += ", filling " + (slots == 2 ? std::string("both") : "all " + unllm::number_word(slots)) + " of its slots";
    else if (slots > 0)
        text += ", in " + unllm::number_word(long(sticks.size())) + " of its " + unllm::number_word(slots) + " slots";
    if (most > 0) text += fmt("; it takes up to %g GB", most);
    return text;
}

// The swap's size over a window: the size recorded with its readings (samples.swap_total), else the live size when it
// is clearly the same device (every size recorded equals it, or none was recorded and the window's peak is the live
// size to the byte), as the swap answer decides. 0 when it is not known.
double swap_size(sqlite3* db, int64_t from, int64_t to, double live) {
    auto in = query(db, "SELECT 0, MAX(swap_total) FROM samples WHERE t >= ? AND t < ?", {from, to});
    if (!in.empty() && in[0].a > 0) return in[0].a;
    if (live <= 0) return 0;
    auto sizes = query(db, "SELECT COUNT(DISTINCT swap_total), MAX(swap_total) FROM samples WHERE swap_total > 0", {});
    if (!sizes.empty() && sizes[0].t > 0) return sizes[0].t == 1 && sizes[0].a == live ? live : 0;
    auto peak = query(db, "SELECT 0, MAX(swap_used) FROM samples WHERE t >= ? AND t < ?", {from, to});
    return !peak.empty() && peak[0].a == live ? live : 0;
}

}  // namespace

bool Ask::memory_choice(const unllm::Reading& r, AskResult* res) {
    if (r.alternatives.size() != 2 || !kinds_.ready() || r.window.live || r.window.present) return false;
    // Each side by meaning: a kind of the memory topic, the swap's own kinds or the others (RAM).
    bool swap_side[2] = {false, false};
    for (int i = 0; i < 2; ++i) {
        const KindMatch m = kinds_.match(r.alternatives[std::size_t(i)]);
        const QuestionKind* k = kinds_.find(m.id);
        if (!k || k->topic != "memory") return false;
        swap_side[i] = m.id.rfind("memory.swap.", 0) == 0;
    }
    if (swap_side[0] == swap_side[1]) return false;
    const int64_t from = r.window.from, to = r.window.to;
    card_.reading = r;
    card_.window = r.window;
    card_.topic = Intent::Memory;
    res->branch = "choice:memory";
    auto agg = query(db_, "SELECT COUNT(*), MAX(mem_pct), MAX(swap_used), MAX(pressure), CAST(MIN(t) AS INTEGER) || ' ' || CAST(MAX(t) AS INTEGER) "
                          "FROM samples WHERE t >= ? AND t < ? AND mem_pct IS NOT NULL", {from, to});
    if (agg.empty() || agg[0].t == 0) {
        res->body = "I have no memory readings between " + span_text(from, to, " and ") + ": the per-second readings are kept for a day.";
        return true;
    }
    if (!sampler_) sampler_ = std::make_unique<Sampler>();
    const SnapshotPtr s = sampler_->sample();
    const double live_swap = s ? double(s->mem.swap_total) : 0, ram = s ? double(s->mem.total) : 0;
    const double mem_peak = agg[0].a, swap_peak = agg[0].b, waited = std::atof(agg[0].s1.c_str());
    // The swap is at its limit at its size (a reading within 0.1% of it); the RAM when programs waited on memory more
    // than the recorder's own line for a memory event.
    const double size = swap_size(db_, from, to, live_swap), line = Triggers{}.pressure;
    const std::string over = std::to_string(line);
    auto full = size > 0 ? query(db_, "SELECT COUNT(*), MIN(t), MAX(t), SUM(pressure > " + over + ") FROM samples WHERE t >= ? AND t < ? AND swap_used >= ?",
                                 {from, to, int64_t(0.999 * size)})
                         : std::vector<Row>{};
    const int64_t at_cap = full.empty() ? 0 : full[0].t, both = full.empty() ? 0 : std::atoll(full[0].s1.c_str());
    const bool swap_full = at_cap > 0, ram_short = waited > line;
    auto top = query(db_, "SELECT CAST(t AS INTEGER), mem_pct FROM samples WHERE t >= ? AND t < ? ORDER BY mem_pct DESC, t LIMIT 1", {from, to});
    std::string text = swap_full && ram_short ? (both > 0 ? "Both, at the same time: the swap was full while programs waited on memory."
                                                          : "Both, at different times: the swap was full, and at other times programs waited on memory.")
                       : swap_full ? "The swap: it was full, while the RAM still had room."
                       : ram_short ? "The RAM: programs waited on memory while the swap still had room."
                                   : "Neither was at its limit.";
    if (size > 0) {
        text += swap_full ? " The swap was full: " + human_bytes(swap_peak) + " of " + human_bytes(size) + ", at its cap for about " +
                                duration_text(at_cap) + " in all (" + std::to_string(at_cap) + " readings) between " +
                                when_text(int64_t(full[0].a)) + " and " + when_text(int64_t(full[0].b)) + "."
                          : " The swap peaked at " + human_bytes(swap_peak) + " of " + human_bytes(size) + " (" + fmt("%.0f", 100 * swap_peak / size) + "%).";
        if (s && size == live_swap) text += zram_text(s->mem);  // the device measured is the one there now
    } else {
        text += " The swap peaked at " + human_bytes(swap_peak) + "; its size wasn't recorded then, so I can't say how full it was.";
    }
    text += " RAM peaked at " + fmt("%.1f", mem_peak) + "%" + (ram > 0 ? " of " + human_bytes(ram) : std::string()) +
            (top.empty() ? std::string() : " at " + when_text(top[0].t)) + ", and programs " +
            (ram_short ? "waited on memory up to " + fmt("%.1f", waited) + "% of the time."
                       : "hardly waited on memory (" + fmt("%.1f", waited) + "% of the time at most).");
    // What it led to and who held the memory.
    std::vector<std::string> killed;
    const auto kills = recorded_kills(db_, from, to);
    for (const auto& k : kills)
        if (killed.size() < 3) killed.push_back((k.process.empty() ? unit_name(k.unit) : k.process) + " at " + when_text(k.t));
    if (kills.size() > killed.size()) killed.push_back(std::to_string(kills.size() - killed.size()) + " more");
    if (!killed.empty()) text += " Memory ran out: the out-of-memory killer stopped " + unllm::join_list(killed) + ".";
    std::vector<std::string> held;
    for (const auto& x : query(db_, "SELECT 0, MAX(peak_rss), 0, name, cmdline FROM minute_top WHERE minute >= ? AND minute < ? "
                                    "GROUP BY pid ORDER BY 2 DESC LIMIT 6", {from - 60, to})) {
        const std::string who = shown_name(x.s1, x.s2) + " at " + human_bytes(x.a);
        if (held.size() < 2 && std::none_of(held.begin(), held.end(), [&](const std::string& h) { return h.rfind(shown_name(x.s1, x.s2) + " at ", 0) == 0; }))
            held.push_back(who);
    }
    if (!held.empty()) text += " The biggest holders were " + unllm::join_list(held) + ".";
    res->body = text;
    const auto ends = agg[0].s2;
    res->receipts.push_back(std::to_string(agg[0].t) + " readings, one a second, from " + when_text(std::atoll(ends.c_str())) + " to " +
                            when_text(std::atoll(ends.c_str() + ends.find(' ') + 1)));
    return true;
}

AskResult Ask::answer_records(const std::string& kind, const unllm::Reading& r, int64_t now) {
    AskResult res;
    const int64_t from = r.window.stated ? r.window.from : now - 86400, to = r.window.stated ? r.window.to : now;
    const std::string span = r.window.stated ? "between " + span_text(from, to, " and ") : "in the last day";
    const bool polar = r.form == Form::Polar;
    // The program the question names, and a unit named by everyday words ("the terminal": a terminal tab).
    std::string program;
    for (const auto& e : r.entities)
        if (e.type == "program") program = e.value;
    const std::string named_unit = !named_.empty() && named_[0].by_unit ? named_[0].program.unit : "";
    // Whether a crash record is about that program or unit: the program by name ("ktorrent", "ollama" for
    // ollama.service), or the failed unit's own process (systemd started plasmashell in plasma-plasmashell.service;
    // "ollama show" typed in a terminal tab doesn't make the tab ollama's).
    auto about = [&](const Mishap& m) {
        if (program.empty() && named_unit.empty()) return true;
        if (!m.unit.empty() && !named_unit.empty()) return fold_unit(m.unit) == named_unit;
        if (program.empty()) return false;
        // A unit's failure is the service manager's own report: "systemd crashes" are all the units that failed.
        if (!m.unit.empty() && is_program(program, "systemd", "", "")) return true;
        if (is_program(program, m.unit.empty() ? m.who : m.who.substr(0, m.who.rfind('.')), "", "")) return true;
        if (m.unit.empty()) return false;
        auto in = query(db_, "SELECT COUNT(*) FROM procs WHERE unit = ? AND parent = 'systemd' AND is_program(?, name, exe, cmdline)", {},
                        {m.unit, program});
        return !in.empty() && in[0].t > 0;
    };
    auto count_of = [](const std::vector<Mishap>& v) {
        long n = 0;
        for (const auto& m : v) n += m.count;
        return n;
    };
    auto times = [](long n) { return n == 1 ? std::string("once") : n == 2 ? std::string("twice") : unllm::number_word(n) + " times"; };
    auto capital = [](std::string s) {
        if (!s.empty()) s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
        return s;
    };
    // Crash records by program, or by unit (every instance of a template together), the most first: "ask four times
    // (from Thu 14:35 to Thu 14:40)", "ollama.service twice (killed when memory ran out, at Thu 07:23 and Thu 10:53)".
    auto grouped = [&](const std::vector<Mishap>& v) {
        struct Group {
            long n = 0;
            int64_t first = 0, last = 0;
            std::string name;
            bool memory = true, unit = false;
        };
        std::map<std::string, Group> by;
        std::vector<std::string> order;
        for (const auto& m : v) {
            const std::string k = m.unit.empty() ? m.who : unit_kind(m.unit);
            Group& g = by[k];
            if (g.n == 0) {
                order.push_back(k);
                g.first = m.t;
                g.name = m.who;
                g.unit = !m.unit.empty();
            } else if (g.name != m.who) {
                g.name = k;  // several instances: the template's name
            }
            g.n += m.count;
            g.last = m.t;
            g.memory = g.memory && m.memory;
        }
        std::stable_sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) { return by[a].n > by[b].n; });
        std::vector<std::string> said;
        // Six named, the rest counted; one or two past six are named instead of counted.
        const std::size_t shown = order.size() <= 8 ? order.size() : 6;
        for (std::size_t i = 0; i < shown; ++i) {
            const Group& g = by[order[i]];
            const std::string why = g.memory ? "killed when memory ran out, " : g.unit ? "failed, " : "";
            said.push_back(g.name + " " + times(g.n) + " (" + why +
                           (g.n == 1   ? "at " + when_text(g.first)
                            : g.n == 2 ? "at " + when_text(g.first) + " and " + when_text(g.last)
                                       : "from " + when_text(g.first) + " to " + when_text(g.last)) +
                           ")");
        }
        if (order.size() > shown) said.push_back(std::to_string(order.size() - shown) + " more");
        return unllm::join_list(said);
    };
    // After a kill for memory: memory and swap just before it, from the samples; after a unit failed: the unit
    // started again.
    auto aftermath = [&](const Mishap& m) {
        std::vector<std::string> said;
        if (m.memory) {
            auto mem = query(db_, "SELECT 0, MAX(mem_pct), MAX(swap_used) FROM samples WHERE t >= ? AND t <= ?", {m.t - 10, m.t + 1});
            if (!mem.empty() && mem[0].a > 0)
                said.push_back("memory at " + fmt("%.0f", mem[0].a) + "%" + (mem[0].b > 0 ? " with " + human_bytes(mem[0].b) + " in swap" : ""));
        }
        if (!m.unit.empty()) {
            auto again = query(db_, "SELECT CAST(MIN(started) AS INTEGER) FROM procs WHERE started >= ? AND started < ? AND unit = ?",
                               {m.t, m.t + 600}, {m.unit});
            if (!again.empty() && again[0].t > 0) said.push_back("started again at " + when_text(again[0].t));
        }
        return said.empty() ? std::string() : " (" + unllm::join_list(said) + ")";
    };

    if (kind == "memory.swap.value.now") {
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr s = sampler_->sample();
        if (!s || s->mem.swap_total == 0) {
            res.body = "This machine has no swap set up.";
        } else {
            const double used = double(s->mem.swap_total - s->mem.swap_free);
            res.body = "Swap: " + human_bytes(used) + " used of " + human_bytes(double(s->mem.swap_total)) + " (" +
                       fmt("%.0f", 100.0 * used / double(s->mem.swap_total)) + "%)." + zram_text(s->mem);
            auto procs = s->procs;  // what fills it: the biggest holders of memory
            std::sort(procs.begin(), procs.end(), [](const auto& a, const auto& b) { return a.rss + a.swap > b.rss + b.swap; });
            std::vector<std::string> top;
            for (std::size_t i = 0; i < procs.size() && top.size() < 3; ++i) top.push_back(procs[i].name + " " + human_bytes(double(procs[i].rss + procs[i].swap)));
            if (!top.empty()) res.body += " The biggest memory holders: " + unllm::join_list(top) + ".";
            // Why: pages go to swap when memory runs short, and stay there until something reads them.
            if (r.wants == unllm::Wants::Cause) {
                auto mem = series(db_, "mem_pct", "mem_pct", now - 86400, now);
                const Row* hi = nullptr;
                for (const auto& x : mem)
                    if (!hi || x.a > hi->a) hi = &x;
                if (hi) {
                    const std::string who = top_culprit(db_, hi->t - 600, hi->t + 600, KindMemory, "");
                    res.body += " Swap fills when memory runs short: in the last day memory peaked at " + fmt("%.0f", hi->a) + "% at " +
                                when_text(hi->t) + (who.empty() ? "" : ", mostly from " + who) +
                                ". Pages pushed out then stay in swap until something needs them, so a full swap is a sign of that peak" +
                                (s->mem.percent() >= 85 ? std::string(", and memory is short now too, at ") + fmt("%.0f", s->mem.percent()) + "%."
                                                        : std::string(", not of memory being short now."));
                }
            }
        }
        res.branch = "records:swap-now";
        return res;
    }
    if (kind == "heat.fan.value.window" && r.form == unllm::Form::Polar) {
        // "was claude running when the fan hit its peak": the peak's moments, then the program at each.
        std::string program;
        for (const auto& e : r.entities)
            if (e.type == "program") program = e.value;
        const std::string a = program.empty() ? "" : peak_answer(db_, r.terms, "fan", "the fan", program, from, to, now, &res.receipts, &res.lead);
        if (!a.empty()) {
            res.body = a;
            res.branch = "records:fan-peak";
            return res;
        }
    }
    if (kind == "memory.swap.value.window") {
        // Swap use against its size, the size recorded with each reading (samples.swap_total). Readings from before
        // the size was recorded (NULL, or a database without the column) take the live size only when it is clearly
        // the same device: every size recorded since equals it, or none was recorded and the window's peak is the
        // live size to the byte. Otherwise the bytes alone.
        auto rows = query(db_, "SELECT CAST(t AS INTEGER), swap_used, IFNULL(swap_total, -1) FROM samples WHERE swap_used IS NOT NULL "
                               "AND t >= ? AND t < ? ORDER BY t", {from, to});
        if (rows.empty())
            rows = query(db_, "SELECT CAST(t AS INTEGER), swap_used, -1 FROM samples WHERE swap_used IS NOT NULL AND t >= ? AND t < ? ORDER BY t", {from, to});
        // Before the readings kept: each hour's highest use, with the size rolled up beside it.
        const bool hourly = rows.empty();
        if (hourly)
            rows = query(db_, "SELECT h.hour_ts, h.max, IFNULL((SELECT s.max FROM samples_hourly s WHERE s.metric = 'swap_total' AND s.hour_ts = h.hour_ts), -1) "
                              "FROM samples_hourly h WHERE h.metric = 'swap_used' AND h.hour_ts >= ? AND h.hour_ts < ? ORDER BY h.hour_ts", {from, to});
        if (rows.empty()) {
            res.body = "I have no swap readings " + span + ".";
            res.branch = "records:none";
            return res;
        }
        const Row* hi = &rows[0];
        const Row* lo = &rows[0];
        double sum = 0;
        bool unsized = false;
        for (const auto& x : rows) {
            if (x.a > hi->a) hi = &x;
            if (x.a < lo->a) lo = &x;
            sum += x.a;
            unsized = unsized || x.b <= 0;
        }
        const double avg = sum / double(rows.size());
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr s = sampler_->sample();
        const double live = s ? double(s->mem.swap_total) : 0;
        auto sizes = query(db_, "SELECT COUNT(DISTINCT swap_total), MAX(swap_total) FROM samples WHERE swap_total > 0", {});
        const bool recorded = !sizes.empty() && sizes[0].t > 0;
        double fallback = 0;
        std::string note;
        if (unsized && live > 0 && recorded && sizes[0].t == 1 && sizes[0].a == live) {
            fallback = live;
            note = " The size wasn't recorded with all of those readings; every size recorded is " + human_bytes(live) +
                   ", the swap's size now, so that is the size used.";
        } else if (unsized && live > 0 && !recorded && hi->a == live) {
            fallback = live;
            note = " The size wasn't recorded then; the peak is the swap's size now (" + human_bytes(live) + ") to the byte, so it is the same device.";
        }
        const double size = hi->b > 0 ? hi->b : fallback;
        int64_t full = 0;
        if (size > 0) {
            // At the cap: a reading within 0.1% of its size.
            int64_t first = 0, last = 0;
            for (const auto& x : rows) {
                const double cap = x.b > 0 ? x.b : fallback;
                if (cap > 0 && x.a >= 0.999 * cap) {
                    ++full;
                    if (!first) first = x.t;
                    last = x.t;
                }
            }
            res.body = "Swap use " + span + " peaked at " + human_bytes(hi->a) + " of " + human_bytes(size) + " (" + fmt("%.0f", 100.0 * hi->a / size) +
                       "%) at " + when_text(hi->t) + ", averaging " + human_bytes(avg) + ".";
            if (full > 0 && hourly)
                res.body += " It reached the cap in " + std::to_string(full) + (full == 1 ? " hour" : " hours") + ", between " + when_text(first) +
                            " and " + when_text(last) + ".";
            else if (full > 0)
                res.body += " It was full: at the cap for about " + duration_text(full) + " in all (" + std::to_string(full) +
                            " readings, one a second), between " + when_text(first) + " and " + when_text(last) + ".";
            res.body += note;
            if (s && size == live) res.body += zram_text(s->mem);  // the device measured is the one there now
        } else {
            res.body = "Swap use " + span + " peaked at " + human_bytes(hi->a) + " at " + when_text(hi->t) + ", averaging " + human_bytes(avg) +
                       ". The swap's size wasn't recorded then" +
                       (live > 0 ? " and the peak doesn't show it is the swap there now (" + human_bytes(live) + ")" : std::string()) +
                       ", so I can't say how full it was.";
        }
        res.receipts.push_back(std::to_string(rows.size()) + (hourly ? " hourly records" : " readings") + " from " + when_text(rows.front().t) + " to " +
                               when_text(rows.back().t));
        // "Was swap getting hit": yes when it filled, or grew by a twentieth of its size (a tenth of its peak, size unknown).
        if (r.form == Form::Polar) {
            const bool hit = full > 0 || hi->a - lo->a >= (size > 0 ? 0.05 * size : 0.1 * hi->a);
            res.lead = hit ? "Yes." : "No.";
            if (!hit) res.body += " It stayed between " + human_bytes(lo->a) + " and " + human_bytes(hi->a) + ", so it wasn't being hit.";
        }
        res.branch = "records:swap";
        return res;
    }
    if (kind == "heat.fan.value.window" || kind == "battery.value.window") {
        const bool fan = kind == "heat.fan.value.window";
        auto rows = series(db_, fan ? "fan" : "battery_pct", fan ? "fan" : "", from, to);
        // The battery's low points over the whole window, the log's where the per-second readings don't reach.
        int64_t readings_from = 0;
        const std::vector<Low> lows = fan ? std::vector<Low>{} : battery_lows(db_, from, to, &readings_from);
        const Low* least = nullptr;
        for (const auto& l : lows)
            if (!least || l.level < least->level) least = &l;
        auto from_where = [&](const Low& l) {
            return readings_from && l.t >= readings_from ? std::string() : l.plug ? " (when the charger went back in, from the log)" : " (from the hourly records)";
        };
        // A yes or no: whether it got low, whether it charged fully, or the battery's wear, which a window's levels can't
        // settle (asked_test; with the matcher's model 27 of 30 wordings measured go to their own test, the misses ties
        // that fall to the first).
        const int test = polar && !fan ? asked_test(r, {{"did the battery get low", "did the battery run down", "did the battery go flat"},
                                                        {"did the battery charge fully", "did the battery fill up to 100%", "did it reach a full charge"},
                                                        {"is the battery wearing out", "is the battery health getting worse", "is the battery losing capacity"}})
                                       : 0;
        if (polar && test == 1) {
            // "did the battery charge fully last night": the latest plug-in in the window (the log's charger rows and the
            // readings), then the highest level after it and when it first got there.
            const auto ps = plugs(db_, from, to);
            const Plug* in = nullptr;
            for (const auto& p : ps)
                if (p.in) in = &p;
            int64_t readings = 0;
            const Low top = battery_high(db_, in ? in->t : from, to, &readings);
            if (top.t) {
                const bool yes = top.level >= 99.5;
                const std::string when = top.plug ? "at " + when_text(top.t) + " (when the charger came out, from the log)"
                                         : readings && top.t >= readings ? "at " + when_text(top.t)
                                                                         : "in the hour from " + when_text(top.t) + " (from the hourly records)";
                res.lead = yes ? "Yes." : "No.";
                res.body = (in ? "It was plugged in at " + when_text(in->t) + (in->level >= 0 ? ", at " + fmt("%.0f", in->level) + "%," : "") + " and "
                               : std::string("It ")) +
                           (yes ? (in ? "reached 100% " : "was at 100% ") : "got no higher than " + fmt("%.0f", top.level) + "%, ") + when + ".";
                res.receipts.push_back((in ? "plugged in " + when_text(in->t) + ", " : std::string()) + "highest after it " + fmt("%.0f", top.level) +
                                       "% " + when);
                res.branch = "records:battery-full";
                return res;
            }
        }
        // The wear, a time set against another, or nothing to tell a full charge by: the window's levels, below.
        if (polar && !fan && test != 0) res.undecided = true;
        if (polar && test == 0 && least) {
            // "did the battery get low": against the level the system itself calls low.
            const double low = low_battery_level(roots_.etc);
            std::vector<std::string> under;
            for (const auto& l : lows)
                if (l.level < low) under.push_back(fmt("%.0f", l.level) + "% at " + when_text(l.t));
            const std::string line = fmt("%.0f", low) + "%, the level this system calls low,";
            res.lead = under.empty() ? "No:" : "Yes:";
            res.body = under.empty() ? "it never fell below " + line + " " + span + ". The lowest was " + fmt("%.0f", least->level) + "% at " +
                                           when_text(least->t) + from_where(*least) + "."
                                     : "it fell below " + line + " " + span + ", lowest " + fmt("%.0f", least->level) + "% at " +
                                           when_text(least->t) + from_where(*least) + ". Under " + fmt("%.0f", low) + "%: " + unllm::join_list(under) + ".";
            if (!readings_from || readings_from > from + 900)
                res.notes.push_back(std::string(readings_from ? "Before " + when_text(readings_from) + " the" : "The") +
                                    " levels come from the log's plug-ins (the level when the charger goes back in is the lowest of the spell on battery before it).");
            res.receipts.push_back(std::to_string(lows.size()) + " low points: the log's plug-ins" + (readings_from ? " and the per-second readings from " + when_text(readings_from) : ""));
            res.branch = "records:battery-low";
            return res;
        }
        if (rows.empty() && !fan) {
            // The per-second readings are kept a day. Before that: the hourly records (each hour's highest and
            // lowest level and its time on battery), else the log's plug and unplug rows.
            auto hours = query(db_, "SELECT hour_ts, max, min, on_battery_frac FROM samples_hourly WHERE metric = 'battery_pct' "
                                    "AND hour_ts >= ? AND hour_ts < ? ORDER BY hour_ts", {from, to});
            if (!hours.empty()) {
                const Row* lo = &hours[0];
                double top = hours[0].a;
                for (const auto& x : hours) {
                    if (x.b < lo->b) lo = &x;
                    top = std::max(top, x.a);
                }
                res.body = "From the hourly records (the per-second readings are kept a day): the battery " + span + " ranged from " +
                           fmt("%.0f", lo->b) + "% to " + fmt("%.0f", top) + "%, lowest " + fmt("%.0f", lo->b) + "% in the hour from " +
                           when_text(lo->t) + ".";
                // Each run of hours on battery: from the first hour's highest level to the last one's lowest.
                double lost = 0, secs = 0;
                for (std::size_t i = 0; i < hours.size(); ++i) {
                    if (std::atof(hours[i].s1.c_str()) <= 0) continue;
                    std::size_t j = i;
                    double on = 0;
                    for (; j < hours.size() && std::atof(hours[j].s1.c_str()) > 0; ++j) on += std::atof(hours[j].s1.c_str()) * 3600;
                    lost += std::max(0.0, hours[i].a - hours[j - 1].b);
                    secs += on;
                    i = j - 1;
                }
                if (secs >= 600 && lost > 0)
                    res.body += " On battery it lost about " + fmt("%.0f", lost) + "% in " + duration_text(int64_t(secs)) + ", about " +
                                fmt("%.0f", lost * 3600 / secs) + "% an hour.";
                res.receipts.push_back(std::to_string(hours.size()) + " hourly records from " + when_text(hours.front().t) + " to " + when_text(hours.back().t));
                res.branch = "records:battery-hourly";
                return res;
            }
            const auto ps = plugs(db_, from, to);
            if (!ps.empty()) {
                res.body = "From the log's plug and unplug entries (the per-second readings are kept a day): " + span + " it was " + plugs_text(ps) + ".";
                double lost = 0, secs = 0;
                std::size_t longest = 0;  // the plug-in that ended the longest spell on battery
                for (std::size_t i = 1; i < ps.size(); ++i)
                    if (!ps[i - 1].in && ps[i].in && ps[i - 1].level >= 0 && ps[i].level >= 0) {
                        lost += std::max(0.0, ps[i - 1].level - ps[i].level);
                        secs += double(ps[i].t - ps[i - 1].t);
                        if (!longest || ps[i].t - ps[i - 1].t > ps[longest].t - ps[longest - 1].t) longest = i;
                    }
                if (secs > 0 && lost > 0)
                    res.body += " On battery it lost " + fmt("%.0f", lost) + "% in " + duration_text(secs) + ", about " + fmt("%.0f", lost * 3600 / secs) +
                                "% an hour.";
                if (longest)
                    if (const std::string ran = programs_ran(db_, ps[longest - 1].t, ps[longest].t, now); !ran.empty()) res.body += " " + ran;
                res.branch = "records:battery-log";
                return res;
            }
        }
        if (rows.empty()) {
            res.body = std::string("I have no ") + (fan ? "fan" : "battery") + " readings " + span + ".";
            res.branch = "records:none";
            return res;
        }
        const Row* hi = &rows[0];
        const Row* lo = &rows[0];
        double sum = 0;
        for (const auto& x : rows) {
            if (x.a > hi->a) hi = &x;
            if (x.a < lo->a) lo = &x;
            sum += x.a;
        }
        const double avg = sum / double(rows.size());
        if (fan) {
            // Before the per-second readings an hour stands in the series as its median; its highest reading is the hourly
            // record's max, and the moment of it is in the readings an event kept that hour, when one did.
            double peak = hi->a;
            std::string when = "at " + when_text(hi->t);
            auto first = query(db_, "SELECT CAST(MIN(t) AS INTEGER) FROM samples WHERE fan IS NOT NULL", {});
            const int64_t detail = first.empty() || first[0].t == 0 ? to : first[0].t;
            auto top = query(db_, "SELECT hour_ts, max FROM samples_hourly WHERE metric = 'fan' AND hour_ts >= ? AND hour_ts < ? ORDER BY max DESC, hour_ts LIMIT 1",
                             {from, std::min(to, detail)});
            if (!top.empty() && top[0].a > peak) {
                peak = top[0].a;
                auto kept = query(db_, "SELECT CAST(MIN(t) AS INTEGER) FROM event_samples WHERE fan >= ? AND t >= ? AND t < ?",
                                  {int64_t(std::floor(peak)), std::max(from, top[0].t), std::min(to, top[0].t + 3600)});
                when = !kept.empty() && kept[0].t > 0 ? "at " + when_text(kept[0].t) : "in the hour from " + when_text(top[0].t);
            }
            res.body = "The fan " + span + " peaked at " + fmt("%.0f", peak) + "% " + when + " and averaged " + fmt("%.0f", avg) + "%.";
            // Where the per-second readings reach: when it was loudest (its stretches at the top level), what was busiest
            // then, and how much of the time it sat at its lowest.
            auto ps = query(db_, "SELECT CAST(t AS INTEGER), fan FROM samples WHERE t >= ?1 AND t < ?2 AND fan >= 0 ORDER BY t", {from, to});
            if (!ps.empty()) {
                double top = ps[0].a, low = ps[0].a;
                for (const auto& x : ps) top = std::max(top, x.a), low = std::min(low, x.a);
                long at_low = 0;
                for (const auto& x : ps) at_low += x.a <= low;
                const auto loud = spells(ps, top);
                std::vector<std::string> times;
                std::map<std::string, std::pair<double, std::string>> busy;  // name -> CPU seconds in the loud stretches, a command line
                for (const auto& sp : loud) {
                    if (times.size() < 3) times.push_back((sp.to - sp.from < 60 ? "at " : "from ") + run_span(sp.from, sp.to));
                    for (const auto& x : minute_cpu(db_, sp.from - sp.from % 60, sp.to + 1, "1", {}, "mt.name", 3)) {
                        busy[x.s1].first += x.a;
                        busy[x.s1].second = x.s2;
                    }
                }
                if (loud.size() > times.size()) times.push_back(unllm::number_word(long(loud.size() - times.size())) + " more times");
                std::vector<std::pair<double, std::string>> ranked;
                for (const auto& [name, v] : busy) ranked.push_back({v.first, shown_name(name, v.second)});
                std::sort(ranked.rbegin(), ranked.rend());
                std::vector<std::string> names;
                for (const auto& [secs, name] : ranked)
                    if (names.size() < 3 && secs >= 1) names.push_back(name);
                if (top > low)
                    res.body += " In the per-second readings" + (ps.front().t > from + 900 ? " (from " + when_text(ps.front().t) + ")" : std::string()) +
                                " it was loudest, at " + fmt("%.0f%%", top) + ", " + unllm::join_list(times) +
                                (names.empty() ? "" : ", when " + unllm::join_list(names) + (names.size() == 1 ? " was" : " were") + " the busiest") +
                                "; it sat at its lowest, " + fmt("%.0f%%", low) + ", " + fmt("%.0f%%", 100.0 * double(at_low) / double(ps.size())) +
                                " of the time.";
            }
        } else {
            // On battery: the time between readings while unplugged, and the moments the charger went out and in.
            auto ac = query(db_, "SELECT CAST(t AS INTEGER), on_ac FROM samples WHERE on_ac IS NOT NULL AND t >= ? AND t < ? ORDER BY t", {from, to});
            double unplugged = 0;
            for (std::size_t i = 1; i < ac.size(); ++i)
                if (ac[i - 1].a == 0) unplugged += std::min<double>(double(ac[i].t - ac[i - 1].t), 120.0);
            const auto ps = plugs(db_, from, to);
            // Readings that start well into the window: its start and its time on battery come from the plug moments.
            const bool whole = rows.front().t <= from + 900;
            if (!whole) {
                unplugged = 0;
                for (std::size_t i = 1; i < ps.size(); ++i)
                    if (!ps[i - 1].in && ps[i].in) unplugged += double(ps[i].t - ps[i - 1].t);
            }
            res.body = "The battery " + span + (whole ? " went from " + fmt("%.0f", rows.front().a) + "% to " + fmt("%.0f", rows.back().a) + "%," : ":") +
                       " lowest " + (least && least->level < lo->a ? fmt("%.0f", least->level) + "% at " + when_text(least->t) + from_where(*least)
                                                                   : fmt("%.0f", lo->a) + "% at " + when_text(lo->t)) + "." +
                       (unplugged > 60 ? " It ran on battery for about " + duration_text(unplugged) + (ps.empty() ? "." : ": " + plugs_text(ps) + ".")
                                       : ps.empty() ? " It stayed plugged in." : " It was " + plugs_text(ps) + ".");
            // The drain: the level lost between readings while unplugged, over that time, and the
            // programs that used the most CPU in those minutes.
            auto lvl = query(db_, "SELECT CAST(t AS INTEGER), battery_pct, on_ac FROM samples WHERE battery_pct IS NOT NULL AND on_ac IS NOT NULL "
                                  "AND t >= ? AND t < ? ORDER BY t", {from, to});
            double lost = 0, secs = 0;
            for (std::size_t i = 1; i < lvl.size(); ++i)
                if (lvl[i - 1].b == 0 && lvl[i].b == 0 && lvl[i].t - lvl[i - 1].t <= 120) {
                    lost += std::max(0.0, lvl[i - 1].a - lvl[i].a);
                    secs += double(lvl[i].t - lvl[i - 1].t);
                }
            if (secs >= 600 && lost > 0) {
                res.body += (whole ? std::string(" On battery") : " From " + when_text(rows.front().t) + ", on battery") + " it lost " +
                            fmt("%.0f", lost) + "% in " + duration_text(secs) + ", about " + fmt("%.0f", lost * 3600 / secs) + "% an hour";
                auto busy = query(db_, "SELECT 0, SUM(cpu_seconds), 0, name FROM minute_top WHERE minute IN "
                                       "(SELECT DISTINCT (CAST(t AS INTEGER) / 60) * 60 FROM samples WHERE on_ac = 0 AND t >= ? AND t < ?) "
                                       "GROUP BY name ORDER BY 2 DESC LIMIT 3", {from, to});
                std::vector<std::string> names;
                for (const auto& x : busy) names.push_back(x.s1 + " (" + duration_text(x.a) + " of CPU)");
                res.body += names.empty() ? "." : ". The busiest programs meanwhile: " + unllm::join_list(names) + ".";
            }
        }
        res.receipts.push_back(fmt("%.0f", double(rows.size())) + " readings from " + when_text(rows.front().t) + " to " + when_text(rows.back().t));
        res.branch = std::string("records:") + (fan ? "fan" : "battery");
        return res;
    }
    if (kind == "slow.above.value.window") {
        // "how long was the cpu above 80% today": the time at or over the level the question gives (a temperature when it
        // gives degrees), the recorder's busy level when it gives none; today when no window is named.
        const bool heat = quantity_threshold(r, "C") >= 0;
        double level = quantity_threshold(r, heat ? "C" : "%");
        if (level < 0) level = Triggers().cpu_total;
        int64_t f = from;
        if (!r.window.stated) {
            std::time_t tt = now;
            std::tm tm{};
            localtime_r(&tt, &tm);
            tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
            tm.tm_isdst = -1;
            f = int64_t(std::mktime(&tm));
        }
        res.body = above_text(db_, heat ? "temp" : "cpu", heat ? "temp" : "cpu", heat ? "The CPU temperature" : "CPU use",
                              [&](double v) { return fmt(heat ? "%.0f C" : "%.0f%%", v); }, level, f, to);
        if (res.body.empty()) res.body = "I have no CPU readings " + span + ".";
        res.branch = "records:above";
        return res;
    }
    if (kind == "network.value.window" || kind == "sharing.value.window") {
        // Bytes moved: each per-second reading's rates count for the second it covers, so a gap (asleep, off)
        // counts nothing. Before the readings kept, an hourly median counts for the part of its hour the readings
        // don't hold (its count of readings less those still kept).
        auto rows = query(db_, "SELECT CAST(t AS INTEGER), net_rx, net_tx FROM samples WHERE net_rx IS NOT NULL AND t >= ? AND t < ? ORDER BY t", {from, to});
        auto first = query(db_, "SELECT CAST(MIN(t) AS INTEGER) FROM samples WHERE net_rx IS NOT NULL", {});
        const int64_t detail = first.empty() || first[0].t == 0 ? to : first[0].t;
        auto hours = query(db_, "SELECT h.hour_ts, h.med * MAX(0, h.n - (SELECT COUNT(*) FROM samples s WHERE s.net_rx IS NOT NULL "
                                "AND s.t >= h.hour_ts AND s.t < h.hour_ts + 3600)) FROM samples_hourly h WHERE h.metric = 'net_rxtx' "
                                "AND h.hour_ts >= ? AND h.hour_ts < ?", {from, std::min(to, detail)});
        // Data sent (the sharing kind): what went out leads, with its fastest moment; where it went is not measured.
        const bool sent = kind == "sharing.value.window";
        double down = 0, up = 0, early = 0;
        const Row* hi = nullptr;
        std::vector<std::string> gaps;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            down += rows[i].a;
            up += rows[i].b;
            if (!hi || (sent ? rows[i].b > hi->b : rows[i].a + rows[i].b > hi->a + hi->b)) hi = &rows[i];
            if (i > 0 && rows[i].t - rows[i - 1].t > 600) gaps.push_back(span_text(rows[i - 1].t + 1, rows[i].t, " to "));
        }
        for (const auto& h : hours) early += h.a;
        if (rows.empty() && early <= 0) {
            res.body = "I have no network readings " + span + ".";
            res.branch = "records:none";
            return res;
        }
        res.body = std::string(r.window.stated ? "Between " + span_text(from, to, " and ") : "In the last day") + " about " +
                   (sent ? human_bytes(up) + " was sent and " + human_bytes(down) + " received" : human_bytes(down) + " was downloaded and " + human_bytes(up) + " uploaded");
        res.body += early > 0 ? ", " + human_bytes(down + up + early) + " in all with about " + human_bytes(early) + " from the hourly records before " +
                                    when_text(detail) + "."
                              : " (" + human_bytes(down + up) + " in all).";
        if (!rows.empty()) {
            res.body += " The readings run from " + span_text(rows.front().t, rows.back().t, " to ");
            if (!gaps.empty())
                res.body += ", with none from " + unllm::join_list(gaps) + ", so nothing is counted there";
            res.body += sent ? ". The fastest upload was " + human_bytes(hi->b) + "/s at " + when_text(hi->t) + "."
                             : ". The fastest was " + human_bytes(hi->a + hi->b) + "/s at " + when_text(hi->t) + ".";
        }
        if (sent) res.notes.push_back("I keep the machine's totals and which addresses each program connected to, not how much went to each program or place.");
        res.receipts.push_back(std::to_string(rows.size()) + " readings, one a second" + (hours.empty() ? "" : ", and " + std::to_string(hours.size()) + " hourly records"));
        res.branch = "records:network";
        return res;
    }
    if (kind == "network.which.window" || kind == "sharing.which.window" || kind == "network.which.now" || kind == "sharing.which.now") {
        // Who talked to the network: connections grouped by program. Now means still open in the last two minutes.
        const bool live = kind.find(".now") != std::string::npos;
        auto rows = live ? query(db_, "SELECT COUNT(*), COUNT(DISTINCT remote), 0, program FROM connections WHERE last_seen >= ? "
                                      "GROUP BY program ORDER BY 1 DESC LIMIT 5", {now - 120})
                         : query(db_, "SELECT COUNT(*), COUNT(DISTINCT remote), 0, program FROM connections WHERE last_seen >= ? AND first_seen < ? "
                                      "GROUP BY program ORDER BY 1 DESC LIMIT 5", {from, to});
        const bool sent = kind.rfind("sharing.", 0) == 0;
        // "What is causing the network slowdown": a why about now gets this side's facts in order: the Wi-Fi link and
        // its signal, whether anything here is loading the link, the hour's drops, then who is connected. Per-program
        // traffic isn't measured, so the cause is placed by elimination, and said so.
        if (live && !sent && r.wants == unllm::Wants::Cause) {
            if (!sampler_) sampler_ = std::make_unique<Sampler>();
            sampler_->sample();
            usleep(600 * 1000);
            const SnapshotPtr sn = sampler_->sample();
            double rx = 0, tx = 0;
            for (const auto& x : sn->nets)
                if (x.kind != "Loopback") rx += x.rx_bps, tx += x.tx_bps;
            const std::string dev = wifi_iface();
            const WifiLink w = dev.empty() ? WifiLink{} : wifi_link(dev);
            const bool wifi = !dev.empty() && !w.ssid.empty();
            std::string body;
            if (!dev.empty() && !wifi) body = "The Wi-Fi isn't connected to a network right now.";
            else if (wifi)
                body = "The Wi-Fi link to \"" + w.ssid + "\" is " + w.grade + " (" + w.signal + ")" + (w.rx.empty() ? "" : ", negotiated at " + w.rx + " Mbit/s") + ".";
            const double link_mbit = std::atof(w.rx.c_str()), load_mbit = 8 * (rx + tx) / 1e6;
            const bool idle = link_mbit > 0 && load_mbit < 0.05 * link_mbit;
            body += (body.empty() ? "" : " ") + std::string("The machine is moving ") + human_bytes(rx) + "/s down and " + human_bytes(tx) + "/s up right now" +
                    (idle ? ", nowhere near what the link carries, so nothing on this machine is loading it"
                          : link_mbit > 0 && load_mbit >= 0.5 * link_mbit ? ", most of what the link carries" : "") + ".";
            if (wifi) {
                const auto drops = wifi_drops(now - 3600, now);
                if (!drops.empty())
                    body += " The Wi-Fi dropped " + (drops.size() == 1 ? std::string("once") : std::to_string(drops.size()) + " times") + " in the last hour.";
                const std::string grade = w.grade;
                if (grade == "weak" || grade == "fair") body += " A " + grade + " signal is the likeliest cause on this side.";
                else if (!drops.empty()) body += " A link that keeps dropping is the likeliest cause on this side.";
                else if (idle) body += " A slowdown is then more likely the router or the internet side than this laptop.";
            }
            if (!rows.empty()) {
                std::vector<std::string> named;
                for (const auto& x : rows) named.push_back(x.s1 + " (" + std::to_string(x.t) + (x.t == 1 ? " connection" : " connections") + ")");
                body += " Connected right now: " + unllm::join_list(named) + ".";
            }
            res.body = body;
            res.notes.push_back("How much each program sends or receives isn't measured, only who connects where.");
            res.branch = "records:network-cause";
            return res;
        }
        // How much each program moved is not recorded; the machine's own figures are: its rates now, or what its per-second
        // readings add up to over the window (and, for data sent, the last day's upload).
        auto moved = [&](int64_t a, int64_t b) {
            auto m = query(db_, "SELECT COUNT(*), SUM(net_rx), SUM(net_tx) FROM samples WHERE net_rx IS NOT NULL AND t >= ? AND t < ?", {a, b});
            return m.empty() || m[0].t == 0 ? std::pair<double, double>{-1, -1} : std::pair<double, double>{m[0].a, m[0].b};
        };
        std::string machine;
        if (live) {
            if (!sampler_) sampler_ = std::make_unique<Sampler>();
            sampler_->sample();
            usleep(600 * 1000);  // rates need two looks
            const SnapshotPtr sn = sampler_->sample();
            double rx = 0, tx = 0;
            for (const auto& x : sn->nets)
                if (x.kind != "Loopback") rx += x.rx_bps, tx += x.tx_bps;
            machine = sent ? "the machine as a whole is sending " + human_bytes(tx) + "/s right now"
                           : "the machine as a whole is moving " + human_bytes(rx) + "/s down and " + human_bytes(tx) + "/s up right now";
            if (const auto day = sent ? moved(now - 86400, now) : std::pair<double, double>{-1, -1}; day.second >= 0)
                machine += " and sent " + human_bytes(day.second) + " in the last day";
        } else if (const auto [d, u] = moved(from, to); u >= 0) {
            machine = sent ? "the machine as a whole sent " + human_bytes(u) + " then"
                           : "the machine as a whole downloaded " + human_bytes(d) + " and uploaded " + human_bytes(u) + " then";
        }
        const std::string measured = std::string("How much each program ") + (sent ? "sends" : "sends or receives") +
                                     " isn't measured, only who connects where" + (machine.empty() ? "." : "; " + machine + ".");
        if (rows.empty()) {
            res.body = std::string("No program had a network connection ") + (live ? "open right now." : span + ".");
        } else {
            std::vector<std::string> named;
            for (const auto& x : rows)
                named.push_back(x.s1 + " (" + std::to_string(x.t) + (x.t == 1 ? " connection" : " connections") + " to " +
                                std::to_string(int64_t(x.a)) + (int64_t(x.a) == 1 ? " place)" : " places)"));
            res.body = std::string(live ? "Connected right now: " : "The programs that used the network " + span + ": ") +
                       unllm::join_list(named) + ".";
        }
        res.notes.push_back(measured);
        res.branch = "records:connections";
        return res;
    }
    if (kind == "crashes.list.window" || kind == "crashes.when.window" || kind == "crashes.which.window" || kind == "crashes.cause.window") {
        // Core dumps, segfaults, out-of-memory kills and failed units. "When did X last crash" with no time named
        // reads the whole history.
        const bool all = kind == "crashes.when.window" && !r.window.stated;
        const int64_t f = all ? 0 : from, t = all ? now : to;
        const std::string where = all ? "in the records" : span;
        std::vector<Mishap> found, crashed, failed;
        for (const auto& m : mishaps(db_, f, t))
            if (about(m)) {
                found.push_back(m);
                (m.unit.empty() ? crashed : failed).push_back(m);
            }
        const std::string of = program.empty() ? "" : " of " + program;
        for (auto it = found.rbegin(); it != found.rend() && res.receipts.size() < 6; ++it) res.receipts.push_back(when_text(it->t) + "  " + it->text);
        res.branch = "records:crashes";
        // The kernel's note on a crash at t: "kernel: movie[2409161]: segfault at 14 ip ... in libprotobuf-lite.so.36.1.0[ce4ef,...]"
        // read as the fault and the library it happened in.
        auto fault_note = [&](int64_t t) -> std::string {
            auto detail = query(db_, "SELECT t, 0, 0, detail FROM log WHERE category = 'problems' AND t >= ?1 AND t <= ?2 AND detail != '' ORDER BY t DESC LIMIT 1",
                                {t - 2, t + 2});
            if (detail.empty()) return "";
            const std::string& d = detail[0].s1;
            const std::string fault = d.find("segfault") != std::string::npos ? "a segfault" : d.find("general protection") != std::string::npos ? "a protection fault" : "";
            if (fault.empty()) return "";
            std::string lib;
            if (const auto at = d.find(" in "); at != std::string::npos) {
                lib = d.substr(at + 4, d.find_first_of("[ ", at + 4) - at - 4);
                if (lib.find('.') == std::string::npos) lib.clear();
            }
            return " (" + fault + (lib.empty() ? "" : " in " + lib) + ")";
        };
        // A named program with nothing in the window: its last crash on record, with the kernel's note on it, else that it
        // has never crashed on record and when it last ran, so a why gets what there is.
        auto history = [&]() -> std::string {
            if (program.empty() || all) return "";
            std::vector<Mishap> ever;
            for (const auto& m : mishaps(db_, 0, from))
                if (about(m) && m.unit.empty()) ever.push_back(m);
            if (!ever.empty()) {
                const Mishap& m = ever.back();
                return " Its last crash on record was " + moment_text(m.t) + fault_note(m.t) + aftermath(m) + ".";
            }
            auto first = query(db_, "SELECT CAST(MIN(t) AS INTEGER) FROM log", {});
            return " None is on record at all" + (first.empty() || first[0].t <= 0 ? std::string() : ", back to " + moment_text(first[0].t)) + "." +
                   last_run_text(db_, program, now);
        };
        if (found.empty()) {
            if (polar) settle(res, r, false);
            res.body = "No crash or failed unit" + of + " was recorded " + where + "." + history();
            if (!program.empty()) {
                card_.subject = program;  // "why" and "when" after it still mean this program
                card_.subjects_named = {program};
            }
            return res;
        }
        card_.moment = found.back().t;
        card_.subject = program.empty() ? found.back().who : program;
        // A yes is for a crash; a named program whose unit only failed gets the plain account.
        const bool yes = polar && (!crashed.empty() || program.empty());
        if (kind == "crashes.which.window") {
            res.body = "By count, " + where + ": " + grouped(found) + ".";
            // Again and again: what crashed or failed on the most separate days (two or more), which a count led by
            // one day's burst hides; the first time on each of those days.
            std::map<std::string, std::vector<int64_t>> days;
            for (const auto& m : found) {
                auto& d = days[m.unit.empty() ? m.who : unit_kind(m.unit)];
                if (d.empty() || local_date(d.back()) != local_date(m.t)) d.push_back(m.t);
            }
            std::size_t most = 0;
            for (const auto& [who, d] : days) most = std::max(most, d.size());
            if (most >= 2) {
                std::vector<std::string> again;
                for (const auto& [who, d] : days)
                    if (d.size() == most) {
                        std::vector<std::string> at;
                        for (int64_t x : d) at.push_back(when_text(x));
                        again.push_back(who + " (" + unllm::join_list(at) + ")");
                    }
                res.body += " Again and again, on " + unllm::number_word(int64_t(most)) + " separate days: " + unllm::join_list(again) + ".";
            }
            return res;
        }
        if (kind == "crashes.when.window") {
            const Mishap& m = found.back();
            if (yes) settle(res, r, true);
            res.body = "The latest crash record" + of + " " + where + " is from " + moment_text(m.t) +
                       ": " + m.text + aftermath(m) + ".";
            if (count_of(found) > 1) res.body += " It is one of " + std::to_string(count_of(found)) + " " + where + ".";
            return res;
        }
        // The list, oldest first: the crashes, then the failed units, each with its reason (a named program's own
        // crashes need only their times); more than five, by program or unit.
        auto said = [&](const std::vector<Mishap>& v, bool times_only) {
            if (v.size() > 5 && !times_only) return grouped(v);
            std::vector<std::string> out;
            const std::size_t skip = v.size() > 5 ? v.size() - 5 : 0;  // a named program's latest five
            if (skip) out.push_back(std::to_string(skip) + " earlier");
            // A named program's own crashes carry the kernel's note each ("Sun 14:34 (a segfault in libQt6Core.so.6)").
            for (std::size_t i = skip; i < v.size(); ++i)
                out.push_back((times_only ? "" : v[i].text + " at ") + when_text(v[i].t) + (times_only && v[i].unit.empty() ? fault_note(v[i].t) : "") + aftermath(v[i]));
            return unllm::join_list(out);
        };
        std::string text;
        if (!crashed.empty()) {
            const long n = count_of(crashed);
            text = program.empty() ? capital(unllm::number_word(n)) + " " + unllm::plural_noun("crash", n) + (n == 1 ? " was" : " were") +
                                         " recorded " + where + ": " + said(crashed, false) + "."
                                   : capital(program) + " crashed " + times(n) + " " + where + ": " + said(crashed, true) + ".";
        } else if (!program.empty()) {
            text = "No crash of " + program + " itself was recorded " + where + "." + history();
        }
        if (!failed.empty()) {
            const long n = count_of(failed);
            text += (text.empty() ? "" : " ") +
                    (program.empty() ? capital(unllm::number_word(n)) + " unit " + unllm::plural_noun("failure", n) + (n == 1 ? " was" : " were") +
                                           " recorded" + (crashed.empty() ? " " + where : std::string())
                                     : "Its unit failed " + times(n)) +
                    ": " + said(failed, false) + ".";
        }
        if (yes) settle(res, r, true);
        res.body = text;
        // The same program's records in the week before, so "again" has its history.
        if (!program.empty() && !all && to - from < 7 * 86400) {
            std::vector<std::string> before;
            for (const auto& m : mishaps(db_, to - 7 * 86400, from))
                if (about(m)) before.push_back(when_text(m.t));
            if (!before.empty()) res.body += " Before that, in the week to " + when_text(to) + ": " + unllm::join_list(before) + ".";
        }
        return res;
    }
    if (kind == "startup.list.window") {
        // The startup watcher's rows ("<what> <verb>: <path>", as the recorder writes them), each file told by its life
        // in the rows' own times: a file the window added and removed within a minute was temporary; a file removed that
        // was never seen there, in the same seconds as another file of its folder was added, was the temporary copy
        // renamed onto that one; a file added again while it was there was rewritten (one the window added) or replaced
        // in place (one there before). A question that uses one of the rows' own verbs ("what got added") gets those.
        struct Change {
            int64_t t = 0;
            std::string what, verb, path, detail;
        };
        std::vector<Change> rows;
        for (const auto& x : query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title, detail FROM log WHERE category = 'startup' AND t >= ? AND t < ? "
                                        "ORDER BY t", {from, to})) {
            const auto colon = x.s1.find(": ");
            const auto sp = colon == std::string::npos || colon == 0 ? std::string::npos : x.s1.rfind(' ', colon - 1);
            if (sp == std::string::npos) rows.push_back({x.t, x.s1, "", "", x.s2});
            else rows.push_back({x.t, x.s1.substr(0, sp), x.s1.substr(sp + 1, colon - sp - 1), x.s1.substr(colon + 2), x.s2});
        }
        std::string verb;
        for (const auto& t : unllm::tokenize(card_.question))
            for (const auto& c : rows)
                if (!c.verb.empty() && unllm::normalize_word(c.verb) == t.norm) verb = c.verb;
        struct File {
            Change first;
            std::vector<int64_t> again;  // rewritten (a new file) or replaced in place (one there before)
            bool gone = false;
            std::string copy;            // a file added beside it at the same moment under its name and more
        };
        std::vector<File> added, replaced;
        std::vector<Change> removed, other;
        std::map<std::string, bool> there;  // each path: there or not, as the rows go
        long temporary = 0;
        auto folder = [](const std::string& p) { return p.substr(0, p.rfind('/') + 1); };
        for (const auto& c : rows) {
            if (c.path.empty()) {
                other.push_back(c);
                continue;
            }
            if (!there.count(c.path)) {
                auto before = query(db_, "SELECT t, 0, 0, title FROM log WHERE category = 'startup' AND t < ?1 AND "
                                         "substr(title, -length(?2) - 2) = ': ' || ?2 ORDER BY t DESC LIMIT 1", {from}, {c.path});
                there[c.path] = !before.empty() && before[0].s1.find(" removed: ") == std::string::npos;
            }
            auto mine = std::find_if(added.begin(), added.end(), [&](const File& f) { return f.first.path == c.path && !f.gone; });
            if (c.verb == "removed") {
                const bool unseen = !there[c.path] && mine == added.end();
                const bool onto = std::any_of(rows.begin(), rows.end(), [&](const Change& o) {
                    return o.verb == "added" && o.path != c.path && folder(o.path) == folder(c.path) && std::llabs(o.t - c.t) <= 2;
                });
                if (mine != added.end() && c.t - mine->first.t <= 60) added.erase(mine), ++temporary;
                else if (unseen && onto) ++temporary;
                else {
                    if (mine != added.end()) mine->gone = true;
                    removed.push_back(c);
                }
                there[c.path] = false;
                continue;
            }
            if (mine != added.end()) {
                mine->again.push_back(c.t);
            } else if (there[c.path] || c.verb != "added") {
                auto old = std::find_if(replaced.begin(), replaced.end(), [&](const File& f) { return f.first.path == c.path; });
                if (old == replaced.end()) replaced.push_back({c, {c.t}});
                else old->again.push_back(c.t);
            } else {
                added.push_back({c, {}});
            }
            there[c.path] = true;
        }
        // A copy made beside a new file at the same moment ("x.service.bak-2026-10-22" beside "x.service") is told with it.
        {
            std::vector<bool> copy(added.size(), false);
            for (std::size_t i = 0; i < added.size(); ++i)
                for (std::size_t j = 0; j < added.size(); ++j) {
                    const Change &a = added[i].first, &b = added[j].first;
                    if (j != i && !copy[i] && !copy[j] && added[i].copy.empty() && folder(a.path) == folder(b.path) &&
                        b.path.size() > a.path.size() && b.path.rfind(a.path, 0) == 0 && std::llabs(a.t - b.t) <= 2) {
                        added[i].copy = b.path.substr(b.path.rfind('/') + 1);
                        copy[j] = true;
                    }
                }
            std::vector<File> kept;
            for (std::size_t i = 0; i < added.size(); ++i)
                if (!copy[i]) kept.push_back(added[i]);
            added = kept;
        }
        auto lower = [](std::string s) {
            if (!s.empty()) s[0] = char(std::tolower(static_cast<unsigned char>(s[0])));
            return s;
        };
        // What systemd makes of a unit file now: its state, when this very file is the unit's.
        auto unit_state = [](const std::string& path) {
            const std::string name = path.substr(path.rfind('/') + 1);
            if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@._-:\\") != std::string::npos)
                return std::string();
            for (const std::string scope : {"--user ", ""}) {
                const std::string out = run_out("systemctl " + scope + "show -p FragmentPath -p UnitFileState -- " + name + " 2>/dev/null");
                const auto fp = out.find("FragmentPath="), st = out.find("UnitFileState=");
                if (fp == std::string::npos || st == std::string::npos || out.substr(fp + 13, out.find('\n', fp) - fp - 13) != path) continue;
                return trim(out.substr(st + 14, out.find('\n', st) - st - 14));
            }
            return std::string();
        };
        auto new_text = [&](const File& f) {
            std::string s = f.first.path + " (" + lower(f.first.what) + ", added " + when_text(f.first.t);
            if (!f.again.empty())
                s += f.again.size() == 1 ? ", rewritten at " + when_text(f.again[0]) : ", rewritten " + times(long(f.again.size())) + ", last at " + when_text(f.again.back());
            if (f.gone) s += ", since removed";
            if (f.first.detail.rfind("Link to ", 0) == 0) s += ", a link to " + trim(f.first.detail.substr(8, f.first.detail.find('\n') - 8));
            if (const std::string state = f.gone ? "" : unit_state(f.first.path); !state.empty()) s += "; systemd has it " + state;
            if (!f.copy.empty()) s += "; with a copy of it made beside it at the same moment, " + f.copy;
            return s + ")";
        };
        std::vector<std::string> said;
        if (rows.empty()) said.push_back("No startup changes were recorded " + span + ".");
        else if (verb != "removed") {
            std::vector<std::string> each;
            for (const auto& f : added) each.push_back(new_text(f));
            said.push_back(each.empty() ? "Nothing new was added " + span + "."
                                        : capital(unllm::number_word(long(each.size()))) + " new " + (each.size() == 1 ? "file was" : "files were") +
                                              " added " + span + ": " + unllm::join_list(each) + ".");
            std::vector<std::string> again;
            for (const auto& f : replaced)
                again.push_back(f.first.path + " " + times(long(f.again.size())) + " (" +
                                (f.again.size() == 1 ? "at " + when_text(f.again[0]) : "from " + when_text(f.again[0]) + " to " + when_text(f.again.back())) + ")");
            if (!again.empty()) said.push_back("Not new, replaced in place: " + unllm::join_list(again) + ".");
        }
        if (!rows.empty() && verb != "added") {
            std::vector<std::string> each;
            for (const auto& c : removed) each.push_back(c.path + " (" + lower(c.what) + ", at " + when_text(c.t) + ")");
            if (verb == "removed" || !each.empty())
                said.push_back(each.empty() ? "Nothing was removed " + span + "." : "Removed: " + unllm::join_list(each) + ".");
        }
        if (verb.empty() && !other.empty()) {
            std::vector<std::string> each;
            for (const auto& c : other) each.push_back(c.what + " (" + when_text(c.t) + ")");
            said.push_back("Also: " + unllm::join_list(each) + ".");
        }
        if (temporary > 0)
            said.push_back("Left out: " + unllm::number_word(temporary) + " temporary " + (temporary == 1 ? "file" : "files") +
                           " that came and went while other files were written.");
        for (const auto& s : said) res.body += (res.body.empty() ? "" : " ") + s;
        if (polar) {
            const bool any = verb == "added" ? !added.empty() : verb == "removed" ? !removed.empty()
                                                              : !(added.empty() && replaced.empty() && removed.empty() && other.empty());
            settle(res, r, any);
        }
        for (std::size_t i = rows.size() > 6 ? rows.size() - 6 : 0; i < rows.size(); ++i)
            res.receipts.push_back(when_text(rows[i].t) + "  " + rows[i].what + (rows[i].verb.empty() ? "" : " " + rows[i].verb + ": " + rows[i].path));
        res.branch = "records:startup-changes";
        return res;
    }
    if (kind == "startup.which.window" || (kind == "startup.often.window" && program.empty())) {
        // The units systemd reported failed in the window, by unit, the most first, by their real names (a row's
        // detail holds the whole name).
        std::vector<Mishap> failed;
        for (const auto& m : mishaps(db_, from, to))
            if (!m.unit.empty() && about(m)) failed.push_back(m);
        for (std::size_t i = failed.size() > 6 ? failed.size() - 6 : 0; i < failed.size(); ++i)
            res.receipts.push_back(when_text(failed[i].t) + "  " + failed[i].text);
        const long total = count_of(failed);
        res.body = total == 0 ? "No unit failed " + span + "."
                              : capital(unllm::number_word(total)) + " unit " + unllm::plural_noun("failure", total) + (total == 1 ? " was" : " were") +
                                    " recorded " + span + ": " + grouped(failed) + ".";
        // With no time named, the units in a failed state now as well.
        if (!r.window.stated) {
            std::vector<std::string> failing;
            for (const std::string scope : {"", "--user "}) {
                std::istringstream in(run_out("systemctl " + scope + "list-units --failed --plain --no-legend --no-pager 2>/dev/null"));
                for (std::string line; std::getline(in, line);)
                    if (const std::string u = trim(line.substr(0, line.find(' '))); !u.empty()) failing.push_back(unit_name(u));
            }
            res.body += failing.empty() ? " No unit is in a failed state now." : " In a failed state now: " + unllm::join_list(failing) + ".";
        }
        res.branch = "records:units-failed";
        return res;
    }
    if (kind == "startup.often.window" || (kind == "startup.often.reboot" && !program.empty() && program != r.condition)) {
        // A program's starts in the window, each process once (one already running when the window began is no
        // start there), and the crashes and failures of it and its unit. A restart count naming a program is about
        // that program, not the machine.
        auto starts = query(db_, "SELECT CAST(MIN(started) AS INTEGER), 0, 0, name FROM procs WHERE started >= ? AND started < ? AND "
                                 "is_program(?, name, exe, cmdline) GROUP BY pid ORDER BY 1",
                            {from, to}, {program});
        std::vector<std::string> bad;
        for (const auto& m : mishaps(db_, from, to))
            if (about(m)) bad.push_back(m.text + " at " + when_text(m.t));
        const long n = long(starts.size());
        res.body = capital(n == 0 ? program + " didn't start " + span + "."
                           : n == 1 ? program + " started once " + span + ", at " + when_text(starts[0].t) + "."
                                    : program + " started " + times(n) + " " + span + ", the first at " + when_text(starts.front().t) +
                                          " and the last at " + when_text(starts.back().t) + ".");
        res.body += bad.empty() ? " No crash or failure of it was recorded there." : " Crashes and failures there: " + unllm::join_list(bad) + ".";
        for (auto it = starts.rbegin(); it != starts.rend() && res.receipts.size() < 6; ++it) res.receipts.push_back(when_text(it->t) + "  " + it->s1 + " started");
        res.branch = "records:restarts";
        return res;
    }
    if (kind == "battery.sleep.list.window" || kind == "battery.sleep.when.window" || kind == "battery.sleep.often.window") {
        // The sleeps that overlap the window ("when" with no time named: all the records), from the session log.
        const bool when = kind == "battery.sleep.when.window", often = kind == "battery.sleep.often.window";
        const bool all = when && !r.window.stated;
        const int64_t f = all ? 0 : from, t = all ? now : to;
        const std::string where = all ? "in the records" : span;
        std::vector<Sleep> in;
        for (const auto& s : sleeps(db_, now))
            if (s.went < t && (s.woke == 0 || s.woke > f)) in.push_back(s);
        auto one = [&](const Sleep& s) {
            return "from " + when_text(s.went) + (s.lid ? " (lid closed)" : "") + " to " +
                   (s.woke ? when_text(s.woke) + ", " + duration_text(s.woke - s.went) : std::string("now, still asleep"));
        };
        for (const auto& s : in)
            if (res.receipts.size() < 6) res.receipts.push_back(when_text(s.went) + "  went to sleep" + (s.woke ? ", woke " + when_text(s.woke) : ""));
        res.branch = "records:sleep";
        if (in.empty()) {
            if (polar) settle(res, r, false);
            res.body = "It stayed awake " + where + ": no sleep was recorded.";
            return res;
        }
        // "When" with no time named is the latest; a day or a span named gets every sleep in it, below.
        if (when && (all || in.size() == 1)) {
            const Sleep& s = in.back();
            if (polar) settle(res, r, true);
            res.body = "It went to sleep at " + when_text(s.went) + (s.lid ? " (lid closed)" : "") +
                       (s.woke ? " and woke at " + when_text(s.woke) + ", after " + duration_text(s.woke - s.went) : " and hasn't woken since") + ".";
            card_.moment = s.woke ? s.woke : s.went;
            return res;
        }
        const long n = long(in.size());
        if (often) {
            int64_t asleep = 0;
            const Sleep* longest = &in[0];
            auto end = [&](const Sleep& s) { return s.woke ? s.woke : now; };
            for (const auto& s : in) {
                asleep += std::min(end(s), t) - std::max(s.went, f);
                if (end(s) - s.went > end(*longest) - longest->went) longest = &s;
            }
            res.body = "It slept " + times(n) + " " + where + ", " + duration_text(asleep) + " in all within that time; the longest " +
                       one(*longest) + ".";
            return res;
        }
        std::vector<std::string> said;
        for (const auto& s : in) said.push_back(one(s));
        if (polar) settle(res, r, true);
        res.body = "It slept " + times(n) + " " + where + ": " + unllm::join_list(said) + ".";
        return res;
    }
    if (kind.rfind("battery.charger.", 0) == 0 || kind.rfind("battery.lid.", 0) == 0 || kind.rfind("startup.login.", 0) == 0) {
        // One thing the session log holds, in its two sides as the recorder titles them: the charger ("Plugged into power
        // at 56% battery", "Unplugged from power ..."), the lid, logging in and out (logind's own lines; the login
        // screen's sessions and the user's service manager are no person logging in: only a session of class 'user' is,
        // and its end). The side asked is the one whose title is nearer the question by meaning ("shut the lid" is "Lid
        // closed"). A "when" with no time named, or a "last" (the reading's all_history), is the latest in all the
        // records; otherwise every one in the window.
        struct Side {
            const char* title;  // the recorder's title, up to its particulars
            const char* verb;   // as the answer says it
            const char* base;   // "you didn't log in"
        };
        const bool charger = kind.rfind("battery.charger.", 0) == 0, lid = kind.rfind("battery.lid.", 0) == 0, login = !charger && !lid;
        const Side sides[2] = {charger ? Side{"Plugged into power", "plugged in", ""} : lid ? Side{"Lid closed", "closed", ""} : Side{"Logged in", "logged in", "log in"},
                               charger ? Side{"Unplugged from power", "unplugged", ""} : lid ? Side{"Lid opened", "opened", ""} : Side{"Logged out", "logged out", "log out"}};
        int side = 0;
        if (const unllm::Meaning* model = meaning(); model && model->can_embed()) {
            const auto q = model->embed(card_.question, false);
            float best = -2;
            for (int i = 0; i < 2; ++i) {
                const auto v = model->embed(sides[i].title, false);
                float d = 0;
                for (std::size_t k = 0; k < q.size() && k < v.size(); ++k) d += q[k] * v[k];
                if (d > best) best = d, side = i;
            }
        }
        struct Mark {
            int64_t t = 0;
            int side = 0;
            double level = -1;  // the battery's, for the charger
            std::string session;
        };
        std::vector<Mark> marks;
        std::map<std::string, bool> person;  // a session id: opened by a person's log-in (the latest log-in with that id)
        for (const auto& x : query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title, detail FROM log WHERE category = 'session' AND t <= ? ORDER BY t", {now})) {
            int which = -1;
            for (int i = 0; i < 2; ++i)
                if (x.s1.rfind(sides[i].title, 0) == 0) which = i;
            if (which < 0) continue;
            Mark m{x.t, which, -1, ""};
            if (const auto a = x.s1.find(" at "); charger && a != std::string::npos) m.level = std::atof(x.s1.c_str() + a + 4);
            if (login) {
                // "New session '5' of user 'u' with class 'user' ..."; "Removed session 5."
                auto between = [](const std::string& s, const std::string& open, const std::string& close) {
                    const auto a = s.find(open);
                    if (a == std::string::npos) return std::string();
                    const auto b = s.find(close, a + open.size());
                    return s.substr(a + open.size(), b == std::string::npos ? std::string::npos : b - a - open.size());
                };
                if (which == 0) {
                    m.session = between(x.s2, "New session '", "'");
                    person[m.session] = between(x.s2, "with class '", "'") == "user";
                    if (!person[m.session]) continue;
                } else {
                    m.session = between(x.s2, "Removed session ", ".");
                    if (!person[m.session]) continue;
                    person[m.session] = false;
                }
            }
            marks.push_back(m);
        }
        // What changed it back: the next mark, when it is the other side (for a log-in, the next mark of its session: a
        // new log-in under the same id means the session ended unrecorded, as when the machine lost power).
        auto back = [&](std::size_t i) -> const Mark* {
            for (std::size_t j = i + 1; j < marks.size(); ++j)
                if (!login || marks[j].session == marks[i].session) return marks[j].side != marks[i].side ? &marks[j] : nullptr;
            return nullptr;
        };
        auto level = [](const Mark& m) { return m.level >= 0 ? fmt(" (%.0f%%)", m.level) : std::string(); };
        const Side& s = sides[side];
        const Side& o = sides[1 - side];
        const std::string who = charger ? "The charger was" : lid ? "The lid was" : "You";
        res.branch = charger ? "records:charger" : lid ? "records:lid" : "records:logins";
        const bool latest = (r.window.all_history || (kind.find(".when.") != std::string::npos && !r.window.stated)) && r.form != Form::Count;
        std::vector<std::size_t> in;
        for (std::size_t i = 0; i < marks.size(); ++i)
            if (marks[i].side == side && (latest || (marks[i].t >= from && marks[i].t < to))) in.push_back(i);
        for (auto it = in.rbegin(); it != in.rend() && res.receipts.size() < 6; ++it)
            res.receipts.push_back(when_text(marks[*it].t) + "  " + s.verb + level(marks[*it]));
        if (in.empty()) {
            const std::string where = latest ? "in the records" : span;
            if (polar) settle(res, r, false);
            res.body = capital((login ? "you didn't " + std::string(s.base) : std::string(charger ? "the charger wasn't " : "the lid wasn't ") + s.verb) + " " +
                               where + ".");
            // No log-in in the window because the session never ended: since when it has been open.
            if (login)
                for (std::size_t i = marks.size(); i-- > 0;)
                    if (marks[i].side == side && marks[i].t < from) {
                        if (!back(i)) res.body += " You have been logged in since " + moment_text(marks[i].t) + ".";
                        break;
                    }
            return res;
        }
        if (latest) {
            const Mark& m = marks[in.back()];
            const Mark* after = back(in.back());
            if (polar) settle(res, r, true);
            res.body = who + " last " + s.verb + " on " + moment_text(m.t) + level(m) +
                       (after ? "; " + std::string(login ? "you " : "it was ") + o.verb + " at " + when_text(after->t) + level(*after)
                              : login ? "; you haven't " + std::string(o.verb) + " since" : "; it hasn't been " + std::string(o.verb) + " since") + ".";
            // Just before it, in time order: the other side's mark, what else the session log holds in the minute before,
            // and a start of the machine in the five minutes before.
            std::vector<std::pair<int64_t, std::string>> before;
            for (std::size_t j = in.back(); j-- > 0;)
                if (marks[j].side != side) {
                    before.emplace_back(marks[j].t, std::string(o.verb) + " at " + when_text(marks[j].t) + level(marks[j]));
                    break;
                }
            for (const auto& x : query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'session' AND t >= ? AND t < ? ORDER BY t",
                                       {m.t - 60, m.t})) {
                if (x.s1.rfind(sides[0].title, 0) == 0 || x.s1.rfind(sides[1].title, 0) == 0) continue;
                std::string head = x.s1.substr(0, std::min(x.s1.find(':'), x.s1.find(" (")));
                if (!head.empty()) head[0] = char(std::tolower(static_cast<unsigned char>(head[0])));
                before.emplace_back(x.t, head + " at " + when_text(x.t));
            }
            for (int64_t b : boot_starts(db_, now))
                if (b <= m.t && m.t - b <= 300) before.emplace_back(b, "the machine started at " + when_text(b));
            std::stable_sort(before.begin(), before.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            std::vector<std::string> told;
            for (const auto& [t, text] : before) told.push_back(text);
            if (!told.empty()) res.body += " Just before: " + unllm::join_list(told) + ".";
            std::vector<std::string> earlier;
            for (std::size_t k = in.size() - 1; k-- > 0 && earlier.size() < 3;) earlier.push_back(when_text(marks[in[k]].t) + level(marks[in[k]]));
            if (!earlier.empty()) res.body += " Before that: " + unllm::join_list(earlier) + ".";
            card_.moment = m.t;
            return res;
        }
        std::vector<std::string> said;
        const std::size_t skip = in.size() > 8 ? in.size() - 8 : 0;
        for (std::size_t k = skip; k < in.size(); ++k) {
            const Mark& m = marks[in[k]];
            const Mark* after = back(in[k]);
            std::string item = when_text(m.t) + level(m);
            const int64_t gap = after ? after->t - m.t : 0;
            if (after) item += gap < 60 ? " (" + std::string(o.verb) + " again " + std::to_string(gap) + (gap == 1 ? " second" : " seconds") + " later)"
                                        : " (" + std::string(o.verb) + " at " + when_text(after->t) + level(*after) + ")";
            said.push_back(item);
        }
        if (skip) said.insert(said.begin(), std::to_string(skip) + " earlier");
        if (polar) settle(res, r, true);
        res.body = who + " " + s.verb + " " + times(long(in.size())) + " " + span + ": " + unllm::join_list(said) + ".";
        return res;
    }
    if (kind == "trust.admin.list.window") {
        // Commands run as administrator, as the logbook keeps them (classify_journal): sudo's and pkexec's, polkit's
        // approvals. sudo's own line ("user : [why ;] TTY=... ; PWD=... ; USER=root ; COMMAND=...") holds the folder it
        // ran in and, in a field that is not KEY=value, why it was refused ("a password is required").
        auto rows = query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title, detail FROM log WHERE category = 'admin' AND t >= ? AND t < ? ORDER BY t", {from, to});
        res.branch = "records:admin";
        if (rows.empty()) {
            if (polar) settle(res, r, false);
            res.body = "Nothing was run as administrator " + span + ".";
            return res;
        }
        std::vector<std::string> said;
        long refused = 0;
        for (const auto& x : rows) {
            const auto colon = x.s1.find(": ");
            std::string cmd = colon == std::string::npos ? x.s1 : x.s1.substr(colon + 2);
            std::string why, where;
            const auto sep = x.s2.find(" : ");
            if (const auto c = x.s2.find("COMMAND="); sep != std::string::npos && c != std::string::npos) {
                cmd = x.s2.substr(c + 8);
                const std::string user = trim(x.s2.substr(x.s2.rfind(' ', sep - 1) + 1, sep - x.s2.rfind(' ', sep - 1) - 1));
                std::istringstream fields(x.s2.substr(sep + 3, c - sep - 3));
                for (std::string f; std::getline(fields, f, ';');) {
                    f = trim(f);
                    const auto eq = f.find('=');
                    const bool key = eq != std::string::npos && eq > 0 &&
                                     std::all_of(f.begin(), f.begin() + std::ptrdiff_t(eq), [](char ch) { return std::isupper(static_cast<unsigned char>(ch)); });
                    if (!key && !f.empty()) why = f;
                    else if (key && f.compare(0, eq, "PWD") == 0) where = f.substr(eq + 1);
                }
                const std::string home = "/home/" + user;
                if (where == home) where.clear();
                else if (where.rfind(home + "/", 0) == 0) where = "~" + where.substr(home.size());
            }
            // The program by its name, not its folder; journald's escaped line breaks as breaks between commands.
            for (std::size_t at; (at = cmd.find("#012")) != std::string::npos;) cmd.replace(at, 4, " ");
            if (cmd.rfind('/', cmd.find(' ')) != std::string::npos && cmd[0] == '/') cmd = cmd.substr(cmd.rfind('/', cmd.find(' ')) + 1);
            if (cmd.size() > 120) cmd = trim(cmd.substr(0, 117)) + "...";
            refused += !why.empty();
            said.push_back(when_text(x.t) + " " + cmd + (why.empty() ? "" : ", refused (" + why + ")") + (where.empty() ? "" : ", in " + where));
            if (res.receipts.size() < 6) res.receipts.push_back(when_text(x.t) + "  " + x.s1.substr(0, 100));
        }
        const std::size_t skip = said.size() > 10 ? said.size() - 10 : 0;
        std::string list;
        for (std::size_t i = skip; i < said.size(); ++i) list += (list.empty() ? "" : "; ") + said[i];
        const long n = long(rows.size());
        if (polar) settle(res, r, true);
        res.body = capital(unllm::number_word(n)) + (n == 1 ? " command was" : " commands were") +
                   " run as administrator " + span + (refused ? " (" + unllm::number_word(refused) + " refused)" : "") + ": " +
                   (skip ? std::to_string(skip) + " earlier, then " : "") + list + ".";
        return res;
    }
    if (kind == "trust.list.window") {
        // Programs run for the first time, as the recorder logs them (LogWatch::first_runs): programs outside /usr, /opt
        // and /snap (downloads, builds, scripts); installed packages are not logged this way. One file name in many
        // folders is one entry (a build copied about); single files sharing a folder are one entry (new versions of a
        // program that updates itself); the most first.
        auto rows = query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'programs' AND title LIKE 'First run of %' "
                               "AND t >= ? AND t < ? ORDER BY t", {from, to});
        res.branch = "records:first-runs";
        const std::string rule = "The recorder notes a first run only for programs outside /usr, /opt and /snap (downloads, builds, scripts), "
                                 "not for installed packages.";
        if (rows.empty()) {
            if (polar) settle(res, r, false);
            res.body = "No program ran for the first time " + span + ".";
            res.notes.push_back(rule);
            return res;
        }
        auto tilde = [](const std::string& p) {
            if (p.rfind("/home/", 0) != 0) return p;
            const auto slash = p.find('/', 6);
            return slash == std::string::npos ? p : "~" + p.substr(slash);
        };
        struct Run {
            int64_t t;
            std::string folder, name;
        };
        std::vector<Run> runs;
        std::map<std::string, long> by_name;
        for (const auto& x : rows) {
            const std::string exe = x.s1.substr(13);
            const auto slash = exe.rfind('/');
            runs.push_back({x.t, exe.substr(0, slash == std::string::npos ? 0 : slash), exe.substr(slash == std::string::npos ? 0 : slash + 1)});
            ++by_name[runs.back().name];
        }
        struct Group {
            long n = 0;
            int64_t first = 0, last = 0;
            std::string name, folder, first_name, last_name;
            std::set<std::string> folders;
        };
        std::map<std::string, Group> groups;
        std::vector<std::string> order;
        for (const auto& u : runs) {
            const bool by_folder = by_name[u.name] == 1;
            const std::string key = by_folder ? "/" + u.folder : u.name;
            Group& g = groups[key];
            if (g.n++ == 0) {
                order.push_back(key);
                g.first = u.t, g.first_name = u.name, g.name = by_folder ? "" : u.name, g.folder = u.folder;
            }
            g.last = u.t, g.last_name = u.name;
            g.folders.insert(u.folder);
        }
        std::stable_sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) { return groups[a].n > groups[b].n; });
        std::vector<std::string> said;
        for (std::size_t i = 0; i < order.size() && i < 8; ++i) {
            const Group& g = groups[order[i]];
            const std::string when = g.n == 1 ? when_text(g.first) : "from " + when_text(g.first) + " to " + when_text(g.last);
            if (g.n == 1) said.push_back(g.first_name + " (" + tilde(g.folder) + ", " + when + ")");
            else if (!g.name.empty())
                said.push_back(g.name + " " + times(g.n) + " (" + (g.folders.size() > 1 ? "copies in " + unllm::number_word(long(g.folders.size())) + " folders, " : "") + when + ")");
            else
                said.push_back(unllm::number_word(g.n) + " new files in " + tilde(g.folder) + " (" + g.first_name + " to " + g.last_name + ", " + when + ")");
        }
        if (order.size() > 8) said.push_back(std::to_string(order.size() - 8) + " more");
        const long n = long(rows.size());
        if (polar) settle(res, r, true);
        res.body = capital(unllm::number_word(n)) + (n == 1 ? " program" : " programs") + " ran for the first time " + span +
                   ": " + unllm::join_list(said) + ". " + rule + " Ask about one by name and I'll check where it came from.";
        for (std::size_t i = rows.size() > 6 ? rows.size() - 6 : 0; i < rows.size(); ++i) res.receipts.push_back(when_text(rows[i].t) + "  " + rows[i].s1);
        return res;
    }
    if (kind == "network.new.list.window") {
        // A program's first connection on record is the earliest first_seen of its rows (one per program, address, port
        // and protocol). Programs seen online before that reached new addresses in the window are told apart: new places,
        // not their first time online. The recorder takes connections in snapshots, so a very short one can be missed.
        auto rows = query(db_, "SELECT CAST(MIN(first_seen) AS INTEGER), COUNT(*), (SELECT MIN(o.first_seen) FROM connections o WHERE o.program = c.program), "
                               "program, (SELECT f.remote || ':' || f.port FROM connections f WHERE f.program = c.program AND f.first_seen >= ?1 "
                               "AND f.first_seen < ?2 ORDER BY f.first_seen LIMIT 1) FROM connections c WHERE first_seen >= ?1 AND first_seen < ?2 "
                               "GROUP BY program ORDER BY 1", {from, to});
        res.branch = "records:first-connections";
        std::vector<std::string> fresh, known;
        for (const auto& x : rows) {
            const std::string places = x.a == 1 ? "one new place" : unllm::number_word(int64_t(x.a)) + " new places";
            if (int64_t(x.b) >= from) fresh.push_back(x.s1 + " at " + when_text(x.t) + " (to " + x.s2 + (x.a > 1 ? ", " + places : "") + ")");
            else known.push_back(x.s1 + " (" + places + ", first at " + when_text(x.t) + "; online since " + when_text(int64_t(x.b)) + ")");
            if (res.receipts.size() < 6) res.receipts.push_back(when_text(x.t) + "  " + x.s1 + " to " + x.s2);
        }
        if (fresh.empty()) {
            if (polar) settle(res, r, false);
            res.body = "No program connected out for the first time " + span + ".";
        } else {
            if (polar) settle(res, r, true);
            res.body = (fresh.size() == 1 ? "One program" : capital(unllm::number_word(long(fresh.size()))) + " programs") +
                       " connected out for the first time on record " + span + ": " + unllm::join_list(fresh) + ".";
        }
        if (!known.empty()) res.body += " Programs seen online before reached new places: " + unllm::join_list(known) + ".";
        res.notes.push_back("A connection shorter than the recorder's snapshots can be missed.");
        return res;
    }
    if (kind == "slow.procs.value.window") {
        // How many were running. The per-second readings count every process the kernel lists, its own threads included;
        // the process records keep programs proper, each run with its start and end, so they say what ran at a moment.
        if (r.window.live || r.window.present) {
            if (!sampler_) sampler_ = std::make_unique<Sampler>();
            const SnapshotPtr snap = sampler_->sample();
            long kernel = 0;
            std::set<std::string> names;
            for (const auto& p : snap->procs) {
                if (p.kernel_thread) ++kernel;
                else names.insert(p.name);
            }
            const long n = long(snap->procs.size());
            res.body = std::to_string(n) + " processes are running now: " + std::to_string(n - kernel) + " programs under " + std::to_string(names.size()) +
                       (names.size() == 1 ? " name" : " names") + ", and " + std::to_string(kernel) + " kernel threads.";
            res.branch = "records:procs-now";
            return res;
        }
        auto agg = query(db_, "SELECT COUNT(*), MIN(procs), MAX(procs), CAST(ROUND(AVG(procs)) AS INTEGER) FROM samples WHERE procs IS NOT NULL AND t >= ? AND t < ?",
                         {from, to});
        // The moment the process records are read at: the one named ("at noon"), else the window's middle.
        const int64_t at = r.window.at > 0 ? r.window.at : std::min(now, from + (to - from) / 2);
        auto running = query(db_, "SELECT COUNT(*), COUNT(DISTINCT name) FROM procs WHERE started <= ?1 AND IFNULL(ended, ?2) >= ?1", {at, now});
        auto top = query(db_, "SELECT COUNT(*), 0, 0, name FROM procs WHERE started <= ?1 AND IFNULL(ended, ?2) >= ?1 GROUP BY name ORDER BY 1 DESC, name LIMIT 5",
                         {at, now});
        res.branch = "records:procs";
        std::string text;
        if (!agg.empty() && agg[0].t > 0)
            text = std::string(r.window.stated ? "Between " + span_text(from, to, " and ") : "In the last day") + " the recorder counted " + fmt("%.0f", agg[0].a) + " to " +
                   fmt("%.0f", agg[0].b) + " processes running, " + agg[0].s1 + " on average; that count takes in the kernel's own threads.";
        if (!running.empty() && running[0].t > 0) {
            std::vector<std::string> names;
            for (const auto& x : top) names.push_back(x.s1 + (x.t > 1 ? " (" + std::to_string(x.t) + ")" : ""));
            text += std::string(text.empty() ? "" : " ") + "The process records, which leave those out, hold " + std::to_string(running[0].t) +
                    " programs running at " + when_text(at) + " under " + fmt("%.0f", running[0].a) + " names; the most: " + unllm::join_list(names) + ".";
        }
        res.body = text.empty() ? "I have no process counts " + span + "." : text;
        return res;
    }
    if (kind == "updates.when.window" || kind == "updates.identity.package") {
        // The package changes as the recorder logs them from pacman's log: a row per run, its packages listed under
        // Installed:, Upgraded:, Downgraded: and Removed:, with their versions.
        auto rows = query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title, detail FROM log WHERE category = 'software' AND t <= ? ORDER BY t", {now});
        auto verb = [](std::string v) {
            for (auto& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
            return v;
        };
        res.branch = "records:packages";
        // A named package: when it came, its latest change, and the version installed now.
        if (const std::string pkg = named_package(db_, an_, card_.question); !pkg.empty()) {
            const Row* first = nullptr;
            const Row* latest = nullptr;
            PackageLine came, last;
            std::vector<std::string> with;  // what came in the same run, named when the run was small
            for (const auto& row : rows)
                for (const auto& p : package_lines(row.s2))
                    if (p.name == pkg) {
                        if (!first) {
                            first = &row;
                            came = p;
                            for (const auto& o : package_lines(row.s2))
                                if (o.name != pkg && o.verb == p.verb) with.push_back(o.name);
                            if (with.size() > 3) with = {std::to_string(with.size()) + " other packages"};
                        }
                        latest = &row;
                        last = p;
                        if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
                    }
            if (!first) {
                res.body = "The package log has no change to " + pkg + " yet.";
                return res;
            }
            res.body = pkg + " was " + verb(came.verb) + " on " + moment_text(first->t) + (came.version.empty() ? "" : " (" + came.version + ")") +
                       (with.empty() ? "" : ", with " + unllm::join_list(with)) + ".";
            if (latest != first)
                res.body += " Its latest change: " + verb(last.verb) + " on " + moment_text(latest->t) + (last.version.empty() ? "" : " (" + last.version + ")") + ".";
            const std::string q = trim(run_out("pacman -Q " + pkg + " 2>/dev/null"));
            if (q.rfind(pkg + " ", 0) == 0) res.body += " Installed now: " + q.substr(pkg.size() + 1) + ".";
            else if (verb(last.verb) == "removed") res.body += " It isn't installed now.";
            return res;
        }
        if (kind == "updates.identity.package") {
            res.body = "Which package? Name it (like \"what version of mesa do i have\") and I'll give its version and when it was installed.";
            res.branch = "records:package-which";
            return res;
        }
        // The latest run that upgraded packages, with the rows of the same run (within half an hour of each other),
        // then the installs and removals after it. A "when" with no time named reads all the records.
        const bool all = !r.window.stated;
        const int64_t f = all ? 0 : from, t = all ? now : to;
        const std::string where = all ? "in the records" : span;
        auto upgraded = [&](const Row& row) {
            for (const auto& p : package_lines(row.s2))
                if (verb(p.verb) == "upgraded") return true;
            return row.s2.empty() && verb(row.s1).find("upgraded") != std::string::npos;  // a row logged without its packages
        };
        int u = -1;
        for (int i = 0; i < int(rows.size()); ++i)
            if (rows[std::size_t(i)].t >= f && rows[std::size_t(i)].t < t && upgraded(rows[std::size_t(i)])) u = i;
        if (u < 0) {
            int latest = -1;
            for (int i = 0; i < int(rows.size()); ++i)
                if (rows[std::size_t(i)].t >= f && rows[std::size_t(i)].t < t) latest = i;
            res.body = latest < 0 ? "No package changed " + where + "."
                                  : "No packages were upgraded " + where + "; the latest change was " + moment_text(rows[std::size_t(latest)].t) + ": " +
                                        rows[std::size_t(latest)].s1 + ".";
            return res;
        }
        std::size_t a = std::size_t(u), z = std::size_t(u);
        while (a > 0 && rows[a].t - rows[a - 1].t < 1800) --a;
        while (z + 1 < rows.size() && rows[z + 1].t - rows[z].t < 1800 && rows[z + 1].t < t) ++z;
        res.body = "The last time packages were upgraded was " + moment_text(rows[a].t) + ": " + rows[a].s1;
        std::vector<std::string> then;
        for (std::size_t i = a + 1; i <= z; ++i) then.push_back(verb(rows[i].s1.substr(0, 1)) + rows[i].s1.substr(1) + " at " + when_text(rows[i].t));
        res.body += (then.empty() ? "" : ", then " + unllm::join_list(then)) + ".";
        std::vector<std::string> after;
        for (std::size_t i = z + 1; i < rows.size() && rows[i].t < t; ++i) after.push_back(moment_text(rows[i].t) + " (" + rows[i].s1 + ")");
        if (!after.empty()) res.body += " Since then packages were only installed or removed: " + unllm::join_list(after) + ".";
        for (std::size_t i = a; i <= z && res.receipts.size() < 6; ++i) res.receipts.push_back(when_text(rows[i].t) + "  " + rows[i].s1);
        card_.moment = rows[a].t;
        return res;
    }
    if (kind == "startup.when.reboot" || kind == "startup.often.reboot") {
        // The journal's boots, and the recorder's own before the journal reaches; the window asked, and what the
        // machine did there when it didn't boot (it woke from sleep, the session ended and began again).
        const auto b = boot_starts(db_, now);
        if (b.empty()) {
            res.body = "I couldn't read the boot history from the journal or the records.";
            res.branch = "records:boots-none";
            return res;
        }
        auto at = [&](int64_t x) { return now - x < 6 * 86400 ? when_text(x) : moment_text(x); };
        for (std::size_t i = b.size() > 6 ? b.size() - 6 : 0; i < b.size(); ++i) res.receipts.push_back("boot " + at(b[i]));
        std::vector<std::string> in;
        int64_t up = 0;  // the boot the machine was running on when the window began
        for (int64_t x : b) {
            if (x >= from && x < to) in.push_back(at(x));
            if (x < from) up = x;
        }
        if (!r.window.stated && kind == "startup.when.reboot") {
            res.body = "It last started " + at(b.back()) + ", up " + duration_text(double(now - b.back())) + " since.";
            // Waking from sleep is starting up too: the last wake since that boot.
            const auto slept = sleeps(db_, now);
            if (!slept.empty() && slept.back().woke > b.back())
                res.body += " It last woke from sleep at " + at(slept.back().woke) + " (asleep since " + at(slept.back().went) + ").";
        } else if (!r.window.stated) {
            res.body = std::to_string(b.size()) + (b.size() == 1 ? " boot" : " boots") + " since " + at(b.front()) + ", as far back as the records go.";
        } else if (!in.empty()) {
            if (polar) settle(res, r, true);
            res.body = kind == "startup.when.reboot" ? "It started " + span + ", at " + unllm::join_list(in) + "."
                                                     : std::to_string(in.size()) + (in.size() == 1 ? " boot " : " boots ") + span + ": " + unllm::join_list(in) + ".";
        } else {
            if (polar) settle(res, r, false);
            res.body = "It didn't boot " + span + (up ? "; it had been up since the boot at " + at(up) : "") + ".";
            const std::string story = session_story(db_, from, to, now);
            if (!story.empty()) res.body += " In that window the records show: " + story + ".";
        }
        res.branch = "records:boots";
        return res;
    }
    if (kind == "startup.uptime.value.now") {
        double up = 0;
        if (std::ifstream in("/proc/uptime"); !(in >> up) || up <= 0) {
            res.body = "I couldn't read the uptime just now.";
            res.branch = "records:uptime-none";
            return res;
        }
        res.body = "It has been up for " + duration_text(int64_t(up)) + ", since " + moment_text(now - int64_t(up)) + ".";
        res.branch = "records:uptime";
        return res;
    }
    if (kind == "startup.value.boottime") {
        // systemd's "Startup finished" note, one per boot the journal holds: its text carries the times.
        struct Fin { int64_t t; std::string boot, total, parts; };
        std::vector<Fin> fins;
        for (const auto& e : journal("MESSAGE_ID=b07a249cd024414a82dd00cd181378ff")) {
            const std::string m = jfield(e, "MESSAGE");
            const auto eq = m.rfind(" = "), in = m.find(" in ");
            if (eq == std::string::npos || in == std::string::npos || in > eq) continue;
            std::string total = m.substr(eq + 3);
            while (!total.empty() && (total.back() == '.' || total.back() == ' ')) total.pop_back();
            fins.push_back({std::atoll(jfield(e, "__REALTIME_TIMESTAMP").c_str()) / 1000000, jfield(e, "_BOOT_ID"), total,
                            m.substr(in + 4, eq - in - 4)});
        }
        if (fins.empty()) {
            res.body = "The journal holds no boot timings.";
            res.branch = "records:boottime-none";
            return res;
        }
        // A window under an hour is "now" ("why does boot take forever now"): the latest boot answers it.
        const bool windowed = r.window.stated && to - from >= 3600;
        std::vector<const Fin*> asked;
        for (const auto& x : fins)
            if (windowed && x.t >= from && x.t < to) asked.push_back(&x);
        const bool none_in = windowed && asked.empty();
        // Each boot by its start, from the journal's boot list; the finish time when it isn't listed.
        const auto bl = boots();
        auto started = [&bl](const Fin& x) {
            for (const auto& b : bl)
                if (b.id == x.boot) return b.first;
            return x.t;
        };
        if (asked.empty()) asked.push_back(&fins.back());
        std::vector<std::string> said;
        for (const Fin* x : asked) said.push_back("the boot at " + when_text(started(*x)) + " took " + x->total + ": " + x->parts);
        res.body = (none_in ? "The machine didn't boot " + span + ". The latest boot, at " + when_text(started(fins.back())) + ", took " +
                                  fins.back().total + ": " + fins.back().parts
                            : unllm::join_list(said)) + ".";
        res.body[0] = char(std::toupper(static_cast<unsigned char>(res.body[0])));
        if (fins.size() > 1) {
            std::vector<std::string> all;
            for (const auto& x : fins) all.push_back(x.total + " on " + when_text(started(x)));
            res.body += " Every boot the journal holds: " + unllm::join_list(all) + ".";
        }
        // What started during that boot: blame for the running one; the journal's unit starts before
        // the finish for an earlier one.
        const Fin* last = asked.back();
        std::vector<std::string> slow;
        if (last == &fins.back()) {
            std::istringstream bs(run_out("systemd-analyze blame --no-pager 2>/dev/null | head -n 3"));
            for (std::string line; std::getline(bs, line);) {
                // "2min 13.402s unit.service": the last word is the unit, the rest its time
                const auto first = line.find_first_not_of(' '), cut = line.rfind(' ');
                if (first == std::string::npos || cut == std::string::npos || cut <= first) continue;
                const std::string unit = line.substr(cut + 1);
                slow.push_back(unit.substr(0, unit.rfind('.')) + " " + line.substr(first, cut - first));
            }
            if (!slow.empty()) res.body += " The units that took longest to start since this boot were " + unllm::join_list(slow) + ".";
        } else {
            auto starts = journal("_BOOT_ID=" + last->boot + " MESSAGE_ID=39f53479d3a045ac8e11786248231fbf --until=@" + std::to_string(last->t + 1));
            std::vector<std::string> tail;
            for (auto it = starts.rbegin(); it != starts.rend() && tail.size() < 3; ++it) {
                std::string u = jfield(*it, "UNIT");
                if (!u.empty()) tail.push_back(u.substr(0, u.rfind('.')));
            }
            if (!starts.empty())
                res.body += " " + std::to_string(starts.size()) + " services started during it; the last to come up were " + unllm::join_list(tail) + ".";
        }
        res.branch = "records:boottime";
        return res;
    }
    if (kind == "programs.owner.list.now") {
        // "Which processes are not owned by root": every process by the account it runs as, with the busiest of each.
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr s = sampler_->sample();
        std::map<std::string, std::vector<const ProcessInfo*>> by;
        for (const auto& p : s->procs)
            if (!p.self) by[p.user.empty() ? "an unknown account" : p.user].push_back(&p);
        std::vector<std::pair<std::string, std::vector<const ProcessInfo*>>> list(by.begin(), by.end());
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
        std::vector<std::string> said;
        std::size_t total = 0;
        for (auto& [user, procs] : list) {
            total += procs.size();
            std::sort(procs.begin(), procs.end(), [](const ProcessInfo* a, const ProcessInfo* b) { return a->cpu > b->cpu; });
            std::vector<std::string> top;
            for (std::size_t i = 0; i < procs.size() && top.size() < 3; ++i)
                if (std::find(top.begin(), top.end(), procs[i]->name) == top.end()) top.push_back(procs[i]->name);
            said.push_back(std::to_string(procs.size()) + " as " + user + " (" + unllm::join_list(top) + ")");
            if (res.receipts.size() < 8) res.receipts.push_back(user + "  " + std::to_string(procs.size()) + " processes");
        }
        res.body = std::to_string(total) + " processes are running right now: " + unllm::join_list(said) + ".";
        res.notes.push_back("Each account's busiest programs are in brackets; kernel threads run as root.");
        res.branch = "records:procs-by-owner";
        return res;
    }
    if (kind == "sound.value.now") {
        // The desktop's sound server (wpctl, PipeWire): the default output's volume and whether it is muted, and its name.
        const std::string vol = trim(run_out("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null"));  // "Volume: 0.50 [MUTED]"
        if (vol.rfind("Volume:", 0) != 0) {
            res.body = "I can't read the sound settings: the desktop's sound server (wpctl) didn't answer.";
            res.branch = "records:sound-none";
            return res;
        }
        const bool muted = vol.find("MUTED") != std::string::npos;
        const double level = std::atof(vol.c_str() + 7) * 100;
        std::string output;
        std::istringstream in(run_out("wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null"));
        for (std::string line; std::getline(in, line);)
            if (const auto at = line.find("node.description = \""); at != std::string::npos && line.rfind('"') > at + 20)
                output = line.substr(at + 20, line.rfind('"') - at - 20);
        // "Is it muted" and "is the sound on" ask opposite things: the side the question is nearer by meaning.
        if (polar) {
            const int side = asked_test(r, {{"the sound is muted", "is it muted"}, {"the sound is on", "is the volume turned up"}});
            if (side < 0) res.undecided = true;
            else res.lead = (side == 0 ? muted : !muted) ? "Yes." : "No.";
        }
        res.body = std::string(muted ? "The sound is muted" : "The sound isn't muted") + fmt(", with the volume set to %.0f%%", level) +
                   (output.empty() ? "" : " on " + output) + ".";
        if (r.window.stated && !r.window.live && !r.window.present) res.scope.push_back("Sound settings aren't kept over time, so this is the present reading.");
        res.receipts.push_back("wpctl: " + vol);
        res.branch = "records:sound";
        return res;
    }
    if (kind == "startup.cause.reboot") {
        // "What caused the last shutdown": the previous boot's end in the journal's boot list; clean (the journal holds the
        // shutdown mark for that boot and no stop event covers it) or a stop; then what the log and the readings show in
        // the hour before it: the power button, the charger and battery, updates, crashes, and for a stop the recorder's
        // story of its last minutes.
        const auto b = boots();
        if (b.size() < 2) {
            res.body = "The journal shows no earlier boot, so there is no shutdown on record to explain" +
                       (b.empty() ? std::string(".") : "; it has been up since " + when_text(b.back().first) + ".");
            res.branch = "records:shutdown-none";
            return res;
        }
        const Boot& prev = b[b.size() - 2];
        const int64_t stop = prev.second;
        auto evs = query(db_, "SELECT CAST(start AS INTEGER), 0, 0, summary FROM events WHERE (kinds & ?1) != 0 AND start >= ?2 AND start < ?3 ORDER BY start DESC LIMIT 1",
                         {int64_t(KindShutdown), stop - 600, stop + 600});
        const bool clean = evs.empty() && !journal("-n 1 _BOOT_ID=" + prev.id + " MESSAGE_ID=98268866d1d54a499c4e98921d93bc40").empty();
        res.body = "The last shutdown was " + moment_text(stop) + (clean ? ", a clean one" : ": the machine stopped without shutting down properly") + ".";
        auto rows = query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE t >= ?1 AND t <= ?2 AND category IN ('session', 'software', 'problems', 'admin') "
                               "ORDER BY t DESC LIMIT 4", {stop - 3600, stop + 5});
        std::vector<std::string> said;
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) said.push_back(it->s1 + " at " + clock_text(it->t));
        if (!said.empty()) res.body += " In the hour before it: " + unllm::join_list(said) + ".";
        auto bat = query(db_, "SELECT CAST(t AS INTEGER), battery_pct, on_ac FROM samples WHERE t <= ?1 AND t >= ?2 AND battery_pct IS NOT NULL ORDER BY t DESC LIMIT 1",
                         {stop, stop - 300});
        if (!bat.empty() && bat[0].b == 0 && bat[0].a <= 5) res.body += fmt(" The battery was at %.0f%% and unplugged, which by itself ends a session.", bat[0].a);
        if (!clean) {
            std::string why = evs.empty() ? "" : evs[0].s1;
            if (const auto at = why.find("). "); at != std::string::npos && at < 40) why = why.substr(at + 3);
            // Its opening ("The machine stopped without warning on ...") repeats the lead: the explanation follows it.
            if (why.rfind("The machine stopped without warning", 0) == 0)
                if (const auto at = why.find(". "); at != std::string::npos) why = why.substr(at + 2);
            const std::string told = stop_story(db_, stop);
            // The event's own readings are told again, fuller, by stop_story: said once.
            if (!told.empty())
                for (const char* own : {" The last reading", " The recorder read on", " In its last two minutes", " The busiest program"})
                    if (const auto at = why.find(own); at != std::string::npos) why.erase(at);
            if (!why.empty() || !told.empty()) res.body += " " + why + (why.empty() || told.empty() ? "" : " ") + told;
            else res.notes.push_back("The log doesn't say whether it hung, lost power or crashed.");
        } else if (said.empty()) {
            res.notes.push_back("The log has nothing in the hour before it, so most likely a shutdown you chose.");
        }
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) res.receipts.push_back(when_text(it->t) + "  " + it->s1);
        res.branch = clean ? "records:shutdown-clean" : "records:shutdown-stop";
        return res;
    }
    if (kind == "programs.open.cause.now") {
        // "Why won't firefox open": whether it is running already (an instance with no window), else its starts in the last
        // hour and how they ended (a start over within seconds is a failed launch), its crashes in the last day, and its
        // last run. Why a launch fails is in the program's own messages, which aren't recorded.
        if (program.empty()) {
            res.body = "Which program won't open? Name it and I'll check whether it is running, whether it was started lately and how those starts ended.";
            res.branch = "records:open-which";
            return res;
        }
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr s = sampler_->sample();
        std::vector<const ProcessInfo*> live;
        for (const auto& p : s->procs)
            if (is_program(program, p.name, p.exe, p.cmdline)) live.push_back(&p);
        std::sort(live.begin(), live.end(), [](const ProcessInfo* a, const ProcessInfo* b) { return a->started_epoch < b->started_epoch; });
        card_.subject = program;
        card_.subjects_named = {program};
        if (!live.empty()) {
            res.body = program + " is running: " + std::to_string(live.size()) + (live.size() == 1 ? " process" : " processes") +
                       (live[0]->started_epoch > 0 ? ", the oldest since " + moment_text(int64_t(live[0]->started_epoch)) : "") +
                       ". If no window shows, that instance is running without one; closing it (or killing it) and starting again usually brings the window back.";
        } else {
            auto starts = query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, 0), 0, name FROM procs WHERE started >= ?1 AND is_program(?2, name, exe, cmdline) "
                                     "ORDER BY started DESC LIMIT 10", {now - 3600}, {program});
            std::size_t brief = 0;
            for (const auto& x : starts)
                if (x.a > 0 && x.a - x.t < 10) ++brief;
            if (starts.empty()) {
                res.body = program + " isn't running, and it hasn't been started in the last hour as far as the records go." + last_run_text(db_, program, now);
            } else {
                res.body = program + " isn't running. It was started " + times(long(starts.size())) + " in the last hour" +
                           (brief ? ", and " + (brief == starts.size() ? std::string("every start") : std::to_string(brief) + " of them") +
                                        " ended within seconds: a failed launch, not a window you missed"
                                  : ", the last at " + clock_text(starts[0].t) + (starts[0].a > 0 ? ", and it ran until " + clock_text(int64_t(starts[0].a)) : "")) +
                           ".";
            }
            std::vector<Mishap> bad;
            for (const auto& m : mishaps(db_, now - 86400, now))
                if (about(m)) bad.push_back(m);
            if (!bad.empty()) res.body += " It crashed " + times(count_of(bad)) + " in the last day, the last at " + when_text(bad.back().t) + ".";
            for (const auto& x : starts)
                if (res.receipts.size() < 6) res.receipts.push_back(when_text(x.t) + "  started" + (x.a > 0 ? ", ended " + clock_text(int64_t(x.a)) : ""));
        }
        res.notes.push_back("Why a launch fails is in the program's own messages, which aren't recorded; starting it from a terminal shows them.");
        res.branch = "records:wont-open";
        return res;
    }
    if (kind == "gpu.model.now") {
        // Ollama's own list of what it has loaded (its local API), then any process serving a model file named on its
        // command line (--model PATH or -m PATH); with neither, the last such run on record.
        std::vector<std::string> loaded;
        const std::string ps = run_out("curl -4 -s -m 2 http://127.0.0.1:11434/api/ps 2>/dev/null");
        for (std::size_t at = 0; (at = ps.find("\"name\":\"", at)) != std::string::npos; at += 8) {
            const std::string win = ps.substr(at, 900);
            const std::string name = jfield(win, "name"), vram = jfield(win, "size_vram");
            if (!name.empty())
                loaded.push_back(name + (vram.empty() || std::atof(vram.c_str()) <= 0 ? "" : " (" + human_bytes(std::atof(vram.c_str())) + " in video memory)"));
        }
        auto model_arg = [](const std::string& cmd) {
            for (const char* flag : {"--model ", " -m "}) {
                const auto at = cmd.find(flag);
                if (at == std::string::npos) continue;
                const auto s = at + std::strlen(flag), e = cmd.find(' ', s);
                const std::string p = cmd.substr(s, e == std::string::npos ? std::string::npos : e - s);
                if (!p.empty() && p[0] == '/') return p;
            }
            return std::string();
        };
        auto model_name = [](const std::string& path) {
            const std::string b = path.substr(path.rfind('/') + 1);
            return b.rfind("sha256-", 0) == 0 ? "an Ollama model file (" + b.substr(0, 19) + "...)" : b;
        };
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr s = sampler_->sample();
        std::vector<std::string> servers;
        for (const auto& p : s->procs)
            if (const std::string m = model_arg(p.cmdline); !m.empty())
                servers.push_back(p.name + " serving " + model_name(m) +
                                  (p.gpu_mem.ok() && p.gpu_mem.get() > 0 ? " (" + human_bytes(p.gpu_mem.get()) + " of video memory)" : ""));
        if (!loaded.empty()) {
            res.body = "Ollama has " + unllm::join_list(loaded) + " loaded right now.";
        } else if (!servers.empty()) {
            res.body = "Running right now: " + unllm::join_list(servers) + ". Ollama itself reports nothing loaded.";
        } else {
            // The last model server on record: a program that has held video memory in the recorder's events (so a speech
            // voice or another small model file doesn't stand in for the AI model), else any process serving a model file.
            auto last = query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, 0), 0, name, cmdline FROM procs p WHERE (cmdline LIKE '%--model /%' OR cmdline LIKE '% -m /%') "
                                   "AND EXISTS (SELECT 1 FROM event_culprits c WHERE c.name = p.name AND c.gpu_mem > 0) ORDER BY started DESC LIMIT 1", {});
            if (last.empty())
                last = query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, 0), 0, name, cmdline FROM procs WHERE cmdline LIKE '%--model /%' OR cmdline LIKE '% -m /%' "
                                  "ORDER BY started DESC LIMIT 1", {});
            res.body = std::string("No model is loaded right now: Ollama reports none") + (ps.empty() ? " (it isn't answering)" : "") + " and no process is serving a model file.";
            if (!last.empty())
                res.body += " The last one on record was " + last[0].s1 + " serving " + model_name(model_arg(last[0].s2)) + ", " +
                            (last[0].a > 0 ? span_text(last[0].t, int64_t(last[0].a), " to ") : "from " + moment_text(last[0].t)) + ".";
        }
        for (const auto& x : loaded) res.receipts.push_back("ollama: " + x);
        for (const auto& x : servers) res.receipts.push_back(x);
        res.branch = "records:model";
        return res;
    }
    if (kind == "disk.folder.list.now") {
        namespace fs = std::filesystem;
        const char* h = getenv("HOME");
        const std::string home = h ? h : "/";
        auto shorten = [&](const std::string& p) { return p == home ? std::string("~") : p.rfind(home + "/", 0) == 0 ? "~" + p.substr(home.size()) : p; };
        auto lower = [](std::string s) {
            for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        // The folder: a path typed, else one of the desktop's named folders (user-dirs.dirs) or a top-level folder of home
        // that a word of the question names.
        std::string folder;
        for (std::size_t at = 0; at < question_.size() && folder.empty(); ++at)
            if ((question_[at] == '/' || (question_[at] == '~' && (at + 1 == question_.size() || question_[at + 1] == '/'))) &&
                (at == 0 || question_[at - 1] == ' ')) {
                const std::size_t end = question_.find_first_of(" ?,", at);
                folder = question_.substr(at, end == std::string::npos ? std::string::npos : end - at);
                if (folder[0] == '~') folder = home + folder.substr(1);
                while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
            }
        std::vector<std::pair<std::string, std::string>> known;  // a folder's name in lower case, and its path
        if (std::ifstream in(home + "/.config/user-dirs.dirs"); in)
            for (std::string line; std::getline(in, line);) {
                const auto q1 = line.find('"'), q2 = line.rfind('"');
                if (line.rfind("XDG_", 0) != 0 || q1 == std::string::npos || q2 <= q1) continue;
                std::string p = line.substr(q1 + 1, q2 - q1 - 1);
                if (p.rfind("$HOME", 0) == 0) p = home + p.substr(5);
                while (p.size() > 1 && p.back() == '/') p.pop_back();
                if (p != home) known.emplace_back(lower(p.substr(p.rfind('/') + 1)), p);
            }
        for (const auto& name : list_dir(home))
            if (!name.empty() && name[0] != '.' && fs::is_directory(home + "/" + name)) known.emplace_back(lower(name), home + "/" + name);
        if (folder.empty())
            for (const auto& t : r.terms)
                for (const auto& [name, path] : known)
                    if (folder.empty() && t.size() >= 4 && (name == t || name.rfind(t, 0) == 0)) folder = path;
        std::error_code ec;
        if (folder.empty() || !fs::is_directory(folder, ec)) {
            std::vector<std::string> names;
            for (const auto& [name, path] : known)
                if (names.size() < 8 && std::find(names.begin(), names.end(), shorten(path)) == names.end()) names.push_back(shorten(path));
            res.body = (folder.empty() ? std::string("Which folder?") : "There is no folder " + shorten(folder) + ".") + " Give its path (like ~/Downloads)" +
                       (names.empty() ? "." : ", or one of yours: " + unllm::join_list(names) + ".");
            res.branch = "records:folder-which";
            return res;
        }
        struct Entry {
            std::string name;
            double bytes = 0;
            int64_t mtime = 0;
            bool dir = false;
        };
        std::vector<Entry> entries;
        double bytes = 0;
        long files = 0, dirs = 0, hidden = 0;
        for (const auto& e : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
            const std::string name = e.path().filename().string();
            if (name.empty() || name[0] == '.') {
                ++hidden;
                continue;
            }
            Entry x;
            x.name = name;
            x.dir = e.is_directory(ec);
            struct stat st {};
            if (stat(e.path().c_str(), &st) == 0) {
                x.mtime = st.st_mtime;
                if (!x.dir && S_ISREG(st.st_mode)) x.bytes = double(st.st_size), bytes += x.bytes;
            }
            (x.dir ? dirs : files)++;
            entries.push_back(x);
            if (entries.size() >= 20000) break;
        }
        card_.subject = shorten(folder);
        if (entries.empty()) {
            res.body = shorten(folder) + " has nothing in it" + (hidden ? ", apart from " + std::to_string(hidden) + " hidden entries" : "") + ".";
            res.branch = "records:folder-empty";
            return res;
        }
        res.body = shorten(folder) + " holds " + std::to_string(entries.size()) + (entries.size() == 1 ? " item: " : " items: ") +
                   (files ? std::to_string(files) + (files == 1 ? " file" : " files") : "") + (files && dirs ? " and " : "") +
                   (dirs ? std::to_string(dirs) + (dirs == 1 ? " folder" : " folders") : "") +
                   (bytes > 0 ? ", " + human_bytes(bytes) + " in the files at the top level" : "") +
                   (hidden ? " (" + std::to_string(hidden) + (hidden == 1 ? " hidden entry" : " hidden entries") + " not counted)" : "") + ".";
        if (entries.size() <= 8) {
            std::vector<std::string> all;
            for (const auto& x : entries) all.push_back(x.name + (x.dir ? " (folder)" : x.bytes > 0 ? " (" + human_bytes(x.bytes) + ")" : ""));
            res.body += " They are " + unllm::join_list(all) + ".";
        } else {
            std::vector<Entry> newest = entries;
            std::sort(newest.begin(), newest.end(), [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });
            std::vector<std::string> said;
            for (std::size_t i = 0; i < newest.size() && i < 3; ++i) said.push_back(newest[i].name + (newest[i].mtime ? " (" + when_text(newest[i].mtime) + ")" : ""));
            res.body += " The newest: " + unllm::join_list(said) + ".";
            std::vector<Entry> biggest;
            for (const auto& x : entries)
                if (!x.dir && x.bytes > 0) biggest.push_back(x);
            std::sort(biggest.begin(), biggest.end(), [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
            said.clear();
            for (std::size_t i = 0; i < biggest.size() && i < 3; ++i) said.push_back(biggest[i].name + " (" + human_bytes(biggest[i].bytes) + ")");
            if (!said.empty()) res.body += " The biggest: " + unllm::join_list(said) + ".";
        }
        for (const auto& x : entries)
            if (res.receipts.size() < 8) res.receipts.push_back(x.name + (x.dir ? "/" : "  " + human_bytes(x.bytes)));
        res.branch = "records:folder";
        return res;
    }
    if (kind == "disk.find.which.now") {
        const char* h = getenv("HOME");
        const std::string home = h ? h : "/";
        auto shorten = [&](const std::string& p) { return p.rfind(home + "/", 0) == 0 ? "~" + p.substr(home.size()) : p; };
        auto quote = [](const std::string& x) {
            std::string q = "'";
            for (char c : x) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
            return q + "'";
        };
        // The name, as typed: text in quotes; else a word with a dot in it ("history.db", "spine-audit.md"); else the words
        // after "called" or "named" ("the file called aeon flux"); else the question's words the analyzer doesn't know
        // (the stemmed forms would break a name). A program named with no file name asks for the program's own file.
        std::string name;
        if (const auto q1 = question_.find('"'); q1 != std::string::npos)
            if (const auto q2 = question_.find('"', q1 + 1); q2 != std::string::npos && q2 > q1 + 1) name = question_.substr(q1 + 1, q2 - q1 - 1);
        if (name.empty()) {
            std::istringstream in(question_);
            for (std::string w; in >> w;) {
                while (!w.empty() && (w.back() == '?' || w.back() == ',' || w.back() == '.' || w.back() == '"')) w.pop_back();
                if (!w.empty() && w[0] == '"') w.erase(0, 1);
                if (w.size() >= 3 && w.find('.') != std::string::npos && w[0] != '.' && w.find("..") == std::string::npos) {
                    name = w;
                    break;
                }
            }
        }
        if (name.empty()) {
            std::istringstream in(question_);
            std::vector<std::string> raw;
            for (std::string w; in >> w;) raw.push_back(w);
            for (std::size_t i = 0; i + 1 < raw.size() && name.empty(); ++i) {
                std::string low = raw[i];
                for (auto& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (low != "called" && low != "named") continue;
                for (std::size_t k = i + 1; k < raw.size(); ++k) {
                    std::string w = raw[k];
                    while (!w.empty() && (w.back() == '?' || w.back() == ',' || w.back() == '.')) w.pop_back();
                    if (!w.empty()) name += (name.empty() ? "" : " ") + w;
                }
            }
        }
        if (!program.empty() && (name.empty() || name == program)) {
            // "where is firefox located": the program's own file, from the records of its runs, else the PATH.
            namespace fs = std::filesystem;
            auto rows = query(db_, "SELECT 0, 0, 0, exe FROM procs WHERE exe != '' AND is_program(?1, name, exe, cmdline) ORDER BY started DESC LIMIT 1", {}, {program});
            std::string path = rows.empty() ? "" : rows[0].s1;
            if (path.empty())
                if (const char* pe = getenv("PATH")) {
                    std::istringstream dirs(pe);
                    for (std::string dir; std::getline(dirs, dir, ':') && path.empty();) {
                        std::error_code ec;
                        if (!dir.empty() && fs::is_regular_file(dir + "/" + program, ec)) path = dir + "/" + program;
                    }
                }
            if (path.empty()) {
                res.body = program + " has no file on record: it never ran while the recorder watched, and nothing by that name is on the PATH.";
                res.branch = "records:find-program-none";
                return res;
            }
            std::error_code ec;
            const std::string target = fs::is_symlink(path, ec) ? fs::canonical(path, ec).string() : "";
            res.body = program + " is at " + shorten(path) + (target.empty() || target == path ? "" : " (a link to " + shorten(target) + ")") + ", the program's own file.";
            // Other copies it has run as: files named like it (a package tool whose command line mentions it is not it).
            std::vector<std::string> own;
            std::string low_program = program;
            for (auto& c : low_program) c = char(std::tolower(static_cast<unsigned char>(c)));
            for (const auto& x : query(db_, "SELECT 0, 0, 0, exe FROM procs WHERE exe != '' AND exe != ?2 AND is_program(?1, name, exe, cmdline) GROUP BY exe ORDER BY COUNT(*) DESC LIMIT 6",
                                       {}, {program, path})) {
                std::string base = x.s1.substr(x.s1.rfind('/') + 1);
                for (auto& c : base) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (base.find(low_program) != std::string::npos && own.size() < 2) own.push_back(shorten(x.s1));
            }
            if (!own.empty()) res.body += " It has also run as " + unllm::join_list(own) + ".";
            res.receipts.push_back(path);
            card_.subject = program;
            card_.subjects_named = {program};
            res.branch = "records:find-program";
            return res;
        }
        if (name.empty()) {
            std::istringstream in(question_);
            std::vector<std::string> raw;
            for (std::string w; in >> w;) {
                while (!w.empty() && (w.back() == '?' || w.back() == ',' || w.back() == '.')) w.pop_back();
                raw.push_back(w);
            }
            for (const auto& term : named_words(r, kind))
                for (const auto& w : raw) {
                    std::string low = w;
                    for (auto& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (low.rfind(term, 0) == 0 && name.find(w) == std::string::npos) {
                        name += (name.empty() ? "" : " ") + w;
                        break;
                    }
                }
        }
        if (name.empty()) {
            res.body = "Which file or folder? Give its name, like \"where is history.db\".";
            res.branch = "records:find-which";
            return res;
        }
        // The file index first (plocate, by base name, case aside), then a walk of the home folder when it has nothing.
        std::string out = run_out("plocate -i -b -l 12 -- " + quote(name) + " 2>/dev/null");
        const bool indexed = !out.empty();
        bool cut_short = false;  // the walk ran out of time (a busy machine, a huge tree)
        // The index may leave the home folder out (here it gave /etc/bash.bashrc for "bashrc" and never ~/.bashrc), so
        // home is walked whenever the index found nothing in it. -I: files a project's .gitignore lists (a build folder)
        // are files all the same.
        if (out.find(home + "/") == std::string::npos) {
            std::string walked = run_out("timeout 25 fd -F -i -H -I --max-results 12 --exclude .cache --exclude .git -- " + quote(name) + " " + quote(home) +
                                         " 2>/dev/null; echo \"__exit=$?\"");
            if (const auto at = walked.rfind("__exit="); at != std::string::npos) {
                cut_short = walked.compare(at + 7, 3, "124") == 0;
                walked.erase(at);
            }
            out = walked + out;
        }
        struct Hit {
            std::string path;
            double bytes = 0;
            int64_t mtime = 0;
            bool dir = false, mine = false;
        };
        std::vector<Hit> hits;
        {
            std::istringstream in(out);
            for (std::string line; std::getline(in, line);) {
                if (line.empty()) continue;
                while (line.size() > 1 && line.back() == '/') line.pop_back();
                Hit x;
                x.path = line;
                x.mine = line.rfind(home + "/", 0) == 0;
                struct stat st {};
                if (stat(line.c_str(), &st) == 0) {
                    x.mtime = st.st_mtime;
                    x.dir = S_ISDIR(st.st_mode);
                    if (S_ISREG(st.st_mode)) x.bytes = double(st.st_size);
                }
                hits.push_back(x);
            }
        }
        if (hits.empty()) {
            res.body = "Nothing named \"" + name + "\" is in the file index or your home folder" + (cut_short ? ", as far as a 25-second walk of it got" : "") + ".";
            res.notes.push_back(cut_short ? "The walk of your home folder ran out of time, so a file deep in a big tree may have been missed."
                                          : "The file index is rebuilt once a day, so a file made today shows only when it is in your home folder.");
            res.branch = "records:find-none";
            return res;
        }
        // The file named exactly first ("history.db" before "history.db-wal"), then yours before the system's, newest first.
        auto lower = [](std::string s) {
            for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string want = lower(name);
        auto exact = [&](const Hit& h) { return lower(h.path.substr(h.path.rfind('/') + 1)) == want; };
        std::sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) {
            if (exact(a) != exact(b)) return exact(a);
            return a.mine != b.mine ? a.mine : a.mtime > b.mtime;
        });
        const Hit& h0 = hits[0];
        res.body = name + " is at " + shorten(h0.path) + (h0.dir ? " (a folder" : " (" + human_bytes(h0.bytes)) +
                   (h0.mtime ? ", changed " + when_text(h0.mtime) : "") + ").";
        if (hits.size() > 1) {
            std::vector<std::string> more;
            for (std::size_t i = 1; i < hits.size() && i < 4; ++i) more.push_back(shorten(hits[i].path));
            res.body += " Also: " + unllm::join_list(more) + (hits.size() > 4 ? ", and " + std::to_string(hits.size() - 4) + " more" : "") + ".";
        }
        for (const auto& x : hits)
            if (res.receipts.size() < 8) res.receipts.push_back(x.path);
        if (indexed) res.notes.push_back("Found through the file index (plocate), which is rebuilt once a day.");
        card_.subject = name;
        res.branch = "records:find";
        return res;
    }
    if (kind == "programs.pid.identity.now") {
        std::string pid_s;
        for (const auto& e : r.entities)
            if (e.type == "pid") pid_s = e.value;
        if (pid_s.empty()) {
            res.body = "Which process? Give its number, like \"explain pid 2310407\"; the Processes page lists them.";
            res.branch = "records:pid-which";
            return res;
        }
        const int64_t pid = std::atoll(pid_s.c_str());
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr s = sampler_->sample();
        const ProcessInfo* live = nullptr;
        std::string parent;
        for (const auto& p : s->procs)
            if (p.key.pid == pid) live = &p;
        if (live)
            for (const auto& p : s->procs)
                if (p.key.pid == live->ppid) parent = p.name;
        // The fact table's word on the program ("what is kswapd0").
        auto fact = [&](const std::string& n) {
            const Fact* f = facts_.lookup(n);
            if (!f || f->explanation.empty()) return std::string();
            std::string e = f->explanation;
            if (e.back() != '.') e += '.';
            return " " + n + ": " + e;
        };
        if (live) {
            std::string cmd = live->cmdline.substr(0, 90);
            if (live->cmdline.size() > 90) cmd += "...";
            res.body = "Process " + pid_s + " is " + live->name + (cmd.empty() || cmd == live->name ? "" : " (" + cmd + ")") + ", started by " +
                       (parent.empty() ? "pid " + std::to_string(live->ppid) : parent) + (live->user.empty() ? "" : " as " + live->user) +
                       (live->unit.empty() ? "" : " in " + live->unit) +
                       (live->started_epoch > 0 ? ", running since " + moment_text(int64_t(live->started_epoch)) : "") + ". Right now it uses " +
                       fmt("%.0f%%", live->cpu) + " of the CPU and holds " + human_bytes(double(live->rss)) + " of memory" +
                       (live->gpu_mem.ok() && live->gpu_mem.get() > 0 ? " and " + human_bytes(live->gpu_mem.get()) + " of video memory" : "") + ".";
            res.body += fact(live->name);
            // weird's verdict on where it came from: the first paragraph of its report.
            const std::string out = run_out("weird --offline --no-prompt " + pid_s + " 2>/dev/null");
            if (const auto end = out.find("\n\n"); end != std::string::npos && out.find("(pid " + pid_s + ")") < end) {
                std::string v = out.substr(0, end);
                for (auto& c : v) c = c == '\n' ? ' ' : c;
                res.body += " " + v;
            }
            card_.subject = live->name;
            card_.subjects_named = {live->name};
            res.receipts.push_back("pid " + pid_s + "  live" + (live->unit.empty() ? "" : "  " + live->unit));
        } else {
            auto rec = query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, 0), cpu_seconds, name, cmdline FROM procs WHERE pid = ?1 ORDER BY started DESC LIMIT 1", {pid});
            auto who = query(db_, "SELECT 0, 0, 0, IFNULL(parent, ''), IFNULL(user, '') FROM procs WHERE pid = ?1 ORDER BY started DESC LIMIT 1", {pid});
            if (rec.empty()) {
                res.body = "No process " + pid_s + " is running now, and none by that number is on record.";
            } else {
                res.body = "No process " + pid_s + " is running now. The records show it was " + rec[0].s1 +
                           (rec[0].s2.empty() || rec[0].s2 == rec[0].s1 ? "" : " (" + rec[0].s2.substr(0, 90) + ")") +
                           (who.empty() || who[0].s1.empty() ? "" : ", started by " + who[0].s1) + (who.empty() || who[0].s2.empty() ? "" : " as " + who[0].s2) +
                           ", running " + (rec[0].a > 0 ? span_text(rec[0].t, int64_t(rec[0].a), " to ") : "from " + moment_text(rec[0].t) + " with no end recorded") +
                           (rec[0].b > 0 ? ", using " + (rec[0].b >= 60 ? duration_text(rec[0].b) : fmt("%.0f s", rec[0].b)) + " of CPU time" : "") + "." +
                           fact(rec[0].s1) + " Process numbers are reused after a restart, so this may not be the one you saw.";
                card_.subject = rec[0].s1;
            }
            res.receipts.push_back("pid " + pid_s + "  from the records");
        }
        res.branch = "records:pid";
        return res;
    }
    if (kind == "programs.list.now") {
        // The apps the desktop launched and still running: systemd runs each in a unit named "app-<id>..." (firefox in
        // app-firefox@..., haruna in app-org.kde.haruna@1.service), so the unit says which processes are apps and which.
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr s = sampler_->sample();
        std::map<std::string, int> apps;
        std::map<std::string, bool> named;  // the apps the question's program is
        for (const auto& p : s->procs) {
            if (p.self || p.unit.rfind("app-", 0) != 0) continue;
            std::string id = p.unit.substr(4);
            if (const auto at = id.find('@'); at != std::string::npos) id = id.substr(0, at);
            else if (const auto dot = id.rfind('.'); dot != std::string::npos) id = id.substr(0, dot);  // ".scope", ".service"
            if (const auto dot = id.rfind('.'); dot != std::string::npos) id = id.substr(dot + 1);  // "org.kde.haruna": haruna
            for (std::size_t x; (x = id.find("\\x2d")) != std::string::npos;) id.replace(x, 4, "-");  // systemd's "-"
            ++apps[id];
            if (!program.empty() && is_program(program, p.name, p.exe, p.cmdline)) named[id] = true;
        }
        std::vector<std::pair<std::string, int>> list(apps.begin(), apps.end());
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        if (list.empty()) {
            res.body = "No desktop apps are open right now: nothing is running in a unit the desktop launched.";
        } else {
            std::vector<std::string> said;
            for (const auto& [id, n] : list) said.push_back(id + (n > 1 ? " (" + std::to_string(n) + " processes)" : ""));
            res.body = capital(unllm::number_word(long(list.size()))) + (list.size() == 1 ? " app is" : " apps are") + " open right now: " +
                       unllm::join_list(said) + ".";
            for (const auto& [id, n] : list)
                if (res.receipts.size() < 8) res.receipts.push_back(id + "  " + std::to_string(n) + (n == 1 ? " process" : " processes"));
        }
        // "is kwrite open": the yes or no first, for the app the question names.
        if (polar && !program.empty()) {
            res.lead = named.empty() ? "No." : "Yes.";
            res.body = (named.empty() ? program + " isn't open. " : program + " is open. ") + res.body;
        }
        res.notes.push_back("Apps are what the desktop launched; programs started from a terminal or by a service aren't counted here.");
        res.branch = "records:apps-open";
        return res;
    }
    if (kind == "programs.installed.now") {
        // The desktop's own list of installed apps: its .desktop entries, system-wide and yours, hidden ones skipped.
        namespace fs = std::filesystem;
        const char* h = getenv("HOME");
        std::vector<std::pair<std::string, int64_t>> apps;  // the name and when its entry was written
        std::set<std::string> seen;
        for (const std::string dir : {std::string("/usr/share/applications"), (h ? std::string(h) : std::string()) + "/.local/share/applications"}) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (e.path().extension() != ".desktop") continue;
                std::ifstream in(e.path());
                std::string line, name;
                bool hidden = false, app = false, entry = false;
                while (std::getline(in, line)) {
                    if (!line.empty() && line[0] == '[') {
                        if (entry) break;
                        entry = line == "[Desktop Entry]";
                        continue;
                    }
                    if (!entry) continue;
                    if (line.rfind("Name=", 0) == 0 && name.empty()) name = line.substr(5);
                    else if (line == "NoDisplay=true" || line == "Hidden=true") hidden = true;
                    else if (line.rfind("Type=", 0) == 0) app = line == "Type=Application";
                }
                if (name.empty() || hidden || !app || !seen.insert(name).second) continue;
                struct stat st {};
                apps.emplace_back(name, stat(e.path().c_str(), &st) == 0 ? int64_t(st.st_mtime) : 0);
            }
        }
        std::sort(apps.begin(), apps.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (apps.empty()) {
            res.body = "I can't find the desktop's list of installed apps: no .desktop entries to read.";
        } else {
            std::vector<std::string> newest;
            for (std::size_t i = 0; i < apps.size() && i < 8; ++i) newest.push_back(apps[i].first + " (" + when_text(apps[i].second) + ")");
            res.body = std::to_string(apps.size()) + " apps are installed, by the desktop's own entries. The newest: " + unllm::join_list(newest) + ".";
            res.notes.push_back("Command-line programs and libraries aren't counted; ask about one by name and I'll check it.");
        }
        res.branch = "records:apps-installed";
        return res;
    }
    if (kind == "startup.list.now") {
        // Enabled now: system services, your own services, and programs your desktop starts at login.
        auto units = [](const std::string& scope) {
            std::vector<std::string> out;
            std::istringstream in(run_out("systemctl " + scope + " list-unit-files --state=enabled --type=service --no-legend --no-pager 2>/dev/null"));
            for (std::string line; std::getline(in, line);) {
                std::istringstream ws(line);
                std::string u;
                if (ws >> u && u.find('@') == std::string::npos) out.push_back(u.substr(0, u.rfind('.')));
            }
            return out;
        };
        const auto sys = units(""), mine = units("--user");
        std::vector<std::string> auto_start;
        const char* h = getenv("HOME");
        for (const std::string dir : {std::string(h ? h : "") + "/.config/autostart", std::string("/etc/xdg/autostart")})
            for (const auto& f : list_dir(dir))
                if (f.size() > 8 && f.compare(f.size() - 8, 8, ".desktop") == 0) auto_start.push_back(f.substr(0, f.size() - 8));
        auto first = [](std::vector<std::string> v, std::size_t n) {
            if (v.size() > n) {
                const std::size_t more = v.size() - n;
                v.resize(n);
                v.push_back(std::to_string(more) + " more");
            }
            return unllm::join_list(v);
        };
        res.body = std::to_string(sys.size()) + " system services start at boot (" + first(sys, 6) + ").";
        if (!mine.empty()) res.body += " At login, " + std::to_string(mine.size()) + " of your own services start: " + first(mine, 8) + ".";
        if (!auto_start.empty()) res.body += " The desktop also starts " + std::to_string(auto_start.size()) + " autostart entries: " + first(auto_start, 6) + ".";
        res.branch = "records:startup-now";
        return res;
    }
    if (kind == "network.inbound.window") {
        // The firewall's kernel log lines: "[UFW BLOCK] IN=wlan0 ... SRC=a DST=b ... PROTO=x ... DPT=n".
        const std::string range = " --since=@" + std::to_string(from) + " --until=@" + std::to_string(to);
        auto lines = journal("-k --grep='UFW BLOCK'" + range);
        if (lines.empty()) {
            res.body = "The firewall blocked nothing coming in " + span + ".";
            res.branch = "records:inbound-none";
            return res;
        }
        auto val = [](const std::string& m, const char* key) {
            const auto at = m.find(std::string(" ") + key + "=");
            if (at == std::string::npos) return std::string();
            const auto b = at + std::strlen(key) + 2;
            return m.substr(b, m.find(' ', b) - b);
        };
        std::map<std::string, int> chatter, aimed;
        int pings = 0, chat = 0;
        std::map<std::string, std::string> port;
        for (const auto& l : lines) {
            const std::string m = jfield(l, "MESSAGE"), src = val(m, "SRC"), dst = val(m, "DST");
            // Multicast and broadcast go to everyone on the network, not to this machine.
            const bool group = dst.rfind("224.", 0) == 0 || dst.rfind("239.", 0) == 0 || dst.rfind("ff0", 0) == 0 || dst == "255.255.255.255" ||
                               (dst.size() > 4 && dst.compare(dst.size() - 4, 4, ".255") == 0);
            if (group) {
                ++chat;
                ++chatter[src];
                continue;
            }
            ++aimed[src];
            if (val(m, "PROTO") == "ICMP") ++pings;
            if (const std::string p = val(m, "DPT"); !p.empty()) port[src] = p;
        }
        auto top = [](const std::map<std::string, int>& m, std::size_t n, const std::map<std::string, std::string>* ports) {
            std::vector<std::pair<int, std::string>> v;
            for (const auto& [k, c] : m) v.emplace_back(c, k);
            std::sort(v.rbegin(), v.rend());
            std::vector<std::string> out;
            for (std::size_t i = 0; i < v.size() && i < n; ++i) {
                std::string x = v[i].second + " (" + std::to_string(v[i].first) + ")";
                if (ports)
                    if (auto it = ports->find(v[i].second); it != ports->end()) x = v[i].second + " (" + std::to_string(v[i].first) + ", port " + it->second + ")";
                out.push_back(x);
            }
            return unllm::join_list(out);
        };
        const int at_me = int(lines.size()) - chat;
        res.body = "The firewall blocked " + std::to_string(lines.size()) + " incoming packets " + span + ".";
        if (chat) res.body += " " + std::to_string(chat) + " were broadcasts to the whole network, not aimed at this machine, mostly from " + top(chatter, 2, nullptr) + ".";
        res.body += at_me ? " Aimed at this machine: " + std::to_string(at_me) + ", from " + top(aimed, 3, &port) + "." : " None was aimed at this machine.";
        res.body += pings ? " " + std::to_string(pings) + " of those were pings." : " None was a ping.";
        res.branch = "records:inbound";
        return res;
    }
    if (kind == "disk.health.now") {
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr sn = sampler_->sample();
        // Kernel warnings that name a drive (its device or its controller, nvme0 for nvme0n1) or an I/O error.
        auto warn = journal("-k -p warning --since=@" + std::to_string(now - 30 * 86400));
        std::vector<std::string> said;
        int errors = 0;
        std::string latest;
        for (const auto& d : sn->disks) {
            if (d.kind == "removable") continue;
            const std::string ctrl = d.name.rfind("nvme", 0) == 0 ? d.name.substr(0, d.name.rfind('n')) : d.name;
            for (const auto& l : warn) {
                const std::string m = jfield(l, "MESSAGE");
                if (m.find(ctrl + ":") != std::string::npos || m.find(ctrl + " ") != std::string::npos ||
                    (m.find("I/O error") != std::string::npos && m.find(d.name) != std::string::npos)) {
                    ++errors;
                    latest = m;
                }
            }
            said.push_back(d.name + (d.temp_c.ok() ? " at " + fmt("%.0f", d.temp_c.get()) + " C" : ""));
        }
        res.body = errors ? "Worth checking: the kernel logged " + std::to_string(errors) + " warnings about the drives in the last 30 days, the latest: \"" +
                                latest.substr(0, 120) + "\"."
                          : "No sign of a failing drive in the records: the kernel logged no drive errors in the last 30 days (" + unllm::join_list(said) + ").";
        res.notes.push_back("The drives' own wear and health counters (SMART) need root to read, which I don't have: sudo smartctl -a /dev/" +
                            (sn->disks.empty() ? std::string("nvme0n1") : sn->disks[0].name) + " shows them.");
        res.branch = "records:disk-health";
        return res;
    }
    if (kind == "identity.bios.flash") {
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr sn = sampler_->sample();
        const SystemId id = system_identity();
        auto dmi = [](const char* f) { return trim(read_line(std::string("/sys/class/dmi/id/") + f).value_or("")); };
        const auto& pw = sn->power;
        res.body = "The BIOS is " + dmi("bios_version") + " from " + dmi("bios_vendor") + ", dated " + dmi("bios_date") + ".";
        if (pw.battery_pct.ok()) {
            const bool ac = pw.on_ac.or_else(false), full = pw.battery_pct.get() >= 50;
            res.body += " A firmware flash needs the charger in and a well-charged battery: it is " + std::string(ac ? "plugged in" : "on battery") +
                        " at " + fmt("%.0f", pw.battery_pct.get()) + "%, so " + (ac && full ? "the power side is fine." : "plug it in and charge it first.");
        }
        res.body += " I can't check the update file itself: take it from the maker's support page for the " + id.product +
                    ", and don't interrupt the flash once it starts.";
        res.branch = "records:bios-flash";
        return res;
    }
    if (kind.rfind("identity.", 0) == 0 || kind == "battery.identity.health" || kind == "battery.value.now.remaining") {
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr sn = sampler_->sample();
        const SystemId id = system_identity();
        auto dmi = [](const char* f) { return trim(read_line(std::string("/sys/class/dmi/id/") + f).value_or("")); };
        res.branch = "records:" + kind;
        if (kind == "identity.cpu") {
            res.body = "The CPU is " + (sn->cpu.model.empty() ? std::string("of an unknown model") : sn->cpu.model) + ", with " +
                       unllm::number_word(sn->cpu.physical) + " cores and " + unllm::number_word(sn->cpu.logical) + " threads" +
                       (sn->cpu.freq_mhz.ok() ? ", running at " + fmt("%.0f", sn->cpu.freq_mhz.get()) + " MHz now" : "") + ".";
        } else if (kind == "identity.gpu") {
            std::vector<std::string> g;
            for (const auto& x : sn->gpus)
                g.push_back(x.name + (x.mem_total.ok() ? " with " + human_bytes(x.mem_total.get()) + " of video memory" : "") +
                            (x.driver.empty() ? "" : " (driver " + x.driver + ")"));
            res.body = g.empty() ? "I can't see a graphics card." : (g.size() == 1 ? "The GPU is " : "It has two GPUs: ") + unllm::join_list(g) + ".";
            if (g.size() > 2) res.body = "It has " + std::to_string(g.size()) + " GPUs: " + unllm::join_list(g) + ".";
        } else if (kind == "identity.list") {
            // The whole list the kind names, each part as its own kind says it: the model, the CPU, the GPUs, the
            // memory, the disks and the battery.
            for (const char* k : {"identity.os", "identity.cpu", "identity.gpu", "identity.ram", "identity.disk", "battery.identity.health"}) {
                std::string a = compose_answer(answer_records(k, r, now));
                if (std::string(k) == "battery.identity.health") a = a.substr(0, a.find(". ") + 1);  // its capacity, not its charge now
                res.body += (res.body.empty() ? "" : " ") + a;
            }
        } else if (kind == "identity.ram") {
            const std::string sticks = memory_sticks("/run/udev/data/+dmi:id");
            res.body = "It has " + human_bytes(double(sn->mem.total)) + " of memory" +
                       (sticks.empty() ? std::string() : ": " + sticks + ".") + " " + human_bytes(double(sn->mem.available)) +
                       " of it is free right now" + (sn->mem.swap_total ? ", plus " + human_bytes(double(sn->mem.swap_total)) + " of swap" : "") + ".";
            if (sticks.empty()) res.notes.push_back("The memory's sticks and speed can't be read without root here: sudo dmidecode -t memory shows them.");
        } else if (kind == "identity.disk") {
            std::vector<std::string> d;
            for (const auto& x : sn->disks)
                if (x.kind != "removable")
                    d.push_back((x.model.empty() ? x.name : trim(x.model)) + ", " + human_bytes(double(x.size)) + (x.kind.empty() ? "" : " " + x.kind) +
                                " (" + x.name + ")");
            res.body = d.empty() ? "I can't see a disk." : std::string(d.size() == 1 ? "The disk is " : "The disks are ") + unllm::join_list(d) + ".";
            // "What filesystem is mounted at /": the mount table's own entry.
            if (FILE* mt = setmntent("/proc/mounts", "r")) {
                while (mntent* e = getmntent(mt))
                    if (std::string(e->mnt_dir) == "/") {
                        res.body += " / is " + std::string(e->mnt_type) + " on " + e->mnt_fsname + ".";
                        break;
                    }
                endmntent(mt);
            }
        } else if (kind == "identity.kernel") {
            res.body = "It runs kernel " + id.kernel + " (" + id.arch + ") on " + id.os_pretty + ".";
            // A question about its changes ("kernel version change history", "when was the kernel last updated"):
            // its package's history too.
            bool changes = r.wants == unllm::Wants::Time;
            for (Intent i : r.intents) changes = changes || i == Intent::Update || i == Intent::Config;
            if (changes) res.body += kernel_history(db_, id.kernel, now);
        } else if (kind == "identity.os") {
            const std::string ver = dmi("product_version"), board = dmi("board_name");
            res.body = "This is " + unllm::indefinite(id.vendor + " " + id.product) + (ver.empty() ? "" : " (" + ver + ")") +
                       (board.empty() ? "" : ", board " + board) + ", running " + id.os_pretty + " on kernel " + id.kernel + ".";
            if (const std::string bv = dmi("bios_version"); !bv.empty())
                res.body += " Its BIOS is " + bv + " from " + dmi("bios_vendor") + ", dated " + dmi("bios_date") + ".";
        } else {
            // The battery: its own numbers from sysfs, in energy or in charge, whichever it reports.
            std::string base;
            for (const auto& f : list_dir("/sys/class/power_supply"))
                if (read_line("/sys/class/power_supply/" + f + "/type").value_or("") == "Battery") base = "/sys/class/power_supply/" + f;
            const auto& pw = sn->power;
            if (base.empty() || !pw.battery_pct.ok()) {
                res.body = "This machine has no battery I can read.";
            } else if (kind == "battery.identity.health") {
                auto full = read_int(base + "/energy_full"), design = read_int(base + "/energy_full_design");
                std::string unit = " Wh";
                if (!full || !design) {
                    full = read_int(base + "/charge_full");
                    design = read_int(base + "/charge_full_design");
                    unit = " Ah";
                }
                if (full && design && *design > 0) {
                    res.body = "The battery holds " + fmt("%.0f", 100.0 * double(*full) / double(*design)) + "% of its design capacity: " +
                               fmt("%.2f", double(*full) / 1e6) + unit + " of " + fmt("%.2f", double(*design) / 1e6) + unit + ", so it has lost " +
                               fmt("%.0f", 100.0 - 100.0 * double(*full) / double(*design)) + "%.";
                    const auto cycles = read_int(base + "/cycle_count");
                    res.body += cycles && *cycles > 0 ? " It has been through " + std::to_string(*cycles) + " charge cycles."
                                                      : " It doesn't report a cycle count.";
                } else {
                    res.body = "The battery doesn't report its design capacity, so I can't say how worn it is.";
                }
                res.body += " It is at " + fmt("%.0f", pw.battery_pct.get()) + "% now (" + pw.battery_state + ").";
            } else {
                const bool draining = pw.battery_state == "Discharging";
                res.body = "The battery is at " + fmt("%.0f", pw.battery_pct.get()) + "%";
                if (pw.battery_minutes.ok())
                    res.body += draining ? ", with about " + duration_text(int64_t(pw.battery_minutes.get() * 60)) + " left at the present draw" +
                                               (pw.battery_w.ok() ? " of " + fmt("%.1f", pw.battery_w.get()) + " W" : "")
                                         : " and charging, full in about " + duration_text(int64_t(pw.battery_minutes.get() * 60));
                else
                    res.body += draining ? ", but it doesn't report its draw, so I can't time it" : " and plugged in (" + pw.battery_state + "), so it isn't running down";
                res.body += ".";
                // On the charger, how fast it drains is told by the last time it ran on its own: the levels the log and the
                // readings give for that spell's ends.
                if (!draining) {
                    const auto ps = plugs(db_, now - 30 * 86400, now);
                    for (std::size_t i = ps.size(); i-- > 1;)
                        if (ps[i].in && !ps[i - 1].in && ps[i - 1].level > ps[i].level && ps[i].level >= 0 && ps[i].t - ps[i - 1].t >= 600) {
                            res.body += " The last time it ran on its own battery, " + run_span(ps[i - 1].t, ps[i].t) + ", it went from " +
                                        fmt("%.0f", ps[i - 1].level) + "% to " + fmt("%.0f", ps[i].level) + "%, about " +
                                        fmt("%.0f", (ps[i - 1].level - ps[i].level) * 3600 / double(ps[i].t - ps[i - 1].t)) + "% an hour.";
                            break;
                        }
                }
            }
        }
        return res;
    }

    if (kind == "memory.oom.list.window") {
        const int64_t f = r.window.stated ? from : now - 30 * 86400;
        const std::string where = r.window.stated ? span : "in the last 30 days";
        if (query(db_, "SELECT 1 FROM log WHERE t >= ? AND t < ? LIMIT 1", {f, to}).empty()) {
            // Nothing logged then: systemd's notes in the journal on each unit the out-of-memory killer emptied.
            std::size_t total = 0;
            const auto named = oom_kills(db_, f, to, 4, &total);
            if (named.empty()) {
                res.body = "Nothing was killed for lack of memory " + where + ".";
                res.branch = "records:oom-none";
                return res;
            }
            res.body = std::to_string(total) + (total == 1 ? " out-of-memory kill " : " out-of-memory kills ") + where +
                       (total > named.size() ? ". The latest: " : ": ") + unllm::join_list(named) + ".";
            res.branch = "records:oom";
            return res;
        }
        // The kills the log holds; a program named keeps those that killed it: the process the kernel killed, or a
        // process of the emptied unit at that moment.
        std::string program;
        for (const auto& e : r.entities)
            if (e.type == "program" && program.empty()) program = e.value;
        auto count = [&](const std::string& sql, const std::vector<int64_t>& ints, const std::vector<std::string>& texts) {
            auto n = query(db_, sql, ints, texts);
            return n.empty() ? int64_t(0) : n[0].t;
        };
        auto kills = recorded_kills(db_, f, to);
        const std::size_t all = kills.size();
        if (!program.empty())
            kills.erase(std::remove_if(kills.begin(), kills.end(), [&](const Kill& k) {
                            if (!k.process.empty() && is_program(program, k.process, "", "")) return false;
                            return k.unit.empty() || count("SELECT COUNT(*) FROM procs WHERE started <= ? AND IFNULL(ended, ?) >= ? AND unit = ? "
                                                           "AND is_program(?, name, exe, cmdline)", {k.t, k.t, k.t - 15}, {k.unit, program}) == 0;
                        }), kills.end());
        if (kills.empty()) {
            res.body = program.empty() ? "Nothing was killed for lack of memory " + where + "."
                                       : "Nothing running " + program + " was killed for lack of memory " + where +
                                             (all ? " (" + std::to_string(all) + (all == 1 ? " other kill was" : " other kills were") + " recorded there)." : ".");
            res.branch = "records:oom-none";
            return res;
        }
        // Each kill: who did it, the unit and what started it (the parent of its first process), what its processes
        // held in the five minutes before, the biggest of them by command line with their peaks, and memory and
        // swap at that moment.
        auto describe = [&](const Kill& k) {
            std::string text = "At " + when_text(k.t) + " ";
            if (!k.process.empty())
                text += "the kernel's out-of-memory killer killed " + k.process + " (PID " + std::to_string(k.pid) + "), which held " + human_bytes(k.rss) +
                        " of memory (" + human_bytes(k.vm) + " virtual)" + (k.unit.empty() ? "" : ", in " + k.unit);
            else
                text += "systemd-oomd killed " + k.unit;
            if (!k.unit.empty()) {
                auto root = query(db_, "SELECT CAST(started AS INTEGER), 0, 0, name, parent FROM procs WHERE started <= ? AND unit = ? ORDER BY started LIMIT 1",
                                  {k.t}, {k.unit});
                if (!root.empty() && !root[0].s2.empty() && root[0].s2 != "systemd") text += ", which " + root[0].s2 + " started (" + root[0].s1 + " ran in it)";
                auto held = query(db_, "SELECT COUNT(*), SUM(m) FROM (SELECT MAX(peak_rss) m FROM minute_top WHERE minute BETWEEN ? AND ? AND unit = ? GROUP BY pid)",
                                  {k.t - 300, k.t}, {k.unit});
                auto procs = query(db_, "SELECT 0, peak_rss, 0, cmdline, name FROM procs WHERE started <= ? AND IFNULL(ended, ?) >= ? AND unit = ? "
                                        "ORDER BY peak_rss DESC", {k.t, k.t, k.t - 15}, {k.unit});
                std::vector<std::pair<std::string, std::vector<double>>> by;  // command -> peaks, biggest first
                for (const auto& p : procs) {
                    const std::string cmd = command_text(p.s1.empty() ? p.s2 : p.s1);
                    auto it = std::find_if(by.begin(), by.end(), [&](const auto& b) { return b.first == cmd; });
                    if (it == by.end()) by.push_back({cmd, {p.a}});
                    else it->second.push_back(p.a);
                }
                std::vector<std::string> biggest;
                for (std::size_t i = 0; i < by.size() && i < 4 && (i == 0 || by[i].second[0] >= 50e6); ++i) {
                    std::vector<std::string> peaks;
                    for (std::size_t j = 0; j < by[i].second.size() && j < 3; ++j) peaks.push_back(human_bytes(by[i].second[j]));
                    biggest.push_back(by[i].first + (by[i].second.size() == 1 ? " (" + peaks[0] + " at its peak)"
                                                                               : " (" + unllm::number_word(int64_t(by[i].second.size())) + ": " +
                                                                                     unllm::join_list(peaks) + ")"));
                }
                if (!procs.empty())
                    text += ": " + std::to_string(procs.size()) + (procs.size() == 1 ? " process" : " processes") +
                            (!held.empty() && held[0].a > 0 ? " that held about " + human_bytes(held[0].a) + " in the five minutes before" : "") +
                            "; the biggest " + (biggest.size() == 1 ? "was " : "were ") + unllm::join_list(biggest);
            }
            auto at = query(db_, "SELECT CAST(t AS INTEGER), mem_pct, swap_used, swap_total FROM samples WHERE t BETWEEN ? AND ? ORDER BY t DESC LIMIT 1",
                            {k.t - 60, k.t});
            if (at.empty())
                at = query(db_, "SELECT CAST(t AS INTEGER), mem_pct, swap_used FROM samples WHERE t BETWEEN ? AND ? ORDER BY t DESC LIMIT 1", {k.t - 60, k.t});
            if (!at.empty())
                text += ". Memory was at " + fmt("%.0f", at[0].a) + "% and swap at " + human_bytes(at[0].b) +
                        (std::atof(at[0].s1.c_str()) > 0 ? " of " + human_bytes(std::atof(at[0].s1.c_str())) : "");
            return text + ".";
        };
        res.body = std::to_string(kills.size()) + (kills.size() == 1 ? " out-of-memory kill" : " out-of-memory kills") +
                   (program.empty() ? "" : " of " + program) + " " + where + (kills.size() > 4 ? ". The latest four:" : ".");
        for (std::size_t i = 0; i < kills.size() && i < 4; ++i) res.body += " " + describe(kills[kills.size() - 1 - i]);
        res.branch = "records:oom";
        return res;
    }
    if (kind == "story.often.window.process") {
        // How long a program was open: its processes overlapping the window merged into runs, each cut to the
        // window and added up; the longest run whole, what it had open, and the sleep inside it.
        std::string program;
        for (const auto& e : r.entities)
            if (e.type == "program" && program.empty()) program = e.value;
        if (program.empty()) {
            res.body = "Which program do you mean? For example: \"how long was firefox open yesterday\".";
            res.branch = "records:open-noprogram";
            res.refused = true;
            return res;
        }
        std::string who = program;
        who[0] = char(std::toupper(static_cast<unsigned char>(who[0])));
        struct Run {
            int64_t from, to;
            double cpu;
            std::string cmdline;
        };
        std::vector<Run> runs;
        for (const auto& p : query(db_, "SELECT CAST(started AS INTEGER), IFNULL(ended, ?), cpu_seconds, cmdline FROM procs WHERE started < ? "
                                        "AND IFNULL(ended, ?) >= ? AND is_program(?, name, exe, cmdline) ORDER BY started",
                                   {now, to, now, from}, {program})) {
            if (!runs.empty() && p.t <= runs.back().to) {
                runs.back().to = std::max(runs.back().to, int64_t(p.a));
                runs.back().cpu += p.b;
            } else {
                runs.push_back({p.t, int64_t(p.a), p.b, p.s1});
            }
        }
        if (runs.empty()) {
            res.body = who + " wasn't open " + span + ".";
            res.branch = "records:open-none";
            return res;
        }
        int64_t total = 0;
        const Run* longest = &runs[0];
        for (const auto& x : runs) {
            total += std::min(x.to, to) - std::max(x.from, from);
            if (x.to - x.from > longest->to - longest->from) longest = &x;
        }
        const std::string opened = opened_text(longest->cmdline);
        res.body = who + " was open for " + duration_text(total) + " " + span + ", in " +
                   (runs.size() == 1 ? std::string("one run") : unllm::number_word(int64_t(runs.size())) + " separate runs") +
                   (opened.empty() ? "" : " (" + opened + ")") + (runs.size() == 1 ? ": " : "; the longest ") + "from " +
                   when_text(longest->from) + " to " + when_text(longest->to) +
                   (longest->from < from || longest->to > to ? ", " + duration_text(longest->to - longest->from) + " in all" : "") + ".";
        // The laptop asleep inside the longest run: open on the clock, not in use.
        int64_t asleep = 0;
        std::vector<std::string> spans;
        for (const auto& [a, b] : sleeps(db_, longest->from - 86400, longest->to)) {
            const int64_t in = std::min(b, longest->to) - std::max(a, longest->from);
            if (in <= 0) continue;
            asleep += in;
            spans.push_back(span_text(std::max(a, longest->from), std::min(b, longest->to), " to "));
        }
        if (asleep > 0)
            res.body += " The laptop slept from " + unllm::join_list(spans) + " during that run, so it was open about " +
                        duration_text(longest->to - longest->from - asleep) + " while awake.";
        if (longest->cpu >= 1) res.body += " It used " + fmt("%.0f", longest->cpu) + " s of CPU in that run.";
        res.branch = "records:open";
        return res;
    }
    if (kind == "diagnose.logs.list.window") {
        // Error-level journal lines in the window, by who wrote them.
        const std::string range = " --since=@" + std::to_string(from) + " --until=@" + std::to_string(to);
        auto lines = journal("-p err -n 20000" + range);
        if (lines.empty()) {
            res.body = "The system log has no errors " + span + ".";
            res.branch = "records:logs-none";
            return res;
        }
        std::map<std::string, std::pair<int, std::string>> by;
        for (const auto& l : lines) {
            std::string who = jfield(l, "SYSLOG_IDENTIFIER");
            if (who.empty()) who = jfield(l, "_COMM");
            auto& e = by[who.empty() ? "unknown" : who];
            ++e.first;
            e.second = jfield(l, "MESSAGE");
        }
        std::vector<std::pair<int, std::string>> order;
        for (const auto& [who, e] : by) order.emplace_back(e.first, who);
        std::sort(order.rbegin(), order.rend());
        std::vector<std::string> top;
        for (std::size_t i = 0; i < order.size() && i < 3; ++i) {
            std::string m = by[order[i].second].second;
            if (m.size() > 90) m = m.substr(0, 87) + "...";
            top.push_back(order[i].second + " " + std::to_string(order[i].first) + " (latest: \"" + m + "\")");
        }
        res.body = std::to_string(lines.size()) + (lines.size() == 1 ? " error line " : " error lines ") + "in the system log " + span +
                   ". Most came from " + unllm::join_list(top) + ".";
        if (order.size() > 3) res.body += " " + std::to_string(order.size() - 3) + " other sources wrote the rest.";
        res.branch = "records:logs";
        return res;
    }
    if (kind == "crashes.yesno.window.system") {
        // ActivityScope's own shutdown events, then any boot in the journal that ended without systemd's
        // power-off notice and that no event covers: both mean the machine stopped without shutting down.
        const auto b = boots();
        auto evs = query(db_, "SELECT CAST(start AS INTEGER), 0, 0, summary FROM events WHERE (kinds & ?) != 0 AND start >= ? AND start < ? ORDER BY start",
                         {int64_t(KindShutdown), r.window.stated ? from : 0, to});
        std::vector<std::pair<int64_t, std::string>> stops;
        for (const auto& e : evs) stops.emplace_back(e.t, e.s1);
        const int64_t f = r.window.stated ? from : 0;
        for (std::size_t i = 0; i + 1 < b.size(); ++i) {
            if (b[i].second < f || b[i].second >= to) continue;
            bool covered = false;
            for (const auto& e : evs) covered = covered || std::llabs(e.t - b[i].second) < 600;
            if (covered || !journal("-n 1 _BOOT_ID=" + b[i].id + " MESSAGE_ID=98268866d1d54a499c4e98921d93bc40").empty()) continue;
            stops.emplace_back(b[i].second, "");
        }
        std::sort(stops.begin(), stops.end());
        const std::string where = r.window.stated ? span : "in the records";
        if (stops.empty()) {
            if (polar) settle(res, r, false);
            else res.lead = "No.";
            res.body = "The machine didn't stop without a clean shutdown " + where + "." +
                       (b.empty() ? "" : " It has been up since " + when_text(b.back().first) + ".");
            // A dark screen without a crash is usually the session: sleep, the lid, the lock screen.
            if (r.window.stated) {
                auto sess = query(db_, "SELECT t, 0, 0, title FROM log WHERE category = 'session' AND t >= ? AND t < ? ORDER BY t DESC LIMIT 4", {from, to});
                std::vector<std::string> said;
                for (auto it = sess.rbegin(); it != sess.rend(); ++it) said.push_back(it->s1 + " at " + when_text(it->t));
                if (!said.empty()) res.body += " The session log has " + unllm::join_list(said) + ".";
            }
            res.branch = "records:unclean-none";
            return res;
        }
        std::vector<std::string> times;
        for (const auto& x : stops) times.push_back(when_text(x.first));
        // A yes/no question is answered with a yes; "how many times" with the count and "why" with the stops, no yes.
        if (polar) settle(res, r, true);
        res.body = "The machine stopped without a clean shutdown " +
                   (stops.size() == 1 ? std::string("once") : std::to_string(stops.size()) + " times") +
                   " " + where + ": " + unllm::join_list(times) + ".";
        // The latest one's explanation, from its event: the text after the "HH:MM:SS to HH:MM:SS (1 s). " lead. Its
        // readings and programs, as the recorder wrote them when it next started, are told again from the records
        // (stop_story): the recorder's own last reading can come after the log's, and its hour holds the rest.
        std::string why = stops.back().second;
        if (const auto at = why.find("). "); at != std::string::npos && at < 40) why = why.substr(at + 3);
        if (!why.empty()) {
            if (const std::string told = stop_story(db_, stops.back().first); !told.empty()) {
                for (const char* own : {" The last reading", " The recorder read on", " In its last two minutes", " The busiest program"})
                    if (const auto at = why.find(own); at != std::string::npos) why.erase(at);
                why += " " + told;
            }
            res.body += " The latest: " + why;
        }
        else res.notes.push_back("The log doesn't say whether it hung, lost power or crashed.");
        res.branch = "records:unclean";
        return res;
    }

    if (kind == "network.wifi.value.now" || kind == "network.wifi.when.window") {
        const std::string dev = wifi_iface();
        if (dev.empty()) {
            res.body = "This machine has no Wi-Fi adapter I can see.";
            res.branch = "records:wifi-none";
            return res;
        }
        const bool now_kind = kind == "network.wifi.value.now";
        const int64_t f = now_kind ? now - 86400 : r.window.stated ? from : now - 7 * 86400;
        const std::string where = now_kind ? "in the last day" : r.window.stated ? span : "in the last week";
        const auto drops = wifi_drops(f, now_kind ? now : to);
        std::string history;
        if (drops.empty()) {
            history = "The Wi-Fi hasn't dropped " + where + " (disconnects for sleep aside).";
        } else {
            std::vector<std::string> d;
            for (auto it = drops.rbegin(); it != drops.rend() && d.size() < 5; ++it)
                d.push_back(when_text(it->from) + (it->to ? " for " + duration_text(double(it->to - it->from)) : ", still down"));
            history = "The Wi-Fi dropped " + (drops.size() == 1 ? std::string("once") : std::to_string(drops.size()) + " times") + " " + where +
                      (drops.size() > d.size() ? ", the latest " : ": ") + unllm::join_list(d) + " (disconnects for sleep aside).";
        }
        if (now_kind) {
            // iw's link report: the network, the signal, the negotiated rates.
            const WifiLink w = wifi_link(dev);
            const std::string &ssid = w.ssid, &signal = w.signal, &rx = w.rx, &tx = w.tx, &freq = w.freq;
            if (ssid.empty()) {
                res.body = "The Wi-Fi (" + dev + ") isn't connected to a network right now. " + history;
            } else {
                const char* grade = w.grade;
                res.body = "The Wi-Fi is connected to \"" + ssid + "\"" + (freq.empty() ? "" : std::atof(freq.c_str()) > 4000 ? " on 5 GHz" : " on 2.4 GHz") +
                           (signal.empty() ? "" : ", with a " + std::string(grade) + " signal (" + signal + ")") +
                           (rx.empty() ? "" : ". The link runs at " + rx + " Mbit/s down" + (tx.empty() ? "" : " and " + tx + " Mbit/s up") +
                                                 ", the radio's rate, not your internet speed") + ". " + history;
            }
            res.branch = "records:wifi-now";
        } else {
            res.body = history;
            res.branch = "records:wifi-drops";
        }
        return res;
    }
    if (kind == "config.list.window" || kind == "config.when.window") {
        // hindsight's snapshots: one git commit each, the config tree as it was then.
        const char* h = getenv("HOME");
        const std::string repo = std::string(h ? h : "") + "/.local/share/hindsight/repo";
        struct Snap { int64_t t; std::string id; };
        std::vector<Snap> snaps;  // newest first, none taken after the clock
        {
            std::istringstream in(run_out("git -C '" + repo + "' log --format='%ct %H' 2>/dev/null"));
            for (std::string line; std::getline(in, line);)
                if (const auto sp = line.find(' '); sp != std::string::npos && std::atoll(line.c_str()) <= now)
                    snaps.push_back({std::atoll(line.c_str()), line.substr(sp + 1)});
        }
        // "When did X change" reads the startup and admin log as well, so it needs no snapshots.
        if (snaps.size() < 2 && kind == "config.list.window") {
            res.body = "hindsight has fewer than two config snapshots, so there is nothing to compare yet.";
            res.branch = "records:config-none";
            return res;
        }
        // A file path as the user knows it: etc/x is /etc/x; home/config/x is ~/.config/x; "__" stands for "/".
        auto shown = [](std::string f) {
            for (std::size_t at; (at = f.find("__")) != std::string::npos;) f.replace(at, 2, "/");
            if (f.rfind("etc/", 0) == 0) return "/" + f;
            if (f.rfind("home/config/", 0) == 0) return "~/.config/" + f.substr(12);
            if (f.rfind("home/", 0) == 0) return "~/" + f.substr(5);
            return f;
        };
        // The files the question names: the unit or program its everyday words resolved to (names.h, "the cpu power
        // cap" is cpu-power-limits.service), else the words that may name something ("sysctl"; not "any", nor the
        // kinds' own "file").
        std::vector<std::string> names;
        if (!named_.empty())
            for (const std::string& n : {named_[0].program.unit, named_[0].program.script, named_[0].program.name})
                if (!n.empty() && n.find_first_of("*(") == std::string::npos) names.push_back(n);
        if (names.empty()) names = named_words(r, kind);
        // A file is named by its name or by a run of the parts its name splits into at dots, dashes and underscores,
        // the last one maybe with an ending ("sysctl" names 99-sysctl.conf, "setting" settings.json, and
        // cpu-power-limits.service names 99-cpu-power-limits.rules); a piece of a part never does ("set").
        auto parts = [](const std::string& s) {
            std::vector<std::string> out(1);
            for (char c : s) {
                if (std::isalnum(static_cast<unsigned char>(c))) out.back() += char(std::tolower(static_cast<unsigned char>(c)));
                else if (!out.back().empty()) out.emplace_back();
            }
            if (out.back().empty()) out.pop_back();
            return out;
        };
        auto names_file = [&](const std::string& n, const std::string& path) {
            const auto want = parts(n.rfind('.') != std::string::npos && n.rfind('.') > 0 ? n.substr(0, n.rfind('.')) : n);
            const auto have = parts(path.substr(path.rfind('/') + 1));
            for (std::size_t i = 0; !want.empty() && i + want.size() <= have.size(); ++i) {
                bool same = true;
                for (std::size_t j = 0; j < want.size() && same; ++j) {
                    const std::string& a = want[j];
                    const std::string& b = have[i + j];
                    same = a == b || (j + 1 == want.size() && b.size() > a.size() && b.size() - a.size() <= 2 && b.compare(0, a.size(), a) == 0);
                }
                if (same) return true;
            }
            return false;
        };
        auto matches = [&](const std::string& f) {
            for (const auto& n : names)
                if (names_file(n, shown(f))) return true;
            return false;
        };
        // Each snapshot's changed files: git log --name-only, newest first.
        struct Change { int64_t t; std::vector<std::string> files; };
        auto changes = [&](const std::string& range) {
            std::vector<Change> out;
            std::istringstream in(run_out("git -C '" + repo + "' log --format=@%ct --name-only " + range + " 2>/dev/null"));
            for (std::string line; std::getline(in, line);) {
                if (line.empty()) continue;
                if (line[0] == '@') out.push_back({std::atoll(line.c_str() + 1), {}});
                else if (!out.empty() && line != "etc/.updated") out.back().files.push_back(line);
            }
            return out;
        };
        if (kind == "config.when.window") {
            // hindsight's snapshots that changed a named file, newest first.
            std::vector<std::pair<int64_t, std::string>> seen;
            if (!snaps.empty())
                for (const auto& c : changes(snaps.front().id))
                    for (const auto& f : c.files)
                        if (names.empty() || matches(f)) {
                            seen.push_back({c.t, shown(f)});
                            break;
                        }
            if (names.empty()) {
                res.body = seen.empty() ? "hindsight's snapshots show no config change."
                                        : "The latest config change hindsight saw was " + seen.front().second + ", in the snapshot of " +
                                              moment_text(seen.front().first) + ".";
                res.branch = seen.empty() ? "records:config-when-none" : "records:config-when";
                return res;
            }
            // The startup and admin log rows that name the file: the moments it was installed, enabled and changed,
            // to the second, where the snapshots only show the day after.
            std::vector<Row> logged;
            for (const auto& row : query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title, category FROM log WHERE category IN ('startup', 'admin') "
                                              "AND t <= ? ORDER BY t", {now})) {
                bool hit = false;
                for (const auto w : split_ws(row.s1))
                    for (const auto& n : names)
                        hit = hit || (w.find('/') != std::string_view::npos && names_file(n, std::string(w)));
                if (hit) logged.push_back(row);
            }
            if (logged.empty() && seen.empty()) {
                res.body = "No record shows a change to a file named like " + names[0] + ".";
                res.branch = "records:config-when-none";
                return res;
            }
            // An admin row's title holds the whole command: its head ("Ran as administrator (sudo) by user") says it.
            auto said = [](const Row& row) { return row.s2 == "admin" ? row.s1.substr(0, row.s1.find(": ")) : row.s1; };
            std::string text;
            if (!logged.empty()) {
                std::vector<std::string> first;
                for (const auto& row : logged)
                    if (row.t - logged.front().t <= 60 && first.size() < 3) first.push_back(said(row));
                text = "The records first show " + names[0] + " on " + moment_text(logged.front().t) + ": " + unllm::join_list(first) + ".";
                if (logged.back().t - logged.front().t > 60)
                    text += " The latest change logged was on " + moment_text(logged.back().t) + ": " + said(logged.back()) + ".";
                for (const auto& row : logged)
                    if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + said(row));
            }
            if (!seen.empty())
                text += logged.empty() ? seen.front().second + " last changed in the snapshot of " + moment_text(seen.front().first) + "." +
                                             (seen.size() > 1 ? " hindsight first saw it change in the snapshot of " + moment_text(seen.back().first) + "." : "")
                                       : " hindsight first shows " + seen.back().second + " in its snapshot of " + moment_text(seen.back().first) +
                                             (seen.size() > 1 ? ", the latest change in the snapshot of " + moment_text(seen.front().first) : "") + ".";
            // What it does, as its unit says.
            if (!named_.empty() && !named_[0].program.about.empty()) text += " Its unit describes it as \"" + named_[0].program.about + "\".";
            res.body = text;
            res.branch = "records:config-when";
            return res;
        }
        const int64_t f0 = r.window.stated ? from : now - 7 * 86400;
        const Snap* base = &snaps.back();
        for (const auto& x : snaps)
            if (x.t <= f0) { base = &x; break; }
        const Snap* last = nullptr;
        for (const auto& x : snaps)
            if (x.t <= to) { last = &x; break; }
        const std::string where = r.window.stated ? span : "in the last week";
        if (!last || last->t <= base->t) {
            res.body = "hindsight took no snapshot " + where + ", so it can't see changes there. The latest snapshot is from " + moment_text(snaps.front().t) + ".";
            res.branch = "records:config-nosnap";
            return res;
        }
        std::map<std::string, int> count;  // file -> snapshots it changed in
        for (const auto& c : changes(base->id + ".." + last->id))
            for (const auto& f : c.files) ++count[f];
        std::vector<std::string> sys, mine, hit;
        for (const auto& [f, n] : count) {
            (f.rfind("etc/", 0) == 0 ? sys : mine).push_back(shown(f));
            if (matches(f)) hit.push_back(shown(f));
        }
        auto first = [](std::vector<std::string> v, std::size_t n) {
            if (v.size() > n) {
                const std::size_t more = v.size() - n;
                v.resize(n);
                v.push_back(std::to_string(more) + " more");
            }
            return unllm::join_list(v);
        };
        const std::string between = "between the snapshots of " + moment_text(base->t) + " and " + moment_text(last->t);
        // A yes-or-no question ("did any config files change") gets its yes in front.
        if (!names.empty()) {
            if (polar && !hit.empty()) settle(res, r, true);
            res.body = hit.empty() ? "No " + names[0] + " file changed " + between + "." : first(hit, 6) + " changed " + between + ".";
            if (hit.empty() && !count.empty()) res.body += " " + std::to_string(count.size()) + " other config files did.";
        } else if (count.empty()) {
            res.body = "No config file changed " + between + ".";
        } else {
            // The files the most snapshots changed first; a folder below the snapshots' own roots (/etc, ~/.config, ~)
            // where three or more changed stands for them ("~/.config/weird (4 files)").
            auto listed = [&](bool etc) {
                std::map<std::string, std::vector<std::string>> by_dir;
                for (const auto& [f, n] : count)
                    if ((f.rfind("etc/", 0) == 0) == etc) by_dir[shown(f).substr(0, shown(f).rfind('/'))].push_back(f);
                std::vector<std::pair<int, std::string>> items;
                for (const auto& [d, fs] : by_dir) {
                    if (fs.size() >= 3 && d != "/etc" && d != "~/.config" && d != "~") {
                        int most = 0;
                        for (const auto& f : fs) most = std::max(most, count[f]);
                        items.push_back({most, d + " (" + std::to_string(fs.size()) + " files)"});
                    } else {
                        for (const auto& f : fs) items.push_back({count[f], shown(f)});
                    }
                }
                std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                std::vector<std::string> out;
                for (const auto& x : items) out.push_back(x.second);
                return out;
            };
            if (polar) settle(res, r, true);
            res.body = std::to_string(count.size()) + " config files changed " + between + ":";
            if (!sys.empty()) res.body += " " + std::to_string(sys.size()) + " in /etc (" + first(listed(true), 5) + ")";
            if (!mine.empty()) res.body += std::string(sys.empty() ? " " : " and ") + std::to_string(mine.size()) + " of yours (" + first(listed(false), 8) + ")";
            res.body += ".";
        }
        res.receipts.push_back("hindsight diff \"" + std::to_string((now - base->t) / 86400) + " days ago\" shows the lines");
        res.branch = "records:config";
        return res;
    }
    if (kind == "diagnose.list.window") {
        // Everything recorded in the window, in time order: the timeline's story of it. A question that names an
        // event the window ends at ("before it cut out", "before i shut the lid": the window narrowed to end
        // there), where the records hold something at that moment, first says what the machine was doing right
        // before, then tells the stretch since it last woke or started, with the programs that ran.
        int64_t a = from;
        std::string lead;
        bool ends_at_event = false;
        if (r.window.stated && !r.event_clause.empty() && !r.event_after) {
            ends_at_event = !query(db_, "SELECT 1 FROM events WHERE start BETWEEN ? AND ? OR \"end\" BETWEEN ? AND ? LIMIT 1",
                                   {to - 120, to + 60, to - 120, to + 60}).empty() ||
                            !query(db_, "SELECT 1 FROM log WHERE t BETWEEN ? AND ? AND category IN ('session', 'problems', 'services') LIMIT 1",
                                   {to - 60, to + 60}).empty();
            if (ends_at_event) {
                lead = right_before(db_, to);
                // The last wake (the logbook's "Woke from sleep" row) or start (pid 1's) inside the window.
                for (const auto& x : {query(db_, "SELECT CAST(MAX(t) AS INTEGER) FROM log WHERE category = 'session' AND title = 'Woke from sleep' "
                                                 "AND t >= ? AND t < ?", {from, to}),
                                      query(db_, "SELECT CAST(MAX(started) AS INTEGER) FROM procs WHERE pid = 1 AND started >= ? AND started < ?", {from, to})})
                    if (!x.empty()) a = std::max(a, x[0].t);
            }
        }
        // A named moment ("what was it doing at 3am"): what the machine was doing then comes first, the window's story after.
        if (!ends_at_event && r.window.at > 0) lead = moment_story(db_, r.window.at, now);
        res.body = story_summary(a, to, now, &res.receipts);
        if (res.body.empty()) res.body = lead.empty() ? "Nothing was recorded " + span + "." : "No event or log entry was recorded " + span + ".";
        else if (res.body.rfind("In that window", 0) == 0) {
            res.body.replace(0, 14, a > from ? "What happened since " + when_text(a) + ", when it last woke or started" : span);
            res.body[0] = char(std::toupper(static_cast<unsigned char>(res.body[0])));
        }
        if (ends_at_event) {
            const std::string ran = programs_ran(db_, a, to, now);
            res.body = (lead.empty() ? "" : lead + " ") + res.body + (ran.empty() ? "" : " " + ran);
        } else if (!lead.empty()) {
            res.body = lead + " " + res.body;
        }
        res.branch = "records:story";
        return res;
    }
    if (kind == "network.address.value.now") {
        // ip's one-line form: "3: wlan0    inet 192.168.3.167/24 ..."; global addresses only.
        std::map<std::string, std::vector<std::string>> by;
        std::istringstream in(run_out("ip -o addr show scope global 2>/dev/null"));
        for (std::string line; std::getline(in, line);) {
            std::istringstream ws(line);
            std::string idx, dev, fam, addr;
            if (ws >> idx >> dev >> fam >> addr && (fam == "inet" || fam == "inet6")) by[dev].push_back(addr.substr(0, addr.find('/')));
        }
        std::vector<std::string> parts;
        for (const auto& [dev, a] : by) parts.push_back(dev + " has " + unllm::join_list(a));
        if (parts.empty()) {
            res.body = "No network interface has an address right now, so the machine is offline.";
        } else {
            res.body = unllm::join_list(parts) + ".";
            res.notes.push_back("Those are this machine's own addresses; the public address the internet sees belongs to your router, and I "
                                "can't read it without asking a server online.");
        }
        res.branch = "records:address";
        return res;
    }
    if (kind == "devices.bluetooth.now") {
        const std::string show = run_out("bluetoothctl show 2>/dev/null");
        if (show.empty()) {
            res.body = "I can't reach the Bluetooth service, so it is off or missing.";
            res.branch = "records:bluetooth-none";
            return res;
        }
        // "Device F8:5C:7D:E7:15:E7 JBL Flip Essential 2": the name follows the address.
        auto names = [](const std::string& out) {
            std::vector<std::string> v;
            std::istringstream in(out);
            for (std::string line; std::getline(in, line);)
                if (line.rfind("Device ", 0) == 0 && line.size() > 25) v.push_back(line.substr(25));
            return v;
        };
        const auto on = names(run_out("bluetoothctl devices Connected 2>/dev/null")), paired = names(run_out("bluetoothctl devices Paired 2>/dev/null"));
        std::vector<std::string> idle;
        for (const auto& p : paired)
            if (std::find(on.begin(), on.end(), p) == on.end()) idle.push_back(p);
        const bool powered = show.find("Powered: yes") != std::string::npos;
        res.body = !powered ? "Bluetooth is off, so nothing is connected."
                   : on.empty() ? "Bluetooth is on, but no device is connected."
                                : "Bluetooth is on and connected to " + unllm::join_list(on) + ".";
        if (!idle.empty()) res.body += " Paired but not connected: " + unllm::join_list(idle) + ".";
        res.branch = "records:bluetooth";
        return res;
    }
    if (kind == "devices.mic.now") {
        // PipeWire's recording streams; streams that read a speaker's monitor are not a microphone.
        const std::string out = run_out("pactl -f json list source-outputs 2>/dev/null");
        if (out.empty()) {
            res.body = "I can't reach the sound server to check the microphone.";
            res.branch = "records:mic-none";
            return res;
        }
        std::vector<std::string> apps;
        for (std::size_t at = 0; (at = out.find("\"application.name\":", at)) != std::string::npos; ++at) {
            const std::string name = jfield(out.substr(at, 200), "application.name");
            if (!name.empty() && std::find(apps.begin(), apps.end(), name) == apps.end()) apps.push_back(name);
        }
        res.body = apps.empty() ? "Nothing is recording from a microphone right now."
                                : unllm::join_list(apps) + (apps.size() == 1 ? " is" : " are") + " recording sound right now.";
        res.branch = "records:mic";
        return res;
    }
    if (kind == "gpu.throttle.now" && !r.window.live && !r.window.present) {
        // Throttling is only read live; for the past, the load history is what the records hold.
        res = answer_records("gpu.value.window", r, now);
        res.scope.insert(res.scope.begin(), "I don't keep a record of GPU throttling over time, only its load.");
        return res;
    }
    if (kind == "gpu.throttle.now") {
        // nvidia-smi's clock event reasons: a bit mask, decoded by NVIDIA's documented bits.
        const std::string out = run_out("nvidia-smi --query-gpu=name,clocks_event_reasons.active,temperature.gpu,clocks.sm,clocks.max.sm "
                                        "--format=csv,noheader,nounits 2>/dev/null");
        const auto f = split(trim(out), ',');
        if (f.size() < 5) {
            res.body = "Only an NVIDIA GPU reports why it slows its clocks, and nvidia-smi didn't answer.";
            res.branch = "records:throttle-none";
            return res;
        }
        const unsigned long bits = std::strtoul(trim(f[1]).c_str(), nullptr, 16);
        static const std::pair<unsigned long, const char*> why[] = {
            {0x80, "hardware overheating"}, {0x40, "software heat limit"}, {0x8, "hardware slowdown (heat or power)"},
            {0x100, "power brake"}, {0x4, "power limit"}, {0x2, "a clock setting"}, {0x1, "nothing to do (idle)"}};
        std::vector<std::string> reasons;
        for (const auto& [b, w] : why)
            if (bits & b) reasons.push_back(w);
        const bool real = (bits & ~0x1UL & ~0x200UL) != 0;
        res.lead = real ? "Yes:" : "No:";
        res.body = trim(f[0]) + " runs at " + trim(f[3]) + " of " + trim(f[4]) + " MHz at " + trim(f[2]) + " C" +
                   (reasons.empty() ? ", with nothing holding it back." : ", held back by " + unllm::join_list(reasons) + ".");
        if (!real && (bits & 0x1)) res.body += " Slow clocks while idle are normal, not throttling.";
        res.branch = "records:throttle";
        return res;
    }
    if (kind == "slow.load.value.now") {
        double l1 = 0, l5 = 0, l15 = 0;
        std::ifstream in("/proc/loadavg");
        in >> l1 >> l5 >> l15;
        const double threads = std::max(1u, std::thread::hardware_concurrency());
        const char* grade = l1 < threads * 0.5 ? "light" : l1 < threads ? "busy" : "more work than it has threads for";
        res.body = "The load average is " + fmt("%.2f", l1) + ", " + fmt("%.2f", l5) + " and " + fmt("%.2f", l15) + " over 1, 5 and 15 minutes. With " +
                   fmt("%.0f", threads) + " threads, that is " + grade + " (" + fmt("%.0f", 100 * l1 / threads) + "% of capacity).";
        // Normal for this machine: the CPU now against the same hour on other days.
        auto hour = query(db_, "SELECT 0, AVG(cpu), 0 FROM samples WHERE t >= ?", {now - 600});
        auto past = query(db_, "SELECT 0, med, 0 FROM samples_hourly WHERE hour_ts >= ? AND metric = 'cpu'", {now - 30 * 86400});
        if (!hour.empty() && past.size() >= 10) {
            std::vector<double> pool;
            for (const auto& x : past) pool.push_back(x.a);
            res.body += " The CPU has averaged " + fmt("%.0f", hour[0].a) + "% for ten minutes; your typical hour is " + fmt("%.0f", judge::median(pool)) +
                        "%, so this is " + (hour[0].a > judge::percentile(pool, 0.95) ? "high for you." : "within your usual.");
        }
        res.branch = "records:load";
        return res;
    }
    if (kind == "devices.list.now") {
        // USB devices from sysfs; hubs (class 09) left out, built-in ones (removable = fixed) apart.
        std::vector<std::string> plugged, inside;
        for (const auto& d : list_dir("/sys/bus/usb/devices")) {
            const std::string base = "/sys/bus/usb/devices/" + d;
            const std::string product = trim(read_line(base + "/product").value_or(""));
            if (product.empty() || trim(read_line(base + "/bDeviceClass").value_or("")) == "09") continue;
            const std::string maker = trim(read_line(base + "/manufacturer").value_or(""));
            const std::string name = maker.empty() || product.find(maker) == 0 ? product : maker + " " + product;
            (trim(read_line(base + "/removable").value_or("")) == "fixed" ? inside : plugged).push_back(name);
        }
        res.body = plugged.empty() ? "Nothing is plugged in by USB right now." : "Plugged in by USB: " + unllm::join_list(plugged) + ".";
        if (!inside.empty()) res.body += " Built in and on USB inside the laptop: " + unllm::join_list(inside) + ".";
        res.branch = "records:usb";
        return res;
    }
    if (kind == "gpu.value.window" || kind == "gpu.value.now.temp") {
        auto rows = series(db_, "gpu", "gpu", from, to);
        if (!sampler_) sampler_ = std::make_unique<Sampler>();
        const SnapshotPtr sn = sampler_->sample();
        double temp_now = -1;
        for (const auto& g : sn->gpus)
            if (g.temp_c.ok()) temp_now = std::max(temp_now, g.temp_c.get());
        // Asked for its temperature then: what isn't kept is said first, and the load that is follows; otherwise it is a
        // note after the load.
        const bool asked = kind == "gpu.value.now.temp";
        if (asked)
            res.scope.push_back("I don't keep the GPU's temperature over time, so I can't say how hot it got " + span +
                                (temp_now >= 0 ? "; it is at " + fmt("%.0f", temp_now) + " C now." : std::string(".")));
        else
            res.notes.push_back("I don't keep the GPU's temperature over time" +
                                (temp_now >= 0 ? "; it is at " + fmt("%.0f", temp_now) + " C now." : std::string(".")));
        if (rows.empty()) {
            res.body = "I have no GPU readings " + span + ".";
            res.branch = "records:gpu-none";
            return res;
        }
        const Row* hi = &rows[0];
        double sum = 0;
        for (const auto& x : rows) {
            if (x.a > hi->a) hi = &x;
            sum += x.a;
        }
        const std::string who = top_culprit(db_, from, to, KindGpu, "");
        res.body = "GPU load " + span + " peaked at " + fmt("%.0f", hi->a) + "% at " + when_text(hi->t) + " and averaged " +
                   fmt("%.0f", sum / double(rows.size())) + "%" + (who.empty() ? "" : "; its busy spells were mostly " + who) + ".";
        // Where the per-second readings reach: its spells at or over the recorder's busy level, with their times.
        if (const std::string told = readings_text(db_, "gpu", "GPU load", [](double v) { return fmt("%.0f%%", v); }, Triggers().gpu, from, to);
            !told.empty())
            res.body = told + (who.empty() ? "" : " Its busy spells were mostly " + who + ".");
        res.branch = "records:gpu-window";
        return res;
    }
    if (kind == "story.used.window") {
        // Who used the machine the records can't say; whether it was used, they can: the log-ins, the lid, sleep and
        // wake, the charger, and the programs opened by hand: apps the desktop starts (app-* units) and terminal tabs
        // (vte-spawn-* scopes), the units events.cpp also takes as the user's own doing.
        std::vector<std::string> said;
        // logind's "Logged in: 'user' with class 'user' and type 'wayland'."; a person's sessions are of class user.
        const std::string person = "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'session' AND title LIKE 'Logged in:%' "
                                   "AND title LIKE '%class ''user''%' ";
        auto who = [](const std::string& title) { return title.substr(12, title.find('\'', 12) - 12); };
        auto logins = query(db_, person + "AND t >= ? AND t < ? ORDER BY t", {from, to});
        if (logins.empty()) {
            auto last = query(db_, person + "AND t < ? ORDER BY t DESC LIMIT 1", {from});
            said.push_back("no one logged in" + (last.empty() ? std::string() : " (the last log-in was " + who(last[0].s1) + "'s, at " + when_text(last[0].t) + ")"));
        } else {
            std::vector<std::string> each;
            for (const auto& x : logins) each.push_back(who(x.s1) + " logged in at " + when_text(x.t));
            said.push_back(unllm::join_list(each));
        }
        // Sleep and wake, the lid opened, the power button.
        auto session = query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'session' AND t >= ? AND t < ? AND (title IN "
                                  "('Went to sleep', 'Woke from sleep', 'Lid opened', 'Power button pressed')) ORDER BY t", {from, to});
        std::vector<std::string> slept, lid, button;
        int64_t fell = 0;
        auto lasted = [](int64_t s) { return s < 60 ? duration_text(double(s)) : duration_text(s); };
        for (const auto& x : session) {
            if (x.s1 == "Went to sleep") fell = x.t;
            else if (x.s1 == "Woke from sleep") {
                // A sleep begun before the window: from its own start.
                if (!fell)
                    if (auto z = query(db_, "SELECT CAST(MAX(t) AS INTEGER) FROM log WHERE category = 'session' AND title = 'Went to sleep' AND t < ?", {x.t});
                        !z.empty())
                        fell = z[0].t;
                slept.push_back(fell ? when_text(fell) + " for " + lasted(x.t - fell) : "until " + when_text(x.t));
                fell = 0;
            }
            else if (x.s1 == "Lid opened") lid.push_back(when_text(x.t));
            else button.push_back(when_text(x.t));
        }
        if (fell) slept.push_back("from " + when_text(fell));
        said.push_back(slept.empty() ? "it didn't sleep" : "it slept " + (slept.size() == 1 ? std::string("once, ") : unllm::number_word(long(slept.size())) + " times: ") + unllm::join_list(slept));
        said.push_back(lid.empty() ? "the lid wasn't opened" : "the lid was opened at " + unllm::join_list(lid));
        if (!button.empty()) said.push_back("the power button was pressed at " + unllm::join_list(button));
        // The charger, unplugged and plugged in.
        std::vector<std::string> out, in;
        for (const auto& x : query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'session' AND t >= ? AND t < ? AND "
                                        "(title LIKE 'Unplugged from power%' OR title LIKE 'Plugged into power%') ORDER BY t", {from, to}))
            (x.s1[0] == 'U' ? out : in).push_back(when_text(x.t));
        if (!out.empty()) said.push_back("the charger was unplugged at " + unllm::join_list(out) + (in.empty() ? "" : " and plugged in at " + unllm::join_list(in)));
        else if (!in.empty()) said.push_back("the charger was plugged in at " + unllm::join_list(in));
        // Programs opened by hand, by the unit each opened in: stretches of openings less than an hour apart, and the
        // longest quiet stretch between them.
        auto opened = query(db_, "SELECT CAST(MIN(started) AS INTEGER), 0, 0, unit FROM procs WHERE unit LIKE 'app-%' OR unit LIKE 'vte-spawn-%' "
                                 "GROUP BY unit HAVING MIN(started) >= ? AND MIN(started) < ? ORDER BY 1", {from, to});
        if (opened.empty()) {
            said.push_back("no program was opened by hand");
        } else {
            std::vector<std::pair<int64_t, int64_t>> runs;
            std::vector<std::string> names;
            int64_t quiet_from = 0, quiet_to = 0;
            for (std::size_t k = 0; k < opened.size(); ++k) {
                const int64_t t = opened[k].t;
                if (k && t - opened[k - 1].t > quiet_to - quiet_from) quiet_from = opened[k - 1].t, quiet_to = t;
                if (!runs.empty() && t - runs.back().second < 3600) runs.back().second = t;
                else runs.push_back({t, t});
                // The app by its id ("app-org.kde.okular@a1.service": okular); a terminal tab as such.
                std::string u = opened[k].s1;
                std::string name = "a terminal tab";
                if (u.rfind("app-", 0) == 0) {
                    u = u.substr(4, u.find_first_of("@") == std::string::npos ? u.rfind('.') - 4 : u.find('@') - 4);
                    name = u.substr(u.rfind('.') + 1);
                }
                if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
            }
            std::vector<std::string> when;
            for (const auto& [a, b] : runs) when.push_back(when_text(a) == when_text(b) ? when_text(a) : span_text(a, b, " to "));
            said.push_back("programs were opened by hand at " + unllm::join_list(when) + " (" + unllm::join_list(names) + ")" +
                           (quiet_to - quiet_from >= 3 * 3600 ? ", none between " + when_text(quiet_from) + " and " + when_text(quiet_to) : ""));
        }
        std::string lead = span;
        lead[0] = char(std::toupper(static_cast<unsigned char>(lead[0])));
        res.body = "The records can't say who used it: they keep the log-ins, sleep and wake, the lid, the charger and the programs opened, "
                   "not who was at the keyboard. " + lead + ": ";
        for (std::size_t k = 0; k < said.size(); ++k) res.body += (k ? "; " : "") + said[k];
        res.body += ".";
        res.branch = "records:used";
        return res;
    }
    if (kind == "updates.cause.window") {
        // What broke, then the software changes before it: for the network, the Wi-Fi's disconnects while the laptop
        // was awake (the log's "Disconnected: <device>" rows more than a minute from its going to sleep); otherwise
        // the first crash or unit failure.
        const int64_t f = r.window.stated ? from : now - 7 * 86400;
        const std::string where = r.window.stated ? span : "in the last week";
        bool net = false;
        for (Intent i : r.intents) net = net || i == Intent::Network;
        int64_t first = 0;
        std::string fault;
        if (net) {
            const std::string dev = wifi_iface();
            auto sleeps = query(db_, "SELECT CAST(t AS INTEGER) FROM log WHERE category = 'session' AND title = 'Went to sleep' AND t >= ? AND t < ?", {f - 60, to + 60});
            std::size_t naps = 0, drops = 0;
            int64_t back = 0;
            for (const auto& x : query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'network' AND t >= ? AND t < ? AND "
                                            "(title LIKE 'Disconnected: %' OR title LIKE 'Connected to % on %') ORDER BY t", {f, to})) {
                if (!dev.empty() && x.s1.compare(x.s1.size() - std::min(x.s1.size(), dev.size()), dev.size(), dev) != 0) continue;
                if (x.s1.rfind("Connected", 0) == 0) {
                    if (first && !back) back = x.t;
                    continue;
                }
                bool asleep = false;
                for (const auto& z : sleeps) asleep = asleep || std::llabs(z.t - x.t) <= 60;
                if (asleep) ++naps;
                else if (++drops == 1) first = x.t;
            }
            if (!drops)
                fault = "The Wi-Fi didn't drop " + where + " while the laptop was awake" +
                        (naps ? ": its " + (naps == 1 ? std::string("one disconnect") : unllm::number_word(long(naps)) + " disconnects") +
                                    " came as it went to sleep, and it joined the network again on waking"
                              : "") +
                        ", so the records show nothing that broke it.";
            else
                fault = "The Wi-Fi first dropped at " + when_text(first) + (back ? " (back at " + when_text(back) + ")" : "") + " while the laptop was awake" +
                        (drops > 1 ? ", " + unllm::number_word(long(drops)) + " times in all " + where : "") + ".";
        } else {
            auto bad = query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category IN ('problems', 'services') AND t >= ? AND t < ? "
                                  "ORDER BY t LIMIT 1", {f, to});
            if (bad.empty()) {
                fault = "Nothing crashed or failed " + where + ".";
            } else {
                first = bad[0].t;
                fault = "The first crash or failure " + where + " was at " + when_text(first) + ": " + bad[0].s1 + ".";
            }
        }
        std::vector<std::string> told;
        for (const auto& x : query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title, detail FROM log WHERE category = 'software' AND t >= ? AND t < ? ORDER BY t DESC",
                                   {f, first ? first : to})) {
            bool s = false;
            if (told.size() < 3) told.push_back(software_change(db_, x.t, x.s1, x.s2, &s));
        }
        std::string changed;
        for (const auto& x : told) changed += (changed.empty() ? "" : ". ") + x;
        res.body = fault + " " + (told.empty() ? (first ? "No software change was recorded before it " + where + "." : "No software change was recorded " + where + ".")
                                                : std::string(first ? "Software changed before it: " : "Software changed " + where + ": ") + changed + ".");
        res.branch = "records:update-cause";
        return res;
    }
    if (kind == "devices.when.window") {
        // The device log: what udev saw plugged in and unplugged (USB devices and serial ports; Bluetooth links are
        // not logged). With no time named, all of it; a device the question names by a word of its own keeps its rows.
        const int64_t f = r.window.stated ? from : 0;
        const std::string where = r.window.stated ? " " + span : "";
        auto rows = query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'devices' AND t >= ? AND t < ? ORDER BY t DESC", {f, to});
        std::vector<Row> kept;
        const auto words = named_words(r, kind);
        for (const auto& x : rows) {
            std::string low = x.s1;
            for (auto& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
            bool named = words.empty();
            for (const auto& w : words) named = named || low.find(w) != std::string::npos;
            if (named) kept.push_back(x);
        }
        if (kept.empty()) {
            res.body = "No device was plugged in or unplugged" + where + " as far as the device log goes (it keeps USB devices and serial ports; "
                       "Bluetooth links aren't logged).";
        } else {
            std::vector<std::string> each;
            for (std::size_t k = 0; k < kept.size() && k < 4; ++k) each.push_back(kept[k].s1 + " at " + when_text(kept[k].t));
            res.body = "The device log" + where + (kept.size() > each.size() ? ", the latest: " : ": ") + unllm::join_list(each) + ".";
        }
        // What the machine did then: going to sleep cuts devices off and waking brings them back.
        if (r.window.stated) {
            std::vector<std::string> naps;
            for (const auto& w : query(db_, "SELECT CAST(t AS INTEGER) FROM log WHERE category = 'session' AND title = 'Woke from sleep' AND t >= ? AND t < ? ORDER BY t",
                                       {from, to})) {
                auto fell = query(db_, "SELECT CAST(MAX(t) AS INTEGER) FROM log WHERE category = 'session' AND title = 'Went to sleep' AND t <= ?", {w.t});
                const int64_t z = fell.empty() ? 0 : fell[0].t;
                auto shut = query(db_, "SELECT 1 FROM log WHERE category = 'session' AND title = 'Lid closed' AND t >= ? AND t <= ? LIMIT 1", {z - 60, z});
                naps.push_back(z ? "it went to sleep at " + when_text(z) + (shut.empty() ? "" : " (the lid closed)") + " and woke " +
                                       (w.t - z < 60 ? duration_text(double(w.t - z)) + " later" : "at " + when_text(w.t))
                                 : "it woke from sleep at " + when_text(w.t));
            }
            if (!naps.empty()) {
                std::string s = unllm::join_list(naps);
                s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
                res.body += " " + s + ": devices drop off while it sleeps and come back as it wakes.";
            }
        }
        res.branch = "records:devices";
        return res;
    }
    if (kind == "flash.list.window") {
        // The flashing log (boards flashed with esptool, arduino-cli and the like); with no time named, all of it.
        const int64_t f = r.window.stated ? from : 0;
        const std::string where = r.window.stated ? "recorded " + span : "on record";
        std::string program;
        for (const auto& e : r.entities)
            if (e.type == "program") program = e.value;
        std::vector<Row> rows;
        for (const auto& x : query(db_, "SELECT CAST(t AS INTEGER), 0, 0, title FROM log WHERE category = 'flashing' AND t >= ? AND t < ? ORDER BY t DESC", {f, to}))
            if (program.empty() || x.s1.find(program) != std::string::npos) rows.push_back(x);
        if (rows.empty()) {
            res.body = "No firmware flash is " + where + ".";
        } else {
            std::vector<std::string> each;
            for (std::size_t k = 0; k < rows.size() && k < 4; ++k) each.push_back(rows[k].s1 + " (" + when_text(rows[k].t) + ")");
            res.body = std::to_string(rows.size()) + (rows.size() == 1 ? " firmware flash is " : " firmware flashes are ") + where +
                       (rows.size() > each.size() ? ", the latest: " : ": ") + unllm::join_list(each) + ".";
        }
        // The laptop's own firmware, when the question's words are about the machine itself ("firmware", "bios": the
        // analyzer's own reading, before the kind led it): the BIOS by version and date; when it was put on isn't kept.
        bool own = false;
        for (Intent i : an_.read(card_.question, now).intents) own = own || i == Intent::Identity;
        if (own) {
            auto dmi = [](const char* name) { return trim(read_line(std::string("/sys/class/dmi/id/") + name).value_or("")); };
            if (const std::string v = dmi("bios_version"); !v.empty())
                res.body += " The BIOS is " + v + " from " + dmi("bios_vendor") + ", dated " + dmi("bios_date") +
                            "; the records don't say when it was put on, and no BIOS update is in them.";
        }
        res.branch = "records:flash";
        return res;
    }
    if (kind == "diagnose.compare.window") {
        // The window's events against the same hours on each of the seven days before.
        res.body = story_summary(from, to, now, &res.receipts);
        if (res.body.rfind("In that window", 0) == 0) {
            res.body.replace(0, 14, span);
            res.body[0] = char(std::toupper(static_cast<unsigned char>(res.body[0])));
        }
        auto count = [&](int64_t a, int64_t b) {
            auto c = query(db_, "SELECT COUNT(*) FROM events WHERE start >= ? AND start < ?", {a, b});
            return c.empty() ? 0 : c[0].t;
        };
        const int64_t here = count(from, to);
        int64_t sum = 0;
        for (int d = 1; d <= 7; ++d) sum += count(from - d * 86400, to - d * 86400);
        const double avg = double(sum) / 7.0;
        if (res.body.empty()) res.body = "Nothing was recorded " + span + ".";
        res.body += " That is " + std::to_string(here) + (here == 1 ? " event" : " events") + " against " + fmt("%.0f", avg) +
                    " on an average day over the same hours, so it was " +
                    (here > avg * 1.5 + 2 ? "a worse stretch than usual." : here < avg * 0.5 ? "a quieter stretch than usual." : "about usual.");
        std::size_t kills = 0;
        oom_kills(db_, from, to, 0, &kills);
        if (kills) res.body += " Memory ran out " + std::to_string(kills) + (kills == 1 ? " time" : " times") + ", and the out-of-memory killer stopped programs.";
        res.branch = "records:compare-days";
        return res;
    }
    if (kind == "battery.value.now") {
        // Now, and for "at 6am vs now" the reading nearest that moment as well.
        res = answer_records("battery.value.now.remaining", r, now);
        if (r.window.stated && !r.window.live && to - from <= 2 * 3600) {
            const int64_t mid = (from + to) / 2;
            auto at = query(db_, "SELECT CAST(t AS INTEGER), battery_pct, IFNULL(on_ac, -1) FROM samples WHERE battery_pct IS NOT NULL "
                                 "AND t BETWEEN ? AND ? ORDER BY ABS(t - ?) LIMIT 1", {mid - 3600, mid + 3600, mid});
            if (at.empty())
                res.scope.insert(res.scope.begin(), "I have no battery reading near " + when_text(mid) + ".");
            else
                res.body = "At " + when_text(at[0].t) + " the battery was at " + fmt("%.0f", at[0].a) + "%" +
                           (at[0].b == 1 ? " on the charger" : at[0].b == 0 ? " on battery" : "") + ". Now: " + res.body;
        }
        res.branch = "records:battery-now";
        return res;
    }
    // Only the free-space kinds go on from here; any other kind reaching this point lost its handler.
    if (kind.rfind("disk.space.", 0) != 0) {
        res.body = "I can't answer that from the records yet.";
        res.branch = "records:unhandled:" + kind;
        res.refused = true;
        return res;
    }
    const auto fs = real_filesystems();
    if (fs.empty()) {
        res.body = "I couldn't read the filesystems' sizes just now.";
        res.branch = "space:none";
        res.refused = true;
        return res;
    }
    for (const auto& f : fs) res.receipts.push_back(space_line(f));
    const Space* root = &fs[0];
    for (const auto& f : fs)
        if (f.mount == "/") root = &f;

    if (kind == "disk.space.trend.window") {
        // The per-minute samples of / free, first and last in the window asked
        // (the last week when none was named).
        const int64_t from = r.window.stated ? r.window.from : now - 7 * 86400;
        auto rows = series(db_, "disk_free_pct", "disk_free_pct", from, to);
        if (rows.size() < 2) {
            auto first = query(db_, "SELECT CAST(MIN(t) AS INTEGER) FROM samples WHERE disk_free_pct IS NOT NULL", {});
            res.body = space_line(*root) + " right now. I have only been recording free space since " +
                       (first.empty() || first[0].t == 0 ? std::string("recently") : when_text(first[0].t)) +
                       ", so I can't say how it changed over that time.";
            res.branch = "space:trend-short";
            return res;
        }
        res.body = space_change(db_, rows, root->total) + " " + space_line(*root) + " now.";
        // A moment the question names by a word of its own ("before monday's backup"): the latest program or log entry
        // in the window that the word names as a whole part of its name, command line or title, and the free space
        // just before it; or that nothing in the records does. The kind's own wording (its phrasings: "lose", "gone")
        // and a program the question is about ("how much did ollama eat") name no moment.
        std::vector<std::string> words;
        for (const auto& w : named_words(r, kind)) {
            bool program = false;
            for (const auto& e : r.entities) program |= e.type == "program" && e.value == w;
            if (!program && !kinds_.phrased(kind, w)) words.push_back(w);
        }
        if (!words.empty() && r.window.stated) {
            const std::string& w = words[0];
            auto parts_of = [](const std::string& s) {
                std::set<std::string> out;
                std::string cur;
                for (char c : s + " ") {
                    if (std::isalnum(static_cast<unsigned char>(c))) cur += char(std::tolower(static_cast<unsigned char>(c)));
                    else if (!cur.empty()) out.insert(cur), cur.clear();
                }
                return out;
            };
            std::vector<Row> hit;
            for (auto& x : query(db_, "SELECT CAST(started AS INTEGER), 0, 0, name, cmdline FROM procs WHERE started >= ?1 AND started < ?2 "
                                      "AND instr(lower(name || ' ' || IFNULL(cmdline, '')), ?3) > 0 UNION ALL "
                                      "SELECT CAST(t AS INTEGER), 0, 0, title, '' FROM log WHERE t >= ?1 AND t < ?2 AND instr(lower(title), ?3) > 0 "
                                      "ORDER BY 1 DESC", {from, to}, {w}))
                if (hit.empty() && parts_of(x.s1 + " " + x.s2).count(w)) hit.push_back(x);
            if (hit.empty()) {
                res.scope.insert(res.scope.begin(), "Nothing in the records names a " + w + " " + span + ": no program run and no log entry, so I can't place it.");
            } else {
                std::string what = hit[0].s1 + " at " + when_text(hit[0].t) + (hit[0].s2.empty() ? "" : " (" + command_text(hit[0].s2, 4) + ")");
                const Row* before = nullptr;
                for (const auto& x : rows)
                    if (x.t < hit[0].t) before = &x;
                res.scope.insert(res.scope.begin(), "The " + w + " in the records: " + what + "." +
                                                        (before ? " The last reading before it, at " + when_text(before->t) + ", had " +
                                                                      fmt("%.0f", before->a) + "% of / free (about " +
                                                                      human_bytes(before->a / 100.0 * root->total) + ")."
                                                                : std::string(" No reading of the free space comes before it in that window.")));
            }
        }
        res.branch = "space:trend";
        return res;
    }

    if (kind == "disk.space.which.now") {
        // The home folder walked for at most 20 s; what was found by then is ranked. The scan is kept for ten minutes
        // (scan_cache_), so two questions a minute apart get the same figures.
        struct Scan {
            double total = 0;
            bool cut = false;
            std::vector<std::pair<std::string, double>> kids, largest;
        } scan;
        bool have = false;
        const char* h = getenv("HOME");
        const std::string home = h ? h : "/";
        auto tabs = [](const std::string& line) {
            std::vector<std::string> out;
            std::size_t at = 0;
            for (std::size_t tab; (tab = line.find('\t', at)) != std::string::npos; at = tab + 1) out.push_back(line.substr(at, tab - at));
            out.push_back(line.substr(at));
            return out;
        };
        auto shorten_home = [&](const std::string& path) { return path.rfind(home + "/", 0) == 0 ? "~" + path.substr(home.size()) : path; };
        if (std::ifstream in(scan_cache_); in) {
            std::string line;
            int64_t at = 0;
            while (std::getline(in, line)) {
                const auto f = tabs(line);
                if (f.size() == 2 && f[0] == "at") at = std::atoll(f[1].c_str());
                else if (f.size() == 3 && f[0] == "total") scan.total = std::atof(f[1].c_str()), scan.cut = f[2] == "cut";
                else if (f.size() == 3 && f[0] == "kid") scan.kids.emplace_back(f[1], std::atof(f[2].c_str()));
                else if (f.size() == 3 && f[0] == "file") scan.largest.emplace_back(f[1], std::atof(f[2].c_str()));
            }
            have = at > 0 && now - at < 600 && !scan.kids.empty();
        }
        if (!have) {
            scan = Scan{};
            std::atomic<bool> cancel{false};
            std::atomic<long long> files{0}, bytes{0};
            std::thread timer([&cancel] {
                for (int i = 0; i < 200 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                cancel = true;
            });
            auto tree = scan_dir(home, cancel, files, bytes);
            scan.cut = cancel.load();
            cancel = true;
            timer.join();
            if (!tree || tree->kids.empty()) {
                res.body = space_line(*root) + ". I couldn't read your home folder to say what takes the space.";
                res.branch = "space:which-none";
                return res;
            }
            scan.total = tree->bytes;
            for (const auto& k : tree->kids) scan.kids.emplace_back(k->name, k->bytes);
            std::sort(scan.kids.begin(), scan.kids.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            scan.largest = tree->largest;
            if (std::ofstream out(scan_cache_); out) {
                out << "at\t" << now << "\ntotal\t" << fmt("%.0f", scan.total) << "\t" << (scan.cut ? "cut" : "whole") << "\n";
                for (std::size_t i = 0; i < scan.kids.size() && i < 5; ++i) out << "kid\t" << scan.kids[i].first << "\t" << fmt("%.0f", scan.kids[i].second) << "\n";
                for (const auto& [path, b] : scan.largest) out << "file\t" << path << "\t" << fmt("%.0f", b) << "\n";
            }
        }
        std::string top;
        for (std::size_t i = 0; i < scan.kids.size() && i < 5; ++i)
            top += std::string(i ? (i + 1 == std::min<std::size_t>(5, scan.kids.size()) ? " and " : ", ") : "") + scan.kids[i].first + " " + human_bytes(scan.kids[i].second);
        const bool cut = scan.cut;
        const std::string folders = "In your home folder (" + human_bytes(scan.total) + (cut ? " counted in 20 s, so at least that" : "") +
                                    "), the biggest are " + top + ".";
        // The biggest single files the walk passed. A question about the largest file leads with them ("what is the
        // largest file on disk"); one about folders, or space in general, with the folders.
        std::vector<std::string> big;
        for (const auto& [path, b] : scan.largest) big.push_back(shorten_home(path) + " (" + human_bytes(b) + ")");
        const int asked = big.empty() ? 0 : asked_test(r, {{"which folders take the most space", "the biggest folders on the disk"},
                                                          {"the largest single file on the disk", "which one file is the biggest"}});
        if (asked == 1) {
            std::vector<std::string> rest(big.begin() + 1, big.end());
            res.body = "The largest single file is " + big[0] + (rest.empty() ? "" : ", then " + unllm::join_list(rest)) + ". " + folders + " " +
                       space_line(*root) + ".";
        } else {
            res.body = folders + " " + space_line(*root) + "." + (big.empty() ? "" : " The biggest single files: " + unllm::join_list(big) + ".");
        }
        // "What filled up my drive this week": the folders' sizes are a scan made now; over a past window only the free
        // space is kept, so that is said first, with how it moved.
        if (r.window.stated && !r.window.live && !r.window.present) {
            const std::string moved = space_change(db_, series(db_, "disk_free_pct", "disk_free_pct", from, to), root->total);
            res.scope.insert(res.scope.begin(), "I don't keep folder sizes over time, only the free space, so I can't say which folder grew " + span + ".");
            res.body = (moved.empty() ? "" : moved + " ") + "Now: " + res.body;
        }
        // Home against the rest of the filesystem that holds it.
        const Space* holder = root;
        for (const auto& f : fs)
            if (home.rfind(f.mount, 0) == 0 && f.mount.size() > holder->mount.size()) holder = &f;
        const double used = holder->total - holder->free;
        if (used > scan.total)
            res.body += " Home is " + std::string(cut ? "at least " : "") + fmt("%.0f", 100.0 * scan.total / used) + "% of what " + holder->mount +
                        " holds; the rest takes " + (cut ? "at most " : "about ") + human_bytes(used - scan.total) + ".";
        res.branch = "space:which";
        return res;
    }

    // Now: every real filesystem.
    std::string text;
    for (std::size_t i = 0; i < fs.size(); ++i) text += (i ? "; " : "") + space_line(fs[i]);
    res.body = text + ".";
    res.branch = "space:now";
    return res;
}

AskResult Ask::follow_up(unllm::Act act, const unllm::Card& card, int64_t now) {
    using unllm::Act;
    AskResult res;
    auto rerun = [&](const unllm::Reading& rr) {
        auto toks = unllm::tokenize(card.question);
        return answer_reading(rr, toks, toks, card.question, now);
    };
    const int64_t from = card.window.from, to = card.window.to ? card.window.to : now;
    auto is_program = [&](const std::string& name) {
        if (name.empty()) return false;
        auto n = query(db_, "SELECT (SELECT COUNT(*) FROM procs WHERE name = ?1) + "
                            "(SELECT COUNT(*) FROM event_culprits WHERE name = ?1)", {}, {name});
        return !n.empty() && n[0].t > 0;
    };
    auto mask_of = [](Intent i) -> unsigned {
        switch (i) {
        case Intent::Heat: return 16;
        case Intent::Slow: return 1;
        case Intent::Memory: return 2;
        case Intent::Disk: return 4;
        case Intent::Gpu: return 8;
        default: return 0;
        }
    };
    const bool live = card.branch.rfind("live:", 0) == 0;

    switch (act) {
    case Act::Why: {
        // "Why" after a crash answer asks what the crash was: the crash records with the kernel's note on each, for the
        // program the answer named, over the same time.
        if (card.branch == "records:crashes" || card.topic == Intent::Crash) {
            unllm::Reading rr = card.reading;
            rr.wants = unllm::Wants::Cause;
            rr.form = Form::Open;
            rr.domain = 1.0;
            bool has_prog = false;
            for (const auto& e : rr.entities) has_prog |= e.type == "program";
            if (!has_prog && is_program(card.subject)) rr.entities.push_back({"program", card.subject});
            return answer_records("crashes.cause.window", rr, now);
        }
        // A rule that fired on the very event is the best "why".
        if (card.event_id) {
            Diagnose diag(db_);
            for (const auto& c : diag.for_event(card.event_id, now))
                if (c.rule_id != "swap_parked") {
                    res.body = c.sentence;
                    res.receipts = c.receipts;
                    return res;
                }
        }
        if (live) {
            if (!sampler_) sampler_ = std::make_unique<Sampler>();
            sampler_->sample();
            usleep(600 * 1000);
            SnapshotPtr s = sampler_->sample();
            auto procs = s->procs;
            std::sort(procs.begin(), procs.end(), [](const auto& a, const auto& b) { return a.cpu > b.cpu; });
            if (!procs.empty() && procs[0].cpu >= 3) {
                res.body = "Right now the busiest program is " + procs[0].name + ", at " +
                           fmt("%.0f%%", procs[0].cpu) + " of the CPU.";
                for (const auto& p : procs)
                    if (res.receipts.size() < 4 && p.cpu >= 0.5)
                        res.receipts.push_back(p.name + "  " + fmt("%.1f%% cpu", p.cpu));
            } else {
                res.body = "Nothing is working hard right now; the reading is the machine's resting state.";
            }
            return res;
        }
        if (is_program(card.subject)) {
            const Fact* f = facts_.lookup(card.subject);
            std::string text;
            if (f) text = unllm::realize({{{card.subject, true}, "be", f->explanation, Tense::Present}}) + " ";
            auto ev = query(db_, "SELECT COUNT(*), 0, 0 FROM events e JOIN event_culprits c ON c.event_id = e.id "
                                 "WHERE e.start >= ? AND e.start < ? AND c.name = ?",
                            {from, to}, {card.subject});
            int64_t n = ev.empty() ? 0 : ev[0].t;
            if (n > 0) {
                text += "It was a culprit in " + unllm::number_word(n) + " resource " + unllm::plural_noun("event", n) +
                        " in that window: it was doing heavy work, not misbehaving on its own.";
                std::size_t seen = 0;
                for (const auto& row : query(db_, "SELECT e.start, 0, 0, e.summary FROM events e JOIN event_culprits c "
                                                  "ON c.event_id = e.id WHERE e.start >= ? AND e.start < ? AND c.name = ? "
                                                  "ORDER BY c.share DESC LIMIT 3",
                                             {from, to}, {card.subject}))
                    if (seen++ < 3) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
            }
            if (!text.empty()) {
                res.body = text;
                return res;
            }
        }
        res.body = "I have no cause on record for that beyond what I said.";
        return res;
    }
    case Act::When: {
        if (card.moment > 0) {
            res.body = "That was " + moment_text(card.moment) + ".";
            return res;
        }
        if (is_program(card.subject)) {
            auto rows = query(db_, "SELECT MAX(started), 0, 0 FROM procs WHERE started >= ? AND started < ? AND name = ?",
                              {from, to}, {card.subject});
            if (!rows.empty() && rows[0].t > 0) {
                res.body = unllm::realize({{{card.subject, true}, "start", "last on " + moment_text(rows[0].t),
                                            Tense::Past}});
                return res;
            }
        }
        res.body = "That answer covers a whole window, not one moment: " + day_text(from) +
                   (to - from > 86400 ? " to " + day_text(to) : "") + ".";
        return res;
    }
    case Act::More: {
        if (card.event_id) {
            auto rows = query(db_, "SELECT start, 0, 0, summary FROM events WHERE id = ?", {card.event_id});
            if (!rows.empty()) {
                res.body = rows[0].s1;
                res.receipts.push_back(when_text(rows[0].t) + "  the event in full");
                return res;
            }
        }
        std::size_t shown = 0;
        for (const auto& rc : card.receipts) shown += rc.rfind("spelling:", 0) == 0 ? 0 : 1;
        receipt_offset_ = shown;
        AskResult more = rerun(card.reading);
        receipt_offset_ = 0;
        if (more.receipts.empty()) {
            res.body = "That's everything the records show for it.";
            return res;
        }
        res.body = "More of the records behind that:";
        res.receipts = more.receipts;
        return res;
    }
    case Act::Normal: {
        // A program's value against that program's own history.
        if (is_program(card.subject) && card.metric == "memory" && !std::isnan(card.value)) {
            std::vector<double> pool;
            for (const auto& row : query(db_, "SELECT 0, MAX(peak_rss), 0 FROM procs WHERE started >= ? AND name = ? "
                                              "GROUP BY date(started, 'unixepoch', 'localtime')",
                                         {now - 30 * 86400}, {card.subject}))
                if (row.a > 0) pool.push_back(row.a);
            if (pool.size() >= 3) {
                double typical = judge::median(pool);
                double rank = judge::percentile_rank(pool, card.value);
                std::string verdict = rank > 95 ? "unusually high for it" : rank < 5 ? "unusually low for it" : "within its usual range";
                res.body = "For " + card.subject + ", " + human_bytes(card.value) + " is " + verdict +
                           ": its typical daily peak is " + human_bytes(typical) + " across " +
                           unllm::number_word(int64_t(pool.size())) + " days of records.";
                return res;
            }
        }
        unllm::Reading rr = card.reading;
        rr.judge = unllm::Judge::Normal;
        rr.domain = 1.0;
        return rerun(rr);
    }
    case Act::Which: {
        // What the answer itself named, in its order ("haruna holds the most, then clangd.main and claude": haruna).
        if (!card.subjects_named.empty()) {
            std::vector<std::string> rest(card.subjects_named.begin() + 1, card.subjects_named.end());
            if (rest.size() > 2) rest.resize(2);
            res.body = "That was " + card.subjects_named[0] +
                       (card.metric == "memory" && !std::isnan(card.value) ? ", with " + human_bytes(card.value) : "") +
                       (rest.empty() ? "" : ", then " + unllm::join_list(rest)) + ".";
            return res;
        }
        unsigned mask = mask_of(card.topic);
        if (mask) {
            auto rows = query(db_, "SELECT 0, SUM(c.share * IFNULL(e.total_cpu_seconds, 1)), COUNT(*), c.name "
                                   "FROM events e JOIN event_culprits c ON c.event_id = e.id "
                                   "WHERE e.start < ? AND e.\"end\" >= ? AND (e.kinds & ?) != 0 "
                                   "GROUP BY c.name ORDER BY 2 DESC LIMIT 3",
                              {to, from, int64_t(mask)});
            std::vector<std::string> names;
            for (const auto& row : rows)
                if (!row.s1.empty()) {
                    names.push_back(row.s1);
                    res.receipts.push_back(row.s1 + "  in " + std::to_string(int64_t(row.b)) + " events");
                }
            if (!names.empty()) {
                res.body = "The main ones were " + unllm::join_list(names) + ", heaviest first.";
                return res;
            }
        }
        if (!card.subject.empty()) {
            res.body = "That was " + card.subject + ".";
            return res;
        }
        res.body = "No single program stands out in that answer.";
        return res;
    }
    case Act::HowLong: {
        unllm::Reading rr = card.reading;
        rr.wants = unllm::Wants::Duration;
        rr.domain = 1.0;
        bool has_prog = false;
        for (const auto& e : rr.entities) has_prog |= e.type == "program";
        if (!has_prog && is_program(card.subject)) rr.entities.push_back({"program", card.subject});
        return rerun(rr);
    }
    case Act::Evidence: {
        std::vector<std::string> recs;
        for (const auto& rc : card.receipts)
            if (rc.rfind("spelling:", 0) != 0) recs.push_back(rc);
        if (recs.empty()) {
            res.body = live ? "That was measured live at " + moment_text(card.at) + ", not read from a record."
                            : "That answer did not rest on any record I can show.";
        } else {
            res.lead = "Yes.";
            res.body = "That came from these records:";
            res.receipts = recs;
        }
        return res;
    }
    default:
        res.body = "I'm not sure what that refers to.";
        return res;
    }
}

}  // namespace as
