#pragma once
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "logbook.h"
#include "snapshot.h"

struct udev;
struct udev_monitor;
struct udev_device;
struct sd_journal;

namespace as {

class Store;

// Collects everything worth a line in the investigation log. Event sources
// (device events, the system journal) are read as they arrive; nothing polls
// hard. Poll once per recorder tick.
class LogWatch {
public:
    explicit LogWatch(Store* store);
    ~LogWatch();
    LogWatch(const LogWatch&) = delete;
    LogWatch& operator=(const LogWatch&) = delete;

    std::vector<LogEntry> poll(const Snapshot& s);

private:
    void devices(std::vector<LogEntry>& out, double now);
    void device_event(udev_device* d, std::vector<LogEntry>& out, double now);
    void journal(std::vector<LogEntry>& out);
    void pacman(std::vector<LogEntry>& out);
    void mounts(std::vector<LogEntry>& out, double now);
    void changes(const Snapshot& s, std::vector<LogEntry>& out);
    void tools(const Snapshot& s, std::vector<LogEntry>& out);
    void first_runs(const Snapshot& s, std::vector<LogEntry>& out);
    std::string recent_tool(double now, const std::string& device) const;

    struct FsState { std::string type, uuid, label; };
    struct ToolRun { ToolUse use; std::string name, cmdline, parent; double start = 0, last_seen = 0; };

    Store* store_;
    udev* udev_ = nullptr;
    udev_monitor* monitor_ = nullptr;
    sd_journal* journal_ = nullptr;
    std::map<std::string, FsState> fs_;          // /dev/sdb1 -> filesystem
    std::map<std::string, std::string> boards_;  // /dev/ttyACM0 -> board name
    std::map<std::pair<int, uint64_t>, ToolRun> running_;
    std::vector<std::pair<double, std::string>> finished_tools_;  // (time, description) for reformat attribution
    std::set<std::string> mounts_;
    bool mounts_ready_ = false;
    double mounts_t_ = 0, pacman_t_ = 0;
    std::map<std::string, std::pair<bool, std::string>> nets_;  // iface -> (up, ipv4)
    int on_ac_ = -1;
    bool first_ = true;
    bool exes_seeded_ = false;
    Repeats repeats_;
};

}  // namespace as
