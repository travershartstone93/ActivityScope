#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "unllm/unllm.h"

struct sqlite3;

namespace as {

// The payoff: plain questions against the history database, answered
// deterministically through unllm. Reading, retrieval, and the realizer;
// never a model, and never silence: a question it cannot answer gets
// "I can't answer that" with what little was recognized.

struct AskResult {
    std::string answer;                 // one short piece of prose, never empty
    std::vector<std::string> receipts;  // the records the answer stands on
    bool refused = false;
};

class Ask {
public:
    ~Ask();
    bool open(const std::string& db_path);  // read-only
    // Optional language data; missing files are simply skipped.
    void load_language(const std::string& synonyms_path, const std::string& vectors_path);
    AskResult ask(const std::string& question, int64_t now);

private:
    sqlite3* db_ = nullptr;
    unllm::Analyzer an_;
    unllm::Vectors vectors_;
    unllm::Index index_;
    unllm::Reading last_;
    bool has_last_ = false;
};

}  // namespace as
