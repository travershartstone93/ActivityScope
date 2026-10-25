#pragma once
// The catalogue of question kinds ask can answer, and the matcher that maps a
// typed question to one of them. Every kind carries example phrasings; each
// phrasing is turned into a vector once (bge-small through unllm's embedder)
// and a question goes to the kind of the phrasing nearest to it. Nothing
// generative: the same question always lands on the same kind, and the
// answer for a kind is computed from the records by the kind's own code.
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace unllm {
class Meaning;
}

namespace as {

struct QuestionKind {
    std::string id, topic, form, window, data, answer;
    std::string reply;  // for data "missing": what ask says instead, naming what it does record
};

struct KindMatch {
    std::string id;        // the kind of the nearest phrasing, "" when nothing is loaded
    std::string phrasing;  // that phrasing
    float score = 0;       // cosine similarity, 1 is identical
    std::vector<std::pair<std::string, float>> runners;  // every kind with its nearest phrasing's score, best first
    std::vector<std::string> runner_phrasings;           // each runner's nearest phrasing, same order
    // Every topic with the mean score of its three nearest phrasings (of all its kinds), best first: how near the
    // question is to the topic as a whole, not to one phrasing of it.
    std::vector<std::pair<std::string, float>> topics;
    float topic_score(const std::string& topic) const;   // that mean for one topic, 0 when it has none
};

class Kinds {
public:
    Kinds();
    ~Kinds();
    // dir holds kinds.tsv and phrasings.tsv (shipped data); cache is a folder
    // where the phrasing vectors are kept and rebuilt when phrasings.tsv
    // changes; model_dir holds the embedding model (model.onnx, vocab.txt,
    // embed.json). rebuild forces the vectors to be made again.
    bool load(const std::string& dir, const std::string& cache, const std::string& model_dir, bool rebuild = false);
    bool ready() const;
    KindMatch match(const std::string& question) const;
    const std::vector<QuestionKind>& kinds() const;
    const QuestionKind* find(const std::string& id) const;
    std::size_t phrasings() const;
    // How many topics' phrasings use the word (a token's normal or lower-case form, as unllm::tokenize gives it):
    // a word the phrasings of many topics use ("any", "lol") is how people ask, not what they ask about.
    std::size_t topics_using(const std::string& word) const;
    // Whether the kind's own phrasings use the word (same forms): how that kind is asked ("lose" for the free space).
    bool phrased(const std::string& kind, const std::string& word) const;
    // The sentence model the matcher loaded, for other matching by meaning (names.h); null when not loaded.
    const unllm::Meaning* model() const;
    // A cut-off on this model's scores ("understand", "doubt", "unplaced_topic", "records", "band", "two_questions"):
    // the value the model's embed.json gives under "cutoffs", else the default, which is the untouched bge-small's.
    // A model's cut-offs are calibrated with it (bench/hyp/embed/calibrate.py) and travel with it, not with the code.
    float cutoff(const std::string& name, float dflt) const;
    // The plain name words.tsv gives a topic or a form ("slow": "CPU load and slowness"); the value itself when it has
    // none. what is "topic" or "form".
    std::string plain(const std::string& what, const std::string& value) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace as
