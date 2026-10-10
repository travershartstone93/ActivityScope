#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace as {

// One folder and everything under it, sized by the space it uses on disk.
struct DirNode {
    std::string name;
    double bytes = 0;
    long files = 0;
    std::vector<std::unique_ptr<DirNode>> kids;
};

// Walks a folder without following links or crossing onto other drives.
// `cancel` stops it early; `files` and `bytes` count progress as it goes.
std::unique_ptr<DirNode> scan_dir(const std::string& root, const std::atomic<bool>& cancel, std::atomic<long long>& files,
                                  std::atomic<long long>& bytes);

}  // namespace as
