#include "diagnose.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

#include "judge.h"
#include "snapshot.h"

namespace as {

namespace {

struct Row {
    int64_t t = 0;
    double a = 0, b = 0;
    std::string s1, s2;
};

std::vector<Row> q(sqlite3* db, const std::string& sql, const std::vector<int64_t>& binds,
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

std::string when_text(int64_t t) {
    std::time_t tt = t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%a %b %d %H:%M", &tm);
    return buf;
}

std::string gb(double bytes) {
    char buf[32];
    if (bytes >= 1e9) std::snprintf(buf, sizeof buf, "%.1f GB", bytes / 1e9);
    else std::snprintf(buf, sizeof buf, "%.0f MB", bytes / 1e6);
    return buf;
}

std::string num(int64_t n) {
    static const char* small[] = {"no", "one", "two", "three", "four", "five", "six",
                                  "seven", "eight", "nine", "ten", "eleven", "twelve"};
    if (n >= 0 && n <= 12) return small[n];
    return std::to_string(n);
}

}  // namespace

std::string advice_for(const std::string& rule_id) {
    static const std::map<std::string, std::string> advice = {
        {"disk_errors", "Copy anything you care about somewhere else today, then check the drive's cable or replace the "
                        "drive; errors like these rarely stop on their own."},
        {"dead_fan", "Have that fan replaced or cleaned; until then keep heavy GPU work short and the vents clear."},
        {"thermal_shutdown", "Clean the vents and fans, and keep long heavy jobs shorter until the cooling is fixed."},
        {"oom_kill", "Close the biggest memory holder before starting heavy work, or add swap or memory."},
        {"update_regression", "Check that package's changelog, and consider rolling it back with your package "
                              "manager's cache if the trouble continues."},
        {"swap_thrash", "Close the biggest memory holder; thrashing stops as soon as the working set fits in memory "
                        "again."},
        {"swap_parked", "Nothing to do: swap holds idle pages, not active work."},
        {"leak", "Restart that program to get its memory back, and watch whether the climb starts again."},
        {"time_to_full", "Free space before it runs out: clear caches, old downloads, or package caches."},
        {"disk_wait", "Space out the heavy disk jobs, or move them to a time you're not using the machine."},
        {"battery_aged", "A replacement battery restores the runtime; nothing in software will."},
    };
    auto it = advice.find(rule_id);
    return it == advice.end() ? "" : it->second;
}

const char* grade_phrase(Grade g) {
    switch (g) {
    case Grade::Is: return "is";
    case Grade::PointsTo: return "points to";
    case Grade::ConsistentWith: return "is most consistent with";
    }
    return "?";
}

std::vector<Conclusion> Diagnose::run(int64_t from, int64_t to, int64_t now, const Snapshot* live) {
    std::vector<Conclusion> out;
    if (!db_) return out;

    // Rule disk_errors (hardware, Is): the disk reporting its own failures
    // outranks every software explanation.
    {
        auto rows = q(db_, "SELECT t,0,0,title FROM log WHERE category='problems' AND t>=? AND t<? AND "
                           "(title LIKE 'Disk read or write error:%' OR title LIKE 'Filesystem error:%' "
                           "OR title LIKE 'Hardware error reported:%') ORDER BY t DESC LIMIT 20",
                      {from, to});
        if (!rows.empty()) {
            Conclusion c{"disk_errors"};
            c.grade = Grade::Is;
            c.salience = 3;
            c.specificity = 2;
            c.t = rows[0].t;
            c.sentence = "The disk is reporting hardware errors (" + num(int64_t(rows.size())) +
                         " in this window); that is a failing disk or cable, not any program. Back up now.";
            for (std::size_t i = 0; i < rows.size() && i < 3; ++i)
                c.receipts.push_back(when_text(rows[i].t) + "  " + rows[i].s1);
            out.push_back(std::move(c));
        }
    }

    // Rule dead_fan (hardware, Is): a fan that never spins while the machine
    // is hot, across days, is a hardware fault.
    {
        auto agg = q(db_, "SELECT MAX(start), COUNT(*), 0, "
                          "(SELECT dead_fans FROM events WHERE dead_fans<>'' AND (kinds&16)!=0 ORDER BY start DESC LIMIT 1) "
                          "FROM events WHERE dead_fans IS NOT NULL AND dead_fans<>'' AND (kinds&16)!=0 AND start>=? AND start<?",
                     {from, to});
        auto days = q(db_, "SELECT COUNT(DISTINCT date(start,'unixepoch','localtime')),0,0 FROM events "
                           "WHERE dead_fans IS NOT NULL AND dead_fans<>'' AND (kinds&16)!=0 AND start>=? AND start<?",
                      {from, to});
        int64_t n = agg.empty() ? 0 : agg[0].a;
        int64_t d = days.empty() ? 0 : days[0].t;
        if (n >= 3 && d >= 2) {
            std::string fan = agg[0].s1.substr(0, agg[0].s1.find('\n'));
            Conclusion c{"dead_fan"};
            c.grade = Grade::Is;
            c.salience = 3;
            c.specificity = 3;
            c.t = agg[0].t;
            c.sentence = "The fan '" + fan + "' is not spinning while the machine is hot, across " + num(n) +
                         " hot events on " + num(d) + " days: that is a hardware fan failure, not any program.";
            c.receipts.push_back(num(n) + " hot events with a dead fan, latest " + when_text(agg[0].t));
            out.push_back(std::move(c));
        }
    }

    // Rule thermal_shutdown: an unclean stop, graded by whether the record
    // itself says heat.
    {
        auto rows = q(db_, "SELECT CAST(start AS INTEGER), kinds, id, summary FROM events WHERE (kinds & 32)!=0 "
                           "AND start>=? AND start<? ORDER BY start DESC LIMIT 3",
                      {from, to});
        for (const auto& row : rows) {
            Conclusion c{"thermal_shutdown"};
            bool heat = (int64_t(row.a)) & 16;
            c.grade = heat ? Grade::Is : Grade::ConsistentWith;
            c.salience = 3;
            c.specificity = heat ? 3 : 1;
            c.t = row.t;
            c.event_id = int64_t(row.b);
            if (heat)
                c.sentence = "The machine shut down uncleanly on " + when_text(row.t) +
                             " with the temperature at the limit: thermal protection fired.";
            else {
                c.sentence = "The machine stopped uncleanly on " + when_text(row.t) +
                             ", which " + grade_phrase(c.grade) + " a power loss or forced shutdown.";
                c.gap = "the record simply stops; nothing in it says heat";
            }
            c.receipts.push_back(when_text(row.t) + "  " + row.s1);
            out.push_back(std::move(c));
        }
    }

    // Rule oom_kill: the kernel's own verdict, plus the biggest holder at
    // that moment (one hop: points to).
    {
        auto rows = q(db_, "SELECT t,0,0,title FROM log WHERE category='problems' AND t>=? AND t<? AND "
                           "title LIKE 'Out of memory: the kernel killed %' ORDER BY t DESC LIMIT 3",
                      {from, to});
        for (const auto& row : rows) {
            std::string victim = row.s1.substr(std::string("Out of memory: the kernel killed ").size());
            auto holder = q(db_, "SELECT 0, MAX(peak_rss), 0, name FROM procs WHERE started < ? AND "
                                 "IFNULL(ended, ?) >= ? GROUP BY name ORDER BY 2 DESC LIMIT 1",
                            {row.t, row.t, row.t - 600});
            Conclusion c{"oom_kill"};
            c.grade = Grade::PointsTo;
            c.salience = 2.5;
            c.specificity = 2;
            c.t = row.t;
            c.sentence = "Memory ran out on " + when_text(row.t) + " and the kernel killed " + victim + ".";
            if (!holder.empty() && !holder[0].s1.empty() && holder[0].s1 != victim)
                c.sentence += " The biggest memory holder at that moment " + std::string(grade_phrase(Grade::PointsTo)) +
                              " " + holder[0].s1 + " (" + gb(holder[0].a) + ").";
            c.receipts.push_back(when_text(row.t) + "  " + row.s1);
            out.push_back(std::move(c));
        }
    }

    // Rule update_regression: events of a kind born, or doubled, within 48 h
    // of an update whose title names the culprit.
    {
        auto updates = q(db_, "SELECT t,0,0,title FROM log WHERE category='software' AND t>=? AND t<? "
                              "ORDER BY t DESC LIMIT 20",
                         {from - 48 * 3600, to});
        for (const auto& up : updates) {
            auto after = q(db_, "SELECT COUNT(*), MAX(start), 0 FROM events WHERE start > ? AND start <= ?",
                           {up.t, up.t + 48 * 3600});
            auto before = q(db_, "SELECT COUNT(*),0,0 FROM events WHERE start >= ? AND start < ?",
                            {up.t - 7 * 86400, up.t});
            int64_t na = after.empty() ? 0 : after[0].t, nb = before.empty() ? 0 : before[0].t;
            if (na < 2 || na < 2 * nb) continue;
            auto culp = q(db_, "SELECT 0,0,0,c.name FROM events e JOIN event_culprits c ON c.event_id=e.id "
                               "WHERE e.start > ? AND e.start <= ? GROUP BY c.name "
                               "ORDER BY SUM(c.share) DESC LIMIT 1",
                          {up.t, up.t + 48 * 3600});
            std::string culprit = culp.empty() ? "" : culp[0].s1;
            if (culprit.empty() || up.s1.find(culprit) == std::string::npos) continue;
            Conclusion c{"update_regression"};
            c.grade = Grade::PointsTo;
            c.salience = 2;
            c.specificity = 3;
            c.t = up.t;
            c.sentence = "Trouble involving " + culprit + " began within 48 hours of the update on " +
                         when_text(up.t) + " (" + num(na) + " events after, " + num(nb) +
                         " in the week before); that " + grade_phrase(Grade::PointsTo) + " the update.";
            c.receipts.push_back(when_text(up.t) + "  " + up.s1);
            out.push_back(std::move(c));
            break;
        }
    }

    // Rules swap_thrash and swap_parked: the pair ships the exoneration
    // mechanism. Pressure decides; occupancy alone accuses nobody.
    {
        auto swaps = q(db_, "SELECT hour_ts, med, 0 FROM samples_hourly WHERE metric='swap_used' AND "
                            "hour_ts>=? AND hour_ts<? ORDER BY hour_ts",
                       {from, to});
        auto pres = q(db_, "SELECT hour_ts, p95, 0 FROM samples_hourly WHERE metric='pressure' AND "
                           "hour_ts>=? AND hour_ts<? ORDER BY hour_ts",
                      {from, to});
        double pmax = 0;
        for (const auto& p : pres) pmax = std::max(pmax, p.a);
        if (swaps.size() >= 2) {
            double growth = swaps.back().a - swaps.front().a;
            double top = 0;
            for (const auto& s : swaps) top = std::max(top, s.a);
            if (growth > 256e6 && pmax >= 10) {
                auto holder = q(db_, "SELECT 0, MAX(peak_rss), 0, name FROM procs WHERE started < ? AND "
                                     "IFNULL(ended, ?) >= ? GROUP BY name ORDER BY 2 DESC LIMIT 1",
                                {to, to, from});
                Conclusion c{"swap_thrash"};
                c.grade = Grade::Is;
                c.salience = 2;
                c.specificity = 2;
                c.t = swaps.back().t;
                c.sentence = "Memory was thrashing: swap grew by " + gb(growth) +
                             " while programs waited on memory (pressure peaked at " +
                             std::to_string(int(pmax)) + "%)." +
                             (holder.empty() || holder[0].s1.empty()
                                  ? ""
                                  : " The biggest memory holder was " + holder[0].s1 + " (" + gb(holder[0].a) + ").");
                c.receipts.push_back("swap " + gb(swaps.front().a) + " to " + gb(swaps.back().a) +
                                     ", pressure p95 " + std::to_string(int(pmax)) + "%");
                out.push_back(std::move(c));
            } else if (top > 1e9 && pmax < 1) {
                Conclusion c{"swap_parked"};
                c.grade = Grade::Is;
                c.salience = 1;
                c.specificity = 2;
                c.t = swaps.back().t;
                c.sentence = "Swap holds " + gb(top) + " but nothing is waiting on memory: those are parked "
                             "pages, not a problem.";
                c.receipts.push_back("swap occupancy high, memory pressure p95 under 1%");
                out.push_back(std::move(c));
            }
        }
    }

    // Rule leak: one program's daily peak memory rising day after day,
    // resetting only on restart.
    {
        auto rows = q(db_, "SELECT 0, MAX(peak_rss), 0, name, date(started,'unixepoch','localtime') FROM procs "
                           "WHERE started >= ? GROUP BY name, date(started,'unixepoch','localtime') "
                           "ORDER BY name, 5",
                      {now - 14 * 86400});
        std::map<std::string, std::vector<double>> series;
        for (const auto& row : rows) series[row.s1].push_back(row.a);
        for (const auto& [name, vals] : series) {
            if (vals.size() < 3 || vals.front() < 50e6) continue;
            bool rising = true;
            for (std::size_t i = 1; i < vals.size(); ++i) rising &= vals[i] >= vals[i - 1] * 1.05;
            if (!rising) continue;
            Conclusion c{"leak"};
            c.grade = vals.size() >= 5 ? Grade::PointsTo : Grade::ConsistentWith;
            c.salience = 2;
            c.specificity = 2;
            c.t = now;
            c.sentence = name + "'s peak memory has risen every day for " + num(int64_t(vals.size())) +
                         " days (" + gb(vals.front()) + " to " + gb(vals.back()) + "), which " +
                         grade_phrase(c.grade) + " a memory leak.";
            if (c.grade == Grade::ConsistentWith) c.gap = "three days is a short series; five would firm it up";
            c.receipts.push_back(name + " daily peaks: " + gb(vals.front()) + " .. " + gb(vals.back()));
            out.push_back(std::move(c));
            break;
        }
    }

    // Rule time_to_full: project the root filesystem's fill rate.
    {
        auto rows = q(db_, "SELECT hour_ts, med, 0 FROM samples_hourly WHERE metric='disk_free_pct' AND "
                           "med >= 0 AND hour_ts >= ? ORDER BY hour_ts",
                      {now - 7 * 86400});
        if (rows.size() >= 12) {
            std::vector<double> series;
            for (const auto& row : rows) series.push_back(row.a);
            auto tr = judge::theil_sen_mk(series);
            if (tr.slope_per_step < -1e-4) {
                double hours_left = series.back() / -tr.slope_per_step;
                if (hours_left < 14 * 24) {
                    auto writer = q(db_, "SELECT 0, SUM(io_bytes), 0, name FROM minute_top WHERE minute >= ? "
                                         "GROUP BY name ORDER BY 2 DESC LIMIT 1",
                                    {now - 86400});
                    Conclusion c{"time_to_full"};
                    c.grade = rows.size() >= 48 ? Grade::PointsTo : Grade::ConsistentWith;
                    c.salience = 2;
                    c.specificity = 2;
                    c.t = rows.back().t;
                    char eta[64];
                    if (hours_left < 48) std::snprintf(eta, sizeof eta, "about %.0f hours", hours_left);
                    else std::snprintf(eta, sizeof eta, "about %.0f days", hours_left / 24);
                    c.sentence = "At the current rate the root disk is full in " + std::string(eta) + " (" +
                                 std::to_string(int(series.back())) + "% free and falling)." +
                                 (writer.empty() || writer[0].s1.empty()
                                      ? ""
                                      : " The biggest writer in the last day was " + writer[0].s1 + ".");
                    if (c.grade == Grade::ConsistentWith)
                        c.gap = "under two days of fill-rate history; the projection is rough";
                    c.receipts.push_back("free " + std::to_string(int(series.front())) + "% to " +
                                         std::to_string(int(series.back())) + "% across " +
                                         std::to_string(rows.size()) + " hours");
                    out.push_back(std::move(c));
                }
            }
        }
    }

    // Rule disk_wait: a "slow" event that was really the disk.
    {
        auto rows = q(db_, "SELECT CAST(start AS INTEGER), id, peak_disk, summary FROM events WHERE (kinds&4)!=0 "
                           "AND peak_cpu < 40 AND peak_disk > 50e6 AND start>=? AND start<? ORDER BY start DESC LIMIT 3",
                      {from, to});
        for (const auto& row : rows) {
            auto culp = q(db_, "SELECT 0,0,0,name FROM event_culprits WHERE event_id=? ORDER BY io_bytes DESC LIMIT 1",
                          {int64_t(row.a)});
            Conclusion c{"disk_wait"};
            c.grade = Grade::PointsTo;
            c.salience = 1.5;
            c.specificity = 2;
            c.t = row.t;
            c.event_id = int64_t(row.a);
            c.sentence = "The machine was not out of processor on " + when_text(row.t) +
                         "; it was waiting on the disk (" + gb(row.b) + "/s)" +
                         (culp.empty() || culp[0].s1.empty() ? "." : ", and the traffic " +
                          std::string(grade_phrase(Grade::PointsTo)) + " " + culp[0].s1 + ".");
            c.receipts.push_back(when_text(row.t) + "  " + row.s1);
            out.push_back(std::move(c));
        }
    }

    // Rule battery_aged (live, Is): fired before any drain hunt, so a worn
    // battery is never blamed on software.
    if (live && live->power.battery_health.ok() && live->power.battery_health.get() < 80) {
        Conclusion c{"battery_aged"};
        c.grade = Grade::Is;
        c.salience = 3;
        c.specificity = 1;
        c.t = now;
        c.sentence = "The battery holds only " + std::to_string(int(live->power.battery_health.get())) +
                     "% of its design capacity: shorter runtimes are the battery aging, not software.";
        c.receipts.push_back("full charge vs design capacity, read from the battery now");
        out.push_back(std::move(c));
    }

    // Order: salience, then specificity, then recency; one per (rule, scope).
    std::stable_sort(out.begin(), out.end(), [](const Conclusion& a, const Conclusion& b) {
        if (a.salience != b.salience) return a.salience > b.salience;
        if (a.specificity != b.specificity) return a.specificity > b.specificity;
        return a.t > b.t;
    });
    std::vector<Conclusion> kept;
    for (auto& c : out) {
        bool dup = false;
        for (const auto& k : kept) dup |= k.rule_id == c.rule_id && k.event_id == c.event_id;
        if (!dup) kept.push_back(std::move(c));
    }
    return kept;
}

std::vector<Conclusion> Diagnose::for_event(int64_t event_id, int64_t now) {
    auto ev = q(db_, "SELECT CAST(start AS INTEGER), \"end\", 0 FROM events WHERE id=?", {event_id});
    if (ev.empty()) return {};
    int64_t from = ev[0].t - 3600, to = int64_t(ev[0].a) + 3600;
    auto all = run(from, to, now, nullptr);
    std::vector<Conclusion> kept;
    for (auto& c : all)
        if (c.event_id == event_id || c.event_id == 0) kept.push_back(std::move(c));
    return kept;
}

}  // namespace as
