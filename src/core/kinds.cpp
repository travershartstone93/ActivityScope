#include "kinds.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "unllm/meaning.h"
#include "unllm/unllm.h"

namespace as {

namespace {

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

int64_t stamp_of(const std::string& path) {  // size and change time: the cache is good while these hold
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return 0;
    return int64_t(st.st_size) * 1000003 + int64_t(st.st_mtime);
}

}  // namespace

struct Kinds::Impl {
    std::vector<QuestionKind> kinds;
    std::map<std::string, std::size_t> index;
    std::vector<int32_t> owner;        // phrasing -> kind index
    std::vector<std::string> phrasing;
    std::vector<float> vectors;        // phrasing-major, d floats each, unit length
    std::map<std::string, std::set<std::string>> topics_of;  // a phrasing word -> the topics whose phrasings use it
    std::map<std::string, std::set<std::string>> kinds_of;   // a phrasing word -> the kinds whose phrasings use it
    int d = 0;
    unllm::Meaning model;
    bool ready = false;

    bool read_cache(const std::string& path, int64_t stamp) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        char magic[8];
        int32_t n = 0, dim = 0;
        int64_t st = 0;
        in.read(magic, 8);
        in.read(reinterpret_cast<char*>(&n), 4);
        in.read(reinterpret_cast<char*>(&dim), 4);
        in.read(reinterpret_cast<char*>(&st), 8);
        if (!in || std::memcmp(magic, "ASKVEC1\0", 8) != 0 || st != stamp || n != int32_t(phrasing.size()) || dim <= 0) return false;
        d = dim;
        vectors.assign(std::size_t(n) * std::size_t(d), 0.0f);
        in.read(reinterpret_cast<char*>(vectors.data()), std::streamsize(vectors.size() * sizeof(float)));
        return bool(in);
    }

    bool write_cache(const std::string& path, int64_t stamp) const {
        std::ofstream out(path + ".tmp", std::ios::binary);
        if (!out) return false;
        const int32_t n = int32_t(phrasing.size()), dim = int32_t(d);
        out.write("ASKVEC1\0", 8);
        out.write(reinterpret_cast<const char*>(&n), 4);
        out.write(reinterpret_cast<const char*>(&dim), 4);
        out.write(reinterpret_cast<const char*>(&stamp), 8);
        out.write(reinterpret_cast<const char*>(vectors.data()), std::streamsize(vectors.size() * sizeof(float)));
        out.close();
        return std::rename((path + ".tmp").c_str(), path.c_str()) == 0;
    }
};

Kinds::Kinds() : impl_(std::make_unique<Impl>()) {}
Kinds::~Kinds() = default;

bool Kinds::load(const std::string& dir, const std::string& cache, const std::string& model_dir, bool rebuild) {
    Impl& im = *impl_;
    im.ready = false;
    std::ifstream kf(dir + "/kinds.tsv");
    if (!kf) return false;
    std::string line;
    std::getline(kf, line);  // the header
    while (std::getline(kf, line)) {
        auto f = split_tabs(line);
        if (f.size() < 6) continue;
        im.index[f[0]] = im.kinds.size();
        im.kinds.push_back({f[0], f[1], f[2], f[3], f[4], f[5], f.size() > 6 ? f[6] : std::string()});
    }
    std::ifstream pf(dir + "/phrasings.tsv");
    if (!pf || im.kinds.empty()) return false;
    while (std::getline(pf, line)) {
        auto f = split_tabs(line);
        if (f.size() < 2) continue;
        auto it = im.index.find(f[0]);
        if (it == im.index.end()) continue;
        im.owner.push_back(int32_t(it->second));
        im.phrasing.push_back(f[1]);
        for (const auto& t : unllm::tokenize(f[1]))
            for (const std::string& w : {t.norm, t.lower}) {
                im.topics_of[w].insert(im.kinds[it->second].topic);
                im.kinds_of[w].insert(f[0]);
            }
    }
    if (im.phrasing.empty() || !im.model.load_model(model_dir)) return false;
    im.d = im.model.dim();
    const int64_t stamp = stamp_of(dir + "/phrasings.tsv");
    const std::string vec = cache + "/phrasings.vec";
    if (rebuild || !im.read_cache(vec, stamp)) {
        // Every phrasing through the model once; a few seconds per thousand.
        im.vectors.assign(im.phrasing.size() * std::size_t(im.d), 0.0f);
        for (std::size_t i = 0; i < im.phrasing.size(); ++i) {
            auto v = im.model.embed(im.phrasing[i], false);
            if (int(v.size()) != im.d) return false;
            std::copy(v.begin(), v.end(), im.vectors.begin() + std::ptrdiff_t(i * std::size_t(im.d)));
        }
        im.write_cache(vec, stamp);  // a failed save only costs the next start the same work
    }
    im.ready = true;
    return true;
}

bool Kinds::ready() const { return impl_->ready; }
const std::vector<QuestionKind>& Kinds::kinds() const { return impl_->kinds; }
std::size_t Kinds::phrasings() const { return impl_->phrasing.size(); }

std::size_t Kinds::topics_using(const std::string& word) const {
    auto it = impl_->topics_of.find(word);
    return it == impl_->topics_of.end() ? 0 : it->second.size();
}

bool Kinds::phrased(const std::string& kind, const std::string& word) const {
    auto it = impl_->kinds_of.find(word);
    return it != impl_->kinds_of.end() && it->second.count(kind) > 0;
}
const unllm::Meaning* Kinds::model() const { return impl_->model.can_embed() ? &impl_->model : nullptr; }

const QuestionKind* Kinds::find(const std::string& id) const {
    auto it = impl_->index.find(id);
    return it == impl_->index.end() ? nullptr : &impl_->kinds[it->second];
}

KindMatch Kinds::match(const std::string& question) const {
    KindMatch out;
    const Impl& im = *impl_;
    if (!im.ready) return out;
    const auto q = im.model.embed(question, false);
    if (int(q.size()) != im.d) return out;
    std::vector<float> best(im.kinds.size(), -2.0f);  // each kind's nearest phrasing
    std::vector<std::size_t> best_at(im.kinds.size(), 0);
    std::size_t top = 0;
    float top_score = -2.0f;
    for (std::size_t i = 0; i < im.phrasing.size(); ++i) {
        const float* v = im.vectors.data() + i * std::size_t(im.d);
        float s = 0;
        for (int k = 0; k < im.d; ++k) s += v[k] * q[std::size_t(k)];
        if (s > best[std::size_t(im.owner[i])]) {
            best[std::size_t(im.owner[i])] = s;
            best_at[std::size_t(im.owner[i])] = i;
        }
        if (s > top_score) {
            top_score = s;
            top = i;
        }
    }
    out.id = im.kinds[std::size_t(im.owner[top])].id;
    out.phrasing = im.phrasing[top];
    out.score = top_score;
    // Every kind with its nearest phrasing's score, best first, so a caller
    // that knows the window or the topic can choose among the kinds that fit.
    std::vector<std::size_t> order(im.kinds.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return best[a] > best[b]; });
    for (std::size_t i : order) {
        out.runners.emplace_back(im.kinds[i].id, best[i]);
        out.runner_phrasings.push_back(best[i] > -2.0f ? im.phrasing[best_at[i]] : "");
    }
    return out;
}

}  // namespace as
