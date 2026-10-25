#include <algorithm>
#include "dirscan.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace as {

namespace {

// The biggest single files seen so far (at most three), kept on the root as the walk goes.
void note_largest(DirNode& root, const std::string& path, double bytes) {
    auto& big = root.largest;
    if (big.size() == 3 && bytes <= big.back().second) return;
    big.emplace_back(path, bytes);
    std::sort(big.begin(), big.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (big.size() > 3) big.pop_back();
}

void walk(int dirfd, DirNode& node, DirNode& root, const std::string& path, dev_t dev, const std::atomic<bool>& cancel,
          std::atomic<long long>& files, std::atomic<long long>& total) {
    DIR* d = fdopendir(dirfd);
    if (!d) {
        ::close(dirfd);
        return;
    }
    while (dirent* e = readdir(d)) {
        if (cancel) break;
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        struct stat st {};
        if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != dev) continue;
            int sub = openat(dirfd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub < 0) continue;
            auto kid = std::make_unique<DirNode>();
            kid->name = e->d_name;
            walk(sub, *kid, root, path + "/" + e->d_name, dev, cancel, files, total);
            node.bytes += kid->bytes;
            node.files += kid->files;
            node.kids.push_back(std::move(kid));
        } else {
            double b = double(st.st_blocks) * 512;
            node.bytes += b;
            ++node.files;
            ++files;
            total += static_cast<long long>(b);
            if (S_ISREG(st.st_mode)) note_largest(root, path + "/" + e->d_name, b);
        }
    }
    closedir(d);
}

}  // namespace

std::unique_ptr<DirNode> scan_dir(const std::string& root, const std::atomic<bool>& cancel, std::atomic<long long>& files,
                                  std::atomic<long long>& bytes) {
    auto node = std::make_unique<DirNode>();
    node->name = root;
    int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return node;
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        return node;
    }
    walk(fd, *node, *node, root, st.st_dev, cancel, files, bytes);
    return node;
}

}  // namespace as
