#include "fs.h"

#include <charconv>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

namespace as {

std::optional<std::string> read_file(const std::string& path, std::size_t limit) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::string out;
    char buf[8192];
    while (out.size() < limit) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            ::close(fd);
            return std::nullopt;
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

std::optional<std::string> read_line(const std::string& path) {
    auto text = read_file(path, 64 * 1024);
    if (!text) return std::nullopt;
    auto nl = text->find('\n');
    if (nl != std::string::npos) text->resize(nl);
    return trim(*text);
}

std::optional<int64_t> read_int(const std::string& path) {
    auto line = read_line(path);
    if (!line) return std::nullopt;
    return to_int(*line);
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> names;
    DIR* d = ::opendir(path.c_str());
    if (!d) return names;
    while (dirent* e = ::readdir(d)) {
        std::string_view n = e->d_name;
        if (n == "." || n == "..") continue;
        names.emplace_back(n);
    }
    ::closedir(d);
    return names;
}

bool exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

std::string read_link(const std::string& path) {
    char buf[PATH_MAX];
    ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
    if (n <= 0) return {};
    return std::string(buf, static_cast<std::size_t>(n));
}

std::string trim(std::string_view s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return std::string(s);
}

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n') ++j;
        if (j > i) parts.push_back(s.substr(i, j - i));
        i = j;
    }
    return parts;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            parts.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

std::optional<int64_t> to_int(std::string_view s) {
    int64_t v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{}) return std::nullopt;
    return v;
}

std::optional<uint64_t> to_uint(std::string_view s) {
    uint64_t v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{}) return std::nullopt;
    return v;
}

}  // namespace as
