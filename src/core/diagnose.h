#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace as {

struct Snapshot;

// The diagnosis rule book: deterministic rules over the recorded tables,
// in the expert-system shape. Every conclusion carries a grade decided by
// countable criteria, the receipts behind it, and (at the weak grade) the
// named gap in the evidence:
//   Is             direct observation: the data itself is the fact
//   PointsTo       one causal hop with a dominant culprit and matching timing
//   ConsistentWith correlation across days, or evidence with a named gap
// Conflicts resolve by salience (hardware 3 > recent change 2 > workload 1),
// then specificity (premises matched), then recency; one conclusion per
// rule and scope. Exoneration rules ("swap is parked, not thrashing") are
// first class, so the book can say what is NOT wrong.
enum class Grade { Is, PointsTo, ConsistentWith };
const char* grade_phrase(Grade g);

// What to do about a finding, one sentence per rule. Advice only: nothing
// is ever run on the user's behalf.
std::string advice_for(const std::string& rule_id);

struct Conclusion {
    std::string rule_id;
    std::string sentence;   // complete graded wording, ends with a period
    std::string gap;        // what is missing; filled at ConsistentWith
    Grade grade = Grade::Is;
    double salience = 1;
    int specificity = 1;
    int64_t t = 0;          // most recent supporting row
    int64_t event_id = 0;   // 0 when not tied to one event
    std::vector<std::string> receipts;
};

class Diagnose {
public:
    explicit Diagnose(sqlite3* db) : db_(db) {}
    // The whole book over a window, ordered; live supplies what only the
    // present can answer (battery wear).
    std::vector<Conclusion> run(int64_t from, int64_t to, int64_t now, const Snapshot* live = nullptr);
    // The rules touching one event, for "why" answers.
    std::vector<Conclusion> for_event(int64_t event_id, int64_t now);

private:
    sqlite3* db_;
};

}  // namespace as
