#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "facts.h"
#include "fs.h"
#include "kinds.h"
#include "names.h"
#include "unllm/dialogue.h"
#include "unllm/local_model.h"
#include "unllm/unllm.h"

struct sqlite3;

namespace as {

class Sampler;

// The payoff: plain questions against the history database, answered
// deterministically through unllm. Reading, retrieval, and the realizer;
// never a model, and never silence: a question it cannot answer gets
// "I can't answer that" with what little was recognized, the classics get
// a fixed palatable reply, and "right now" questions are measured live.
// It converses: every answer leaves a card describing what it said, so
// "why?", "when was that", "no, I meant firefox" and "it" all have
// something to point at.

// An answer is told in parts, each written by the code that decides it; compose_answer() joins them, with single spaces
// and each part's own punctuation, in this order:
//   scope      how the question was taken ("Taking ... as ...:", "Nothing in the records matches ...", "It is only
//              00:01, so ... Yesterday's:"), in the order said
//   lead       the direct reply to the question's form: "Yes.", "No.", "No: ...", a why's verdict; empty when none
//   (opening)  "I can't say yes or no to that from the records. What they show:" when undecided
//   body       the figures and lists
//   notes      limits and coverage: what the records don't hold, the hourly records standing in, and the like
struct AskResult {
    std::string answer;                 // the parts composed, set where the result leaves Ask (two questions in one line: both
                                        // answers in turn); one short piece of prose, never empty
    std::vector<std::string> scope;
    std::string lead;
    bool undecided = false;             // the records can't settle the yes or no; a part that can (the moment the machine
                                        // slept through) supplies the lead and clears it
    std::string body;
    std::vector<std::string> notes;
    std::vector<std::string> receipts;  // the records the answer stands on
    bool refused = false;
    std::string branch;                 // which handler answered: "live:heat", "followup:why"
    std::string topic;                  // the lead topic the question was read as ("heat"), for measuring
    std::string when;                   // "now" or "window": the time the reading settled on
    std::string check;                  // what the final check said: "ok", "partly", "doubt", "kind A -> B", "" when off
    std::string span;                   // the time the reading looked at, as text, for measuring
    std::vector<std::string> readings;  // the kind answered and the next four nearest, for measuring
    std::vector<std::string> programs;  // the programs the reading named; one found from everyday words shows as
                                        // "movie player -> haruna"
};

// The answer's parts in their order (above); empty parts are left out.
std::string compose_answer(const AskResult& r);

class Ask {
public:
    Ask();
    ~Ask();
    bool open(const std::string& db_path);  // read-only
    // Optional language data; missing files are simply skipped.
    void load_language(const std::string& synonyms_path, const std::string& vectors_path,
                       const std::string& banter_path = "", const std::string& rewrites_path = "");
    // Conversation data: the follow-up table and chat-speak. Call after
    // load_language (the speller learns the analyzer's words from it).
    void load_conversation(const std::string& followups_path, const std::string& chatspeak_path);
    // Refused questions are appended here (timestamp, score, question), so
    // the vocabulary can grow from real misses. Local file, never the db.
    void set_miss_log(const std::string& path) { miss_log_ = path; }
    // "What is kswapd0": layered fact files (shipped, harvested, user).
    void load_facts(const std::vector<std::string>& tsv_paths) { facts_.load(tsv_paths); }
    // The catalogue of question kinds (kinds.h): its matcher names the topic
    // of a question by its nearest example phrasing, in place of the word
    // lists (measured 2026-10-22: topic right 77 of 85 against 22).
    bool load_kinds(const std::string& dir, const std::string& cache, const std::string& model_dir) {
        const bool ok = kinds_.load(dir, cache, model_dir);
        names_.set_model(kinds_.model());  // everyday words to programs (names.h), by the same model
        names_.set_cache(cache + "/names.vec");
        return ok;
    }
    // The programs the last question's everyday words named (names.h), best first; empty when the reading
    // named its program itself or none.
    const std::vector<NameMatch>& named() const { return named_; }
    // Tests: live readings come from these /proc and /sys trees.
    void set_roots(const Roots& roots);
    AskResult ask(const std::string& question, int64_t now);
    // The final check: a small local model (Ollama, "qwen3:4b") reads the question, how ask read
    // it and the answer, and says whether the answer fits and which of the closest readings fits
    // best. It never writes the answer: another reading is answered by ask's own code, and an
    // answer that still doesn't fit is said to be in doubt. "off" or "" turns it off (the default).
    void set_checker(const std::string& model) { checker_.configure(model.empty() ? "off" : model); }
    // Measuring only: answer every question as this catalogue kind (no check), or show the
    // check this kind first among the other readings. "" turns either off.
    void force_reading(const std::string& kind) { forced_ = kind; }
    void offer_reading(const std::string& kind) { offered_ = kind; }

private:
    // One pass: the question read and answered, nothing remembered; force_kind answers it as that
    // catalogue kind. remember_ says whether the turn belongs in the conversation.
    AskResult ask_once(const std::string& question, int64_t now, const std::string& force_kind);
    // The model's verdict on one answer: "yes", "partly" or "no", and the letter of the reading
    // that fits best ("A" is ask's own). Nothing when the model is unreachable.
    struct Verdict {
        std::string answers, better, why;
    };
    std::optional<Verdict> check_answer(const std::string& question, const std::string& kind, const AskResult& res,
                                        const std::vector<std::string>& others);
    std::string kind_words(const std::string& kind) const;
    // The question's words that may name something in the records (a file, a moment's program): the reading's words
    // the analyzer doesn't know, less the kind's own wording (the words of its description: "file" for the config
    // kinds) and the everyday wording of questions (a word the phrasings of five or more topics use: "any", "lol").
    // A program the reading names always counts.
    std::vector<std::string> named_words(const unllm::Reading& r, const std::string& kind) const;
    // "while the lid was closed": when the reading's "while ..." clause names a state the session log keeps (the lid,
    // sleep, the charger) and no program, a sentence saying when it held in the window or that it never did; "".
    std::string state_lead(const unllm::Reading& r, int64_t now) const;
    void log_miss(const std::string& question, const unllm::Reading& r);
    // One reading answered: the machine handlers in priority order.
    AskResult answer_reading(const unllm::Reading& r, const std::vector<unllm::Token>& tokens,
                             const std::vector<unllm::Token>& raw_tokens, const std::string& question,
                             int64_t now);
    // A bare follow-up answered against the card it points at.
    AskResult follow_up(unllm::Act act, const unllm::Card& card, int64_t now);
    // "in the half hour before the terminal got killed": the event the reading's clause names, placed by the
    // records, and the window narrowed to the span before or after it (event_at_, event_what_).
    void place_event(unllm::Reading& r, int64_t now);
    // Free disk space: now (live, every real filesystem), over the reading's window
    // (the per-minute samples of / free), or what takes it (the home folder scanned).
    AskResult answer_records(const std::string& kind, const unllm::Reading& r, int64_t now);
    // "Whats up with my laptop": the window as a paragraph, not a count.
    std::string story_summary(int64_t from, int64_t to, int64_t now, std::vector<std::string>* receipts);
    // "was it the ram or the swap": the reading's two alternatives, when both are memory measures (RAM and swap, by
    // the catalogue), answered side by side from the readings. False when they are not, leaving res alone.
    bool memory_choice(const unllm::Reading& r, AskResult* res);

