#include "store.h"
#include "fs.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cstdlib>

namespace as {

namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt& bind(int i, double v) { sqlite3_bind_double(s, i, v); return *this; }
    Stmt& bind(int i, int64_t v) { sqlite3_bind_int64(s, i, v); return *this; }
    Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), int(v.size()), SQLITE_TRANSIENT); return *this; }
    bool step() { return sqlite3_step(s) == SQLITE_ROW; }
    void run() { sqlite3_step(s); sqlite3_reset(s); }
    double d(int c) const { return sqlite3_column_double(s, c); }
    int64_t i(int c) const { return sqlite3_column_int64(s, c); }
    std::string t(int c) const {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(s, c));
        return p ? p : "";
    }
};

void make_dirs(const std::string& path) {
    for (std::size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
}

}  // namespace

std::string default_db_path() {
    if (const char* over = std::getenv("AS_DB"); over && *over) return over;
    const char* data = std::getenv("XDG_DATA_HOME");
    std::string base = data && *data ? data : std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/.local/share";
    return base + "/activityscope/history.db";
}

Store::Store(std::string path) {
    make_dirs(path);
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        error_ = db_ ? sqlite3_errmsg(db_) : "cannot open database";
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    ::chmod(path.c_str(), 0600);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec(R"(CREATE TABLE IF NOT EXISTS samples(
        t REAL PRIMARY KEY, cpu REAL, kernel REAL, mem_pct REAL, swap_used REAL, pressure REAL,
        disk_r REAL, disk_w REAL, net_rx REAL, net_tx REAL, gpu REAL, gpu_mem REAL,
        temp REAL, fan REAL, power REAL, freq REAL, procs INTEGER, cores TEXT))");
    exec(R"(CREATE TABLE IF NOT EXISTS minute_top(
        minute INTEGER, pid INTEGER, start_ticks INTEGER, name TEXT, cmdline TEXT, unit TEXT,
        cpu_seconds REAL, peak_rss REAL, io_bytes REAL, exited INTEGER))");
    exec("CREATE INDEX IF NOT EXISTS minute_top_minute ON minute_top(minute)");
    exec(R"(CREATE TABLE IF NOT EXISTS events(
        id INTEGER PRIMARY KEY, start REAL, end REAL, kinds INTEGER, primary_kind INTEGER,
        triggers TEXT, summary TEXT, peak_cpu REAL, peak_mem REAL, peak_disk REAL,
        peak_gpu REAL, peak_temp REAL, peak_fan REAL, total_cpu_seconds REAL))");
    exec("CREATE INDEX IF NOT EXISTS events_start ON events(start)");
    exec(R"(CREATE TABLE IF NOT EXISTS event_culprits(
        event_id INTEGER, rank INTEGER, kind TEXT, name TEXT, cmdline TEXT, pid INTEGER,
        start_ticks INTEGER, ppid INTEGER, parent TEXT, unit TEXT, exited INTEGER, count INTEGER,
        cpu_seconds REAL, share REAL, peak_rss REAL, rss_growth REAL, io_bytes REAL, gpu_mem REAL))");
    exec("CREATE INDEX IF NOT EXISTS event_culprits_event ON event_culprits(event_id)");
    exec("CREATE TABLE IF NOT EXISTS boots_seen(boot_id TEXT PRIMARY KEY)");
    exec("CREATE TABLE IF NOT EXISTS log(t REAL, category TEXT, title TEXT, detail TEXT)");
    exec("CREATE INDEX IF NOT EXISTS log_t ON log(t)");
    exec("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT)");
    exec("CREATE TABLE IF NOT EXISTS seen_exes(exe TEXT PRIMARY KEY)");
    exec(R"(CREATE TABLE IF NOT EXISTS procs(
        pid INTEGER, started REAL, ended REAL, name TEXT, exe TEXT, cmdline TEXT, user TEXT, unit TEXT,
        ppid INTEGER, parent TEXT, cpu_seconds REAL, peak_rss REAL, UNIQUE(pid, started)))");
    exec("CREATE INDEX IF NOT EXISTS procs_started ON procs(started)");
    exec("CREATE INDEX IF NOT EXISTS procs_ended ON procs(ended)");
    exec(R"(CREATE TABLE IF NOT EXISTS connections(
        program TEXT, exe TEXT, remote TEXT, port INTEGER, proto TEXT, first_seen REAL, last_seen REAL,
        UNIQUE(program, remote, port, proto)))");
    exec("CREATE INDEX IF NOT EXISTS connections_seen ON connections(first_seen)");
    // Everything in time order, for "what happened at 14:32?" in one query.
    exec(R"(CREATE TABLE IF NOT EXISTS event_samples(
        event_id INTEGER, t REAL, cpu REAL, mem_pct REAL, pressure REAL, disk_bps REAL,
        gpu REAL, temp REAL, fan REAL))");
    exec("CREATE INDEX IF NOT EXISTS event_samples_event ON event_samples(event_id)");
    // Columns added after the first release; each ALTER is a no-op when present.
    for (const char* add : {"ALTER TABLE event_culprits ADD COLUMN faults REAL", "ALTER TABLE samples ADD COLUMN guard REAL",
                            "ALTER TABLE samples ADD COLUMN guard_limit REAL", "ALTER TABLE events ADD COLUMN min_margin REAL",
                            "ALTER TABLE events ADD COLUMN min_seconds REAL", "ALTER TABLE events ADD COLUMN guard_limit REAL",
                            "ALTER TABLE events ADD COLUMN guard_zone TEXT", "ALTER TABLE events ADD COLUMN dead_fans TEXT",
                            "ALTER TABLE event_samples ADD COLUMN guard REAL", "ALTER TABLE events ADD COLUMN notable INTEGER",
                            "ALTER TABLE procs ADD COLUMN end_estimated INTEGER"})
        sqlite3_exec(db_, add, nullptr, nullptr, nullptr);
    // The view names columns the ALTERs add, so it is built after them.
    exec("DROP VIEW IF EXISTS v_timeline");
    exec(R"(CREATE VIEW v_timeline AS
        SELECT t, 'log: ' || category AS kind, title, detail FROM log
        UNION ALL SELECT start, 'resource event', summary, triggers FROM events
        UNION ALL SELECT started, 'program started', name || ' (PID ' || pid || ', from ' || IFNULL(parent, '?') || ')', cmdline
                  FROM procs WHERE ended IS NULL OR NOT (ended - started < 10 AND cpu_seconds < 0.1)
        UNION ALL SELECT ended, 'program ended', name || ' (PID ' || pid || ')',
                         printf('ran %.0f s, used %.1f s of CPU, peak memory %.0f MB', ended - started, cpu_seconds, peak_rss / 1e6)
                         || CASE WHEN IFNULL(end_estimated, 0) = 1 THEN ' (end estimated: recording stopped)' ELSE '' END
                  FROM procs WHERE ended IS NOT NULL AND NOT (ended - started < 10 AND cpu_seconds < 0.1)
        UNION ALL SELECT first_seen, 'first connection', program || ' to ' || remote || ':' || port, proto || ', ' || IFNULL(exe, '') FROM connections)");
}

