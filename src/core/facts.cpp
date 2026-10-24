#include "facts.h"

#include <fnmatch.h>

#include <cctype>
#include <fstream>

namespace as {

namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

void Facts::load(const std::vector<std::string>& tsv_paths) {
    for (const auto& path : tsv_paths) {
        std::ifstream f(path);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            Fact fact;
            std::size_t a = line.find('\t');
            if (a == std::string::npos) continue;
            std::size_t b = line.find('\t', a + 1);
            if (b == std::string::npos) continue;
            std::size_t c = line.find('\t', b + 1);
            fact.pattern = line.substr(0, a);
            fact.category = line.substr(a + 1, b - a - 1);
            fact.explanation = c == std::string::npos ? line.substr(b + 1) : line.substr(b + 1, c - b - 1);
            if (c != std::string::npos) fact.source = line.substr(c + 1);
            if (!fact.pattern.empty() && !fact.explanation.empty()) facts_.push_back(std::move(fact));
        }
    }
}

const Fact* Facts::lookup(const std::string& name) const {
    const std::string low = lower(name);
    // Later files were appended later, so the scan runs back to front.
    for (auto it = facts_.rbegin(); it != facts_.rend(); ++it)
        if (lower(it->pattern) == low) return &*it;
    for (auto it = facts_.rbegin(); it != facts_.rend(); ++it)
        if (it->pattern.find('*') != std::string::npos &&
            fnmatch(lower(it->pattern).c_str(), low.c_str(), 0) == 0)
            return &*it;
    return nullptr;
}

std::vector<const Fact*> Facts::lookup_all(const std::string& name) const {
    const std::string low = lower(name);
    std::vector<const Fact*> out;
    for (auto it = facts_.rbegin(); it != facts_.rend(); ++it)
        if (lower(it->pattern) == low) out.push_back(&*it);
    if (out.empty())
        for (auto it = facts_.rbegin(); it != facts_.rend(); ++it)
            if (it->pattern.find('*') != std::string::npos &&
                fnmatch(lower(it->pattern).c_str(), low.c_str(), 0) == 0)
                out.push_back(&*it);
    return out;
}

}  // namespace as
