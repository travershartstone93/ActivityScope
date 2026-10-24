#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "logbook.h"

namespace as {

// Lines added and removed between two versions of a text file.
std::string line_diff(const std::string& before, const std::string& after, int max_lines = 40);

// Watches the places programs use to start themselves (autostart, services,
// shell startup files, ~/.local/bin, key /etc files) and logs every change.
class StartupWatch {
public:
    explicit StartupWatch(std::string home = {}, std::string etc = "/etc");
    ~StartupWatch();
    StartupWatch(const StartupWatch&) = delete;
    StartupWatch& operator=(const StartupWatch&) = delete;
    std::vector<LogEntry> poll(double now);
    int watching() const { return int(dirs_.size()); }

private:
    struct Dir { std::string path, what; std::set<std::string> only; bool recurse = false; };
    void add(const std::string& path, const std::string& what, std::set<std::string> only = {}, bool recurse = false);
    void remember(const std::string& file);
    std::string describe(const Dir& d, const std::string& name, const std::string& change) const;

    int fd_ = -1;
    std::map<int, Dir> dirs_;                       // watch descriptor -> folder
    std::map<std::string, std::string> contents_;   // file -> last known text (small text files)
    std::map<std::string, std::pair<double, std::string>> pending_;  // file -> (last event, what happened)
    std::map<std::string, int> wd_of_file_;
    std::set<std::string> present_;                 // files that existed when last looked at
};

}  // namespace as