    sqlite3* db_ = nullptr;
    unllm::Analyzer an_;
    Kinds kinds_;
    unllm::Vectors vectors_;
    unllm::Banter banter_;
    unllm::Rewrites rewrites_;
    unllm::Speller speller_;
    bool speller_ready_ = false;
    std::set<std::string> program_words_;  // the programs on record, lower case: no typo ("systemd" is not "system")
    unllm::FollowUps followups_;
    unllm::Dialogue dialogue_;
    unllm::Card card_;             // the answer being given, filled by the handlers
    std::size_t receipt_offset_ = 0;  // "tell me more": skip what was already shown
    std::string kind_;                // the catalogue kind the current question matched
    Facts facts_;
    unllm::Index index_;
    std::string miss_log_;
    std::unique_ptr<Sampler> sampler_;  // built on the first live question
    Roots roots_;                       // where live files are read (set_roots): /etc for the system's own settings
    Names names_;                       // everyday words to programs
    std::vector<NameMatch> named_;      // what the last question's words named
    unllm::LocalModel checker_;         // the final check, off unless set_checker names a model
    bool remember_ = false;             // the last ask_once belongs in the conversation
    bool fresh_ = false;                // the last ask_once was a new question, not a follow-up or a clarify
    KindMatch match_;                   // the last ask_once's match, for the check's other readings
    unllm::TimeWindow window_;          // the last ask_once's window, for the check
    std::vector<std::string> programs_; // the programs the last ask_once's reading named
    std::string forced_, offered_;      // force_reading, offer_reading
    int64_t event_at_ = 0;              // the event the last question's clause named, placed (0: none)
    std::string event_what_;            // its record ("ollama.service failed: ...")
    bool event_unmatched_ = false;      // the clause, the person's doing or a "during" thing, matched no record set
    std::vector<std::string> condition_tried_;  // words of a "while ..." clause that named no program
};

}  // namespace as