Store::~Store() {
    if (!db_) return;
    flush();
    sqlite3_close(db_);
}

void Store::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK && err) {
        error_ = err;
        sqlite3_free(err);
    }
}

void Store::add_sample(const Snapshot& s) {
    StoredSample r;
    r.t = s.wall;
    r.cpu = s.cpu.total;
    r.kernel = s.cpu.kernel;
    r.mem_pct = s.mem.percent();
    r.swap_used = double(s.mem.swap_total - std::min(s.mem.swap_total, s.mem.swap_free));
    r.pressure = s.mem.pressure_some10.or_else(0);
    for (const auto& d : s.disks) {
        r.disk_r += d.read_bps;
        r.disk_w += d.write_bps;
    }
    for (const auto& n : s.nets) {
        if (n.kind == "Loopback" || n.kind == "Virtual") continue;
        r.net_rx += n.rx_bps;
        r.net_tx += n.tx_bps;
    }
    for (const auto& g : s.gpus) {
        if (g.util.ok()) r.gpu = std::max(r.gpu, g.util.get());
        if (g.mem_used.ok()) r.gpu_mem = std::max(r.gpu_mem, g.mem_used.get());
    }
    r.temp = s.cpu.temp_c.or_else(-1);
    for (const auto& f : s.power.fans)
        if (f.percent.ok()) r.fan = std::max(r.fan, double(f.percent.get()));
    r.power = s.power.battery_w.or_else(-1);
    r.freq = s.cpu.freq_mhz.or_else(-1);
    r.guard = s.guard.raw.or_else(-1);  // the history keeps what the sensor really said
    r.guard_limit = s.guard.limit.or_else(-1);
    r.procs = int(s.procs.size());
    for (std::size_t i = 0; i < s.cpu.per_core.size(); ++i) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%s%.0f", i ? "," : "", s.cpu.per_core[i]);
        r.cores += buf;
    }
    pending_.push_back(std::move(r));
}

