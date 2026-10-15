#include "askcore.h"

#include <sqlite3.h>

#include <cstdio>
#include <ctime>

#include "events.h"

namespace as {

using unllm::Clause;
using unllm::Intent;
using unllm::NP;
using unllm::Tense;

namespace {

std::string when_text(int64_t t) {
    std::time_t tt = t;
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%a %H:%M", &tm);
    return buf;
}

struct Row {
    int64_t t = 0;
    double a = 0, b = 0;
    std::string s1, s2;
};

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

// The top name among the culprits of the matching events, by summed share.
std::string top_culprit(sqlite3* db, int64_t from, int64_t to, unsigned mask,
                        const std::string& program) {
    std::string sql =
        // Weighted by each event's total CPU seconds: one long render beats
        // many small blips.
        "SELECT 0, SUM(c.share * IFNULL(e.total_cpu_seconds, 1)), 0, c.name "
        "FROM events e JOIN event_culprits c ON c.event_id = e.id "
        "WHERE e.start < ? AND e.\"end\" >= ? AND (e.kinds & ?) != 0 ";
    if (!program.empty()) sql += "AND c.name = ? ";
    sql += "GROUP BY c.name ORDER BY SUM(c.share * IFNULL(e.total_cpu_seconds, 1)) DESC LIMIT 1";
    auto rows = query(db, sql, {to, from, int64_t(mask)}, program.empty() ? std::vector<std::string>{} : std::vector<std::string>{program});
    return rows.empty() ? "" : rows[0].s1;
}

}  // namespace

Ask::~Ask() {
    if (db_) sqlite3_close(db_);
}

bool Ask::open(const std::string& db_path) {
    if (sqlite3_open_v2(db_path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    // The gazetteer: every program the machine has ever seen run.
    std::vector<std::string> programs;
    for (const auto& r : query(db_, "SELECT 0,0,0,name FROM procs GROUP BY name ORDER BY COUNT(*) DESC LIMIT 400", {}))
        programs.push_back(r.s1);
    an_.set_programs(programs);
    // The retrieval index: event summaries and log lines.
    for (const auto& r : query(db_, "SELECT start,0,0,summary FROM events", {}))
        index_.add(0, r.t, "event", r.s1);
    for (const auto& r : query(db_, "SELECT t,0,0,category,title FROM log", {}))
        index_.add(0, r.t, r.s1, r.s2);
    return true;
}

void Ask::load_language(const std::string& synonyms_path, const std::string& vectors_path) {
    auto syn = unllm::load_synonyms(synonyms_path);
    if (!syn.empty()) an_.set_synonyms(std::move(syn));
    if (vectors_.load(vectors_path)) an_.set_vectors(&vectors_);
}

AskResult Ask::ask(const std::string& question, int64_t now) {
    AskResult res;
    if (!db_) {
        res.answer = "I can't answer that: the history database is not open.";
        res.refused = true;
        return res;
    }
    unllm::Reading r = an_.read(question, now, has_last_ ? &last_ : nullptr);
    last_ = r;
    has_last_ = true;

    // Understanding under 0.6 means guessing, and guessing is the failure
    // mode this tool exists to not have: say so instead.
    if (r.domain < 0.6) {
        res.answer = unllm::refuse(r, "this machine");
        res.refused = true;
        return res;
    }

    const int64_t from = r.window.from, to = r.window.to;
    std::string program;
    std::vector<std::string> search_terms = r.terms;
    for (const auto& e : r.entities)
        if (e.type == "program") {
            program = e.value;
            search_terms.push_back(e.value);
        }

    auto add_receipts = [&](const std::string& sql, const std::vector<int64_t>& binds,
                            const std::vector<std::string>& tb = {}) {
        for (const auto& row : query(db_, sql, binds, tb))
            if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
    };

    std::vector<Clause> clauses;
    // One intent leads; the reading's first is as good a lead as any.
    Intent lead = r.intents.empty() ? Intent::Story : r.intents[0];
    unsigned mask = 0;
    const char* metric = nullptr;
    NP subject;
    std::string unit;
    switch (lead) {
    case Intent::Heat: mask = 16; metric = "peak_temp"; subject = {"the CPU", true}; unit = " C"; break;
    case Intent::Slow: mask = 1; metric = "peak_cpu"; subject = {"the CPU", true}; unit = "%"; break;
    case Intent::Memory: mask = 2; metric = "peak_mem"; subject = {"memory use", true}; unit = "%"; break;
    case Intent::Disk: mask = 4; metric = "peak_disk"; subject = {"disk traffic", true}; break;
    default: break;
    }

    if (mask) {
        auto agg = query(db_,
                         std::string("SELECT COUNT(*), MAX(") + metric + "), MIN(IFNULL(min_margin, 999)) "
                         "FROM events WHERE start < ? AND \"end\" >= ? AND (kinds & ?) != 0",
                         {to, from, int64_t(mask)});
        int64_t n = agg.empty() ? 0 : agg[0].t;
        if (n > 0) {
            double peak = agg[0].a;
            std::string peak_text = lead == Intent::Disk ? human_bytes(peak) + "/s" : fmt("%.0f", peak) + unit;
            std::string rest = peak_text + " during " + unllm::number_word(n) + " " +
                               unllm::plural_noun("event", n);
            clauses.push_back({subject, "reach", rest, Tense::Past});
            if (lead == Intent::Heat && agg[0].b < 15)
                clauses.push_back({{"it", true}, "come", "within " + fmt("%.0f", agg[0].b) + " C of the shutdown point", Tense::Past});
            std::string culprit = top_culprit(db_, from, to, mask, program);
            if (!culprit.empty()) clauses.push_back({{culprit, true}, "be", "the main cause", Tense::Past});
            res.answer = unllm::realize(clauses);
            add_receipts("SELECT start,0,0,summary FROM events WHERE start < ? AND \"end\" >= ? AND (kinds & ?) != 0 ORDER BY " +
                             std::string(metric) + " DESC",
                         {to, from, int64_t(mask)});
            return res;
        }
        res.answer = unllm::realize({{subject, "stay", "quiet in that window: no matching events were recorded", Tense::Past}});
        return res;
    }

    auto log_lead = [&](const char* category, const char* noun) {
        auto rows = query(db_, std::string("SELECT t,0,0,title FROM log WHERE category = '") + category +
                              "' AND t >= ? AND t < ? ORDER BY t DESC",
                          {from, to});
        if (!program.empty()) {
            std::vector<Row> kept;
            for (auto& row : rows)
                if (row.s1.find(program) != std::string::npos) kept.push_back(row);
            rows = kept;
        }
        // "Three crashes were recorded ..." or "No crashes were recorded ..."
        res.answer = unllm::realize({{{noun, false, long(rows.size())}, "be", "recorded in that window", Tense::Past}});
        for (const auto& row : rows)
            if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
    };

    switch (lead) {
    case Intent::Crash: log_lead("problems", "crash"); return res;
    case Intent::Startup: log_lead("startup", "startup change"); return res;
    case Intent::Update: log_lead("software", "software change"); return res;
    case Intent::Battery: log_lead("session", "power event"); return res;
    case Intent::Sharing: log_lead("network", "network event"); return res;
    case Intent::Network: {
        std::string sql = "SELECT first_seen,0,0,program || ' connected to ' || remote || ':' || port AS d, program "
                          "FROM connections WHERE first_seen >= ? AND first_seen < ?";
        if (!program.empty()) sql += " AND program = ?";
        sql += " ORDER BY first_seen DESC";
        auto rows = query(db_, sql, {from, to}, program.empty() ? std::vector<std::string>{} : std::vector<std::string>{program});
        res.answer = unllm::realize({{{"new connection", false, long(rows.size())}, "be", "recorded in that window", Tense::Past}});
        for (const auto& row : rows)
            if (res.receipts.size() < 6) res.receipts.push_back(when_text(row.t) + "  " + row.s1);
        return res;
    }
    case Intent::Trust:
        res.answer = "Trust is judged by the weird scanner, not the history: open the Weird processes page and press "
                     "Scan now, or run: weird";
        return res;
    case Intent::Config:
        res.answer = "Config changes are hindsight's department: try hindsight \"2 days ago\" or hindsight diff.";
        return res;
    default: break;
    }

    // The story: whatever the records say, ranked by relevance.
    auto hits = index_.search(search_terms, from, to, 6);
    if (hits.empty() && search_terms.empty())
        for (const auto& row : query(db_, "SELECT t,0,0,title FROM log WHERE t >= ? AND t < ? ORDER BY t DESC LIMIT 6", {from, to}))
            hits.push_back({0, row.t, "log", row.s1, 0});
    if (hits.empty()) {
        res.answer = unllm::realize({{{"record", false, 0}, "match", "that question in that window", Tense::Past}});
        return res;
    }
    res.answer = unllm::realize({{{"record", false, long(hits.size())}, "match", "in that window", Tense::Past}});
    for (const auto& h : hits) res.receipts.push_back(when_text(h.t) + "  " + h.text);
    return res;
}

}  // namespace as
