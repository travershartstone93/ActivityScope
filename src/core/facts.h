#pragma once
#include <string>
#include <vector>

namespace as {

// What a process IS, in one sentence. Three layered TSV files (shipped base,
// machine-generated local, user-owned custom), later file wins, exact name
// beats glob. The explanation column is the predicate after "is":
//   kswapd0 <TAB> kernel <TAB> the kernel's memory reclaim thread; ... <TAB> hand
struct Fact {
    std::string pattern, category, explanation, source;
};

class Facts {
public:
    // Missing files are skipped; later paths override earlier ones.
    void load(const std::vector<std::string>& tsv_paths);
    // Exact match first (case-insensitive), then glob patterns; null when
    // nothing matches. The pointer lives as long as this object.
    const Fact* lookup(const std::string& name) const;
    // Every file's note for the name, the winning one first: the exact ones, or else the globs that match.
    std::vector<const Fact*> lookup_all(const std::string& name) const;
    std::size_t size() const { return facts_.size(); }

private:
    std::vector<Fact> facts_;  // in load order; lookup scans back to front
};

}  // namespace as