void Store::flush() {
    if (!db_ || pending_.empty()) return;
    exec("BEGIN");
    {
        Stmt st(db_, "INSERT OR REPLACE INTO samples(t,cpu,kernel,mem_pct,swap_used,pressure,disk_r,disk_w,net_rx,net_tx,gpu,"
                     "gpu_mem,temp,fan,power,freq,procs,cores,guard,guard_limit) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        for (const auto& r : pending_) {
            st.bind(1, r.t).bind(2, r.cpu).bind(3, r.kernel).bind(4, r.mem_pct).bind(5, r.swap_used)
                .bind(6, r.pressure).bind(7, r.disk_r).bind(8, r.disk_w).bind(9, r.net_rx).bind(10, r.net_tx)
                .bind(11, r.gpu).bind(12, r.gpu_mem).bind(13, r.temp).bind(14, r.fan).bind(15, r.power)
                .bind(16, r.freq).bind(17, int64_t(r.procs)).bind(18, r.cores).bind(19, r.guard).bind(20, r.guard_limit);
            st.run();
        }
    }
    exec("COMMIT");
    pending_.clear();
}

void Store::add_minute(const MinuteTop& m) {
    if (!db_) return;
    exec("BEGIN");
    {
        Stmt st(db_, "INSERT INTO minute_top VALUES(?,?,?,?,?,?,?,?,?,?)");
        for (const auto& c : m.top) {
            st.bind(1, m.minute).bind(2, int64_t(c.pid)).bind(3, int64_t(c.start_ticks)).bind(4, c.name)
                .bind(5, c.cmdline).bind(6, c.unit).bind(7, c.cpu_seconds).bind(8, double(c.peak_rss))
                .bind(9, c.io_bytes).bind(10, int64_t(c.exited));
            st.run();
        }
    }
    exec("COMMIT");
}

