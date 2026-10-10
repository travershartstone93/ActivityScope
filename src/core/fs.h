#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace as {

// Where the kernel's files live. Tests point these at fixture folders.
struct Roots {
    std::string proc = "/proc";
    std::string sys = "/sys";
    std::string etc = "/etc";
};

std::optional<std::string> read_file(const std::string& path, std::size_t limit = 1 << 20);
std::optional<std::string> read_line(const std::string& path);

std::optional<int64_t> read_int(const std::string& path);
std::vector<std::string> list_dir(const std::string& path);
bool exists(const std::string& path);
std::string read_link(const std::string& path);

std::string trim(std::string_view s);
std::vector<std::string_view> split_ws(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep);
bool starts_with(std::string_view s, std::string_view prefix);
std::optional<int64_t> to_int(std::string_view s);
std::optional<uint64_t> to_uint(std::string_view s);

}  // namespace as