int64_t Store::add_event(const Event& e) {
    if (!db_) return 0;
    std::string triggers;
    for (const auto& t : e.triggers) triggers += (triggers.empty() ? "" : "\n") + t;
    exec("BEGIN");
    {
        std::string fans;
        for (const auto& f : e.dead_fans) fans += (fans.empty() ? "" : "\n") + f;
        Stmt st(db_, "INSERT INTO events(start,end,kinds,primary_kind,triggers,summary,peak_cpu,peak_mem,peak_disk,"
                     "peak_gpu,peak_temp,peak_fan,total_cpu_seconds,min_margin,min_seconds,guard_limit,guard_zone,dead_fans,notable) "
                     "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        st.bind(14, e.min_margin).bind(15, e.min_seconds).bind(16, e.guard_limit).bind(17, e.guard_zone).bind(18, fans);
        st.bind(19, int64_t(e.notable));
        st.bind(1, e.start).bind(2, e.end).bind(3, int64_t(e.kinds)).bind(4, int64_t(e.primary)).bind(5, triggers)
            .bind(6, e.summary).bind(7, e.peak_cpu).bind(8, e.peak_mem_pct).bind(9, e.peak_disk_bps)
            .bind(10, e.peak_gpu).bind(11, e.peak_temp).bind(12, e.peak_fan).bind(13, e.total_cpu_seconds);
        st.run();
    }
    int64_t id = sqlite3_last_insert_rowid(db_);
    {
        Stmt st(db_, "INSERT INTO event_culprits(event_id,rank,kind,name,cmdline,pid,start_ticks,ppid,parent,unit,exited,count,"
                     "cpu_seconds,share,peak_rss,rss_growth,io_bytes,gpu_mem,faults) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        int rank = 0;
        for (const auto& c : e.culprits) {
            st.bind(1, id).bind(2, int64_t(rank++)).bind(3, c.kind).bind(4, c.name).bind(5, c.cmdline)
                .bind(6, int64_t(c.pid)).bind(7, int64_t(c.start_ticks)).bind(8, int64_t(c.ppid)).bind(9, c.parent)
                .bind(10, c.unit).bind(11, int64_t(c.exited)).bind(12, int64_t(c.count)).bind(13, c.cpu_seconds)
                .bind(14, c.share).bind(15, double(c.peak_rss)).bind(16, double(c.rss_growth)).bind(17, c.io_bytes)
                .bind(18, c.gpu_mem).bind(19, double(c.faults));
            st.run();
        }
    }
    {
        Stmt st(db_, "INSERT INTO event_samples(event_id,t,cpu,mem_pct,pressure,disk_bps,gpu,temp,fan,guard) VALUES(?,?,?,?,?,?,?,?,?,?)");
        for (const auto& p : e.samples) {
            st.bind(1, id).bind(2, p.t).bind(3, p.cpu).bind(4, p.mem_pct).bind(5, p.pressure).bind(6, p.disk_bps)
                .bind(7, p.gpu).bind(8, p.temp).bind(9, p.fan).bind(10, p.guard);
            st.run();
        }
    }
    exec("COMMIT");
    return id;
}

void Store::prune(double now, double detail_hours, double event_days) {
    if (!db_) return;
    double detail = now - detail_hours * 3600;
    double events = now - event_days * 86400;
    Stmt(db_, "DELETE FROM samples WHERE t < ?").bind(1, detail).run();
    Stmt(db_, "DELETE FROM minute_top WHERE minute < ?").bind(1, detail).run();
    Stmt(db_, "DELETE FROM event_culprits WHERE event_id IN (SELECT id FROM events WHERE start < ?)").bind(1, events).run();
    Stmt(db_, "DELETE FROM event_samples WHERE event_id IN (SELECT id FROM events WHERE start < ?)").bind(1, events).run();
    Stmt(db_, "DELETE FROM events WHERE start < ?").bind(1, events).run();
    exec("PRAGMA wal_checkpoint(TRUNCATE)");
}

std::vector<StoredSample> Store::samples(double from, double to) const {
    std::vector<StoredSample> out;
    if (!db_) return out;
    Stmt st(db_, "SELECT t,cpu,kernel,mem_pct,swap_used,pressure,disk_r,disk_w,net_rx,net_tx,gpu,gpu_mem,temp,fan,power,freq,"
                 "procs,cores,IFNULL(guard,-1),IFNULL(guard_limit,-1) FROM samples WHERE t >= ? AND t <= ? ORDER BY t");
    st.bind(1, from).bind(2, to);
    while (st.step()) {
        StoredSample r;
        r.t = st.d(0); r.cpu = st.d(1); r.kernel = st.d(2); r.mem_pct = st.d(3); r.swap_used = st.d(4);
        r.pressure = st.d(5); r.disk_r = st.d(6); r.disk_w = st.d(7); r.net_rx = st.d(8); r.net_tx = st.d(9);
        r.gpu = st.d(10); r.gpu_mem = st.d(11); r.temp = st.d(12); r.fan = st.d(13); r.power = st.d(14);
        r.freq = st.d(15); r.procs = int(st.i(16)); r.cores = st.t(17); r.guard = st.d(18); r.guard_limit = st.d(19);
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<EventRow> Store::events(double from, double to) const {
    std::vector<EventRow> out;
    if (!db_) return out;
    Stmt st(db_, "SELECT id,start,end,kinds,primary_kind,triggers,summary,peak_cpu,peak_mem,peak_disk,peak_gpu,"
                 "peak_temp,peak_fan,IFNULL(min_margin,-1),IFNULL(notable,1) FROM events WHERE end >= ? AND start <= ? ORDER BY start DESC");
    st.bind(1, from).bind(2, to);
    while (st.step()) {
        EventRow r;
        r.id = st.i(0); r.start = st.d(1); r.end = st.d(2); r.kinds = unsigned(st.i(3)); r.primary = unsigned(st.i(4));
        r.triggers = st.t(5); r.summary = st.t(6); r.peak_cpu = st.d(7); r.peak_mem = st.d(8); r.peak_disk = st.d(9);
        r.peak_gpu = st.d(10); r.peak_temp = st.d(11); r.peak_fan = st.d(12); r.min_margin = st.d(13);
        r.notable = st.i(14) != 0;
        out.push_back(std::move(r));
    }
    return out;
}

Event Store::event(int64_t id) const {
    Event e;
    if (!db_) return e;
    {
        Stmt st(db_, "SELECT start,end,kinds,primary_kind,triggers,summary,peak_cpu,peak_mem,peak_disk,peak_gpu,"
                     "peak_temp,peak_fan,total_cpu_seconds,IFNULL(min_margin,-1),IFNULL(min_seconds,-1),IFNULL(guard_limit,-1),"
                     "IFNULL(guard_zone,''),IFNULL(dead_fans,'') FROM events WHERE id=?");
        st.bind(1, id);
        if (!st.step()) return e;
        e.id = id; e.start = st.d(0); e.end = st.d(1); e.kinds = unsigned(st.i(2)); e.primary = unsigned(st.i(3));
        e.triggers = split(st.t(4), '\n'); e.summary = st.t(5); e.peak_cpu = st.d(6); e.peak_mem_pct = st.d(7);
        e.peak_disk_bps = st.d(8); e.peak_gpu = st.d(9); e.peak_temp = st.d(10); e.peak_fan = st.d(11);
        e.total_cpu_seconds = st.d(12); e.open = false;
        e.min_margin = st.d(13); e.min_seconds = st.d(14); e.guard_limit = st.d(15); e.guard_zone = st.t(16);
        if (!st.t(17).empty()) e.dead_fans = split(st.t(17), '\n');
    }
    {
        Stmt st(db_, "SELECT kind,name,cmdline,pid,start_ticks,ppid,parent,unit,exited,count,cpu_seconds,share,"
                     "peak_rss,rss_growth,io_bytes,gpu_mem,IFNULL(faults,0) FROM event_culprits WHERE event_id=? ORDER BY rank");
        st.bind(1, id);
        while (st.step()) {
            Culprit c;
            c.kind = st.t(0); c.name = st.t(1); c.cmdline = st.t(2); c.pid = int(st.i(3));
            c.start_ticks = uint64_t(st.i(4)); c.ppid = int(st.i(5)); c.parent = st.t(6); c.unit = st.t(7);
            c.exited = st.i(8) != 0; c.count = int(st.i(9)); c.cpu_seconds = st.d(10); c.share = st.d(11);
            c.peak_rss = uint64_t(st.d(12)); c.rss_growth = int64_t(st.d(13)); c.io_bytes = st.d(14); c.gpu_mem = st.d(15);
            c.faults = uint64_t(st.d(16));
            e.culprits.push_back(std::move(c));
        }
    }
    {
        Stmt st(db_, "SELECT t,cpu,mem_pct,pressure,disk_bps,gpu,temp,fan,IFNULL(guard,-1) FROM event_samples WHERE event_id=? ORDER BY t");
        st.bind(1, id);
        while (st.step()) {
            EventSample p;
            p.t = st.d(0); p.cpu = st.d(1); p.mem_pct = st.d(2); p.pressure = st.d(3); p.disk_bps = st.d(4);
            p.gpu = st.d(5); p.temp = st.d(6); p.fan = st.d(7); p.guard = st.d(8);
            e.samples.push_back(p);
        }
    }
    return e;
}

std::vector<Culprit> Store::minute_top(double from, double to) const {
    std::vector<Culprit> out;
    if (!db_) return out;
    Stmt st(db_, "SELECT pid,start_ticks,name,cmdline,unit,SUM(cpu_seconds),MAX(peak_rss),SUM(io_bytes),MAX(exited) "
                 "FROM minute_top WHERE minute >= ? AND minute <= ? GROUP BY pid,start_ticks "
                 "ORDER BY SUM(cpu_seconds) DESC LIMIT 25");
    st.bind(1, int64_t(from) / 60 * 60).bind(2, to);
    while (st.step()) {
        Culprit c;
        c.pid = int(st.i(0)); c.start_ticks = uint64_t(st.i(1)); c.name = st.t(2); c.cmdline = st.t(3);
        c.unit = st.t(4); c.cpu_seconds = st.d(5); c.peak_rss = uint64_t(st.d(6)); c.io_bytes = st.d(7);
        c.exited = st.i(8) != 0;
        out.push_back(std::move(c));
    }
    return out;
}

StoredSample Store::last_sample_before(double t) const {
    StoredSample r;
    r.t = -1;
    if (!db_) return r;
    Stmt st(db_, "SELECT t,cpu,mem_pct,temp,fan,IFNULL(guard,-1),IFNULL(guard_limit,-1) FROM samples WHERE t <= ? ORDER BY t DESC LIMIT 1");
    st.bind(1, t);
    if (st.step()) {
        r.t = st.d(0); r.cpu = st.d(1); r.mem_pct = st.d(2); r.temp = st.d(3); r.fan = st.d(4);
        r.guard = st.d(5); r.guard_limit = st.d(6);
    }
    return r;
}

bool Store::boot_seen(const std::string& id) const {
    if (!db_) return true;
    Stmt st(db_, "SELECT 1 FROM boots_seen WHERE boot_id=?");
    st.bind(1, id);
    return st.step();
}

void Store::mark_boot(const std::string& id) {
    if (!db_) return;
    Stmt(db_, "INSERT OR IGNORE INTO boots_seen VALUES(?)").bind(1, id).run();
}

void Store::add_log(const std::vector<LogEntry>& entries) {
    if (!db_ || entries.empty()) return;
    exec("BEGIN");
    {
        Stmt st(db_, "INSERT INTO log VALUES(?,?,?,?)");
        for (const auto& e : entries) {
            st.bind(1, e.t).bind(2, e.category).bind(3, e.title).bind(4, e.detail);
            st.run();
        }
    }
    exec("COMMIT");
}

std::vector<LogEntry> Store::log(double from, double to, const std::string& search, int limit) const {
    std::vector<LogEntry> out;
    if (!db_) return out;
    Stmt st(db_, "SELECT t,category,title,detail FROM log WHERE t >= ? AND t <= ? AND (? = '' OR title LIKE ? OR detail LIKE ?) "
                 "ORDER BY t DESC LIMIT ?");
    std::string like = "%" + search + "%";
    st.bind(1, from).bind(2, to).bind(3, search).bind(4, like).bind(5, like).bind(6, int64_t(limit));
    while (st.step()) out.push_back({st.d(0), st.t(1), st.t(2), st.t(3)});
    return out;
}

std::string Store::meta(const std::string& key) const {
    if (!db_) return {};
    Stmt st(db_, "SELECT value FROM meta WHERE key=?");
    st.bind(1, key);
    return st.step() ? st.t(0) : std::string();
}

void Store::set_meta(const std::string& key, const std::string& value) {
    if (!db_) return;
    Stmt(db_, "INSERT OR REPLACE INTO meta VALUES(?,?)").bind(1, key).bind(2, value).run();
}

bool Store::exe_seen(const std::string& exe) const {
    if (!db_) return true;
    Stmt st(db_, "SELECT 1 FROM seen_exes WHERE exe=?");
    st.bind(1, exe);
    return st.step();
}

void Store::mark_exe(const std::string& exe) {
    if (!db_) return;
    Stmt(db_, "INSERT OR IGNORE INTO seen_exes VALUES(?)").bind(1, exe).run();
}

void Store::prune_log(double now, double days) {
    if (!db_) return;
    Stmt(db_, "DELETE FROM log WHERE t < ?").bind(1, now - days * 86400).run();
    // Program and connection history are kept 30 days.
    Stmt(db_, "DELETE FROM procs WHERE COALESCE(ended, started) < ?").bind(1, now - 30 * 86400.0).run();
    Stmt(db_, "DELETE FROM connections WHERE last_seen < ?").bind(1, now - 30 * 86400.0).run();
}

void Store::add_procs(const std::vector<ProcessRecord>& records) {
    if (!db_ || records.empty()) return;
    exec("BEGIN");
    {
        Stmt st(db_, "INSERT INTO procs(pid,started,ended,name,exe,cmdline,user,unit,ppid,parent,cpu_seconds,peak_rss) "
                     "VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(pid, started) DO UPDATE SET "
                     "ended = COALESCE(excluded.ended, ended), cpu_seconds = excluded.cpu_seconds, "
                     "peak_rss = MAX(peak_rss, excluded.peak_rss)");
        for (const auto& r : records) {
            st.bind(1, int64_t(r.pid)).bind(2, r.started);
            if (r.ended > 0) st.bind(3, r.ended);
            else sqlite3_bind_null(st.s, 3);
            st.bind(4, r.name).bind(5, r.exe).bind(6, r.cmdline).bind(7, r.user).bind(8, r.unit).bind(9, int64_t(r.ppid))
                .bind(10, r.parent).bind(11, r.cpu_seconds).bind(12, double(r.peak_rss));
            st.run();
        }
    }
    exec("COMMIT");
}

double Store::last_sample_time() const {
    if (!db_) return 0;
    Stmt st(db_, "SELECT IFNULL(max(t), 0) FROM samples");
    return st.step() ? st.d(0) : 0;
}

int Store::close_stale_procs(const std::vector<ProcessRecord>& live, double fallback_end) {
    // Rows left open by a crash, a power loss or an old quit would say
    // "still running" forever. Anything open that does not match a process
    // alive right now gets an honest, estimated end.
    if (!db_ || fallback_end <= 0) return 0;
    std::vector<std::pair<int64_t, double>> open;
    {
        Stmt st(db_, "SELECT pid, started FROM procs WHERE ended IS NULL");
        while (st.step()) open.emplace_back(st.i(0), st.d(1));
    }
    int closed = 0;
    exec("BEGIN");
    {
        Stmt up(db_, "UPDATE procs SET ended = ?, end_estimated = 1 WHERE pid = ? AND started = ? AND ended IS NULL");
        for (const auto& [pid, started] : open) {
            bool alive = false;
            for (const auto& l : live)
                if (l.pid == pid && std::abs(l.started - started) < 2) alive = true;
            if (alive) continue;
            up.bind(1, std::max(fallback_end, started)).bind(2, pid).bind(3, started);
            up.run();
            ++closed;
        }
    }
    exec("COMMIT");
    return closed;
}

std::vector<ConnectionRecord> Store::add_connections(const std::vector<ConnectionRecord>& records) {
    std::vector<ConnectionRecord> fresh;
    if (!db_ || records.empty()) return fresh;
    exec("BEGIN");
    {
        Stmt seen(db_, "SELECT 1 FROM connections WHERE program=? AND remote=? AND port=? AND proto=?");
        Stmt up(db_, "INSERT INTO connections VALUES(?,?,?,?,?,?,?) ON CONFLICT(program, remote, port, proto) DO UPDATE SET "
                     "last_seen = excluded.last_seen, exe = COALESCE(NULLIF(excluded.exe, ''), exe)");
        for (const auto& r : records) {
            if (r.is_new) {
                seen.bind(1, r.program).bind(2, r.remote).bind(3, int64_t(r.port)).bind(4, r.proto);
                if (!seen.step()) fresh.push_back(r);
                sqlite3_reset(seen.s);
            }
            up.bind(1, r.program).bind(2, r.exe).bind(3, r.remote).bind(4, int64_t(r.port)).bind(5, r.proto).bind(6, r.first_seen).bind(7, r.last_seen);
            up.run();
        }
    }
    exec("COMMIT");
    return fresh;
}

}  // namespace as
