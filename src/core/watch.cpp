#include "watch.h"

#include <fcntl.h>
#include <libudev.h>
#include <systemd/sd-journal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>

#include "events.h"
#include "fs.h"
#include "store.h"

namespace as {

namespace {

std::string prop(udev_device* d, const char* key) {
    const char* v = udev_device_get_property_value(d, key);
    return v ? v : "";
}

std::string usb_name(udev_device* d) {
    std::string vid = prop(d, "ID_VENDOR_ID"), pid = prop(d, "ID_MODEL_ID");
    std::string board = board_name(vid, pid);
    std::string vendor = prop(d, "ID_VENDOR_FROM_DATABASE");
    if (vendor.empty()) vendor = prop(d, "ID_VENDOR");
    std::string model = prop(d, "ID_MODEL_FROM_DATABASE");
    if (model.empty()) model = prop(d, "ID_MODEL");
    std::replace(model.begin(), model.end(), '_', ' ');
    std::replace(vendor.begin(), vendor.end(), '_', ' ');
    std::string name = board.empty() ? trim(vendor + " " + model) : board;
    if (name.empty()) name = "USB device";
    if (!vid.empty()) name += " (" + vid + ":" + pid + ")";
    return name;
}

bool interesting_block(const std::string& node) {
    for (const char* skip : {"/dev/loop", "/dev/zram", "/dev/ram", "/dev/dm-", "/dev/sr"})
        if (starts_with(node, skip)) return false;
    return starts_with(node, "/dev/");
}

std::string fs_text(const std::string& type, const std::string& label) {
    if (type.empty()) return "no filesystem";
    return type + (label.empty() ? "" : " '" + label + "'");
}

std::string base(const std::string& path) {
    auto s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

}  // namespace

LogWatch::LogWatch(Store* store) : store_(store) {
    udev_ = udev_new();
    if (udev_) {
        monitor_ = udev_monitor_new_from_netlink(udev_, "udev");
        if (monitor_) {
            for (const char* sub : {"usb", "tty", "block"}) udev_monitor_filter_add_match_subsystem_devtype(monitor_, sub, nullptr);
            udev_monitor_enable_receiving(monitor_);
            int fd = udev_monitor_get_fd(monitor_);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
        // What is already attached: filesystems (to spot reformats) and
        // serial ports (to name the board a flashing tool talks to).
        udev_enumerate* en = udev_enumerate_new(udev_);
        udev_enumerate_add_match_subsystem(en, "block");
        udev_enumerate_add_match_subsystem(en, "tty");
        udev_enumerate_scan_devices(en);
        udev_list_entry* e;
        udev_list_entry_foreach(e, udev_enumerate_get_list_entry(en)) {
            udev_device* d = udev_device_new_from_syspath(udev_, udev_list_entry_get_name(e));
            if (!d) continue;
            const char* node = udev_device_get_devnode(d);
            std::string sub = udev_device_get_subsystem(d) ? udev_device_get_subsystem(d) : "";
            if (node && sub == "block" && interesting_block(node))
                fs_[node] = {prop(d, "ID_FS_TYPE"), prop(d, "ID_FS_UUID"), prop(d, "ID_FS_LABEL")};
            if (node && sub == "tty" && !prop(d, "ID_VENDOR_ID").empty()) boards_[node] = usb_name(d);
            udev_device_unref(d);
        }
        udev_enumerate_unref(en);
    }

    // The system journal, resumed where the last run stopped so nothing that
    // happened while ActivityScope was closed (or the machine was off) is lost.
    if (sd_journal_open(&journal_, SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM | SD_JOURNAL_CURRENT_USER) == 0) {
        for (const char* id : {"sudo", "pkexec", "polkitd", "systemd-sleep", "systemd-logind", "NetworkManager", "tailscaled",
                               "systemd", "systemd-coredump"}) {
            std::string m = std::string("SYSLOG_IDENTIFIER=") + id;
            sd_journal_add_match(journal_, m.c_str(), 0);
        }
        sd_journal_add_disjunction(journal_);
        sd_journal_add_match(journal_, "_TRANSPORT=kernel", 0);
        std::string cursor = store_ ? store_->meta("journal_cursor") : "";
        if (!cursor.empty() && sd_journal_seek_cursor(journal_, cursor.c_str()) == 0) {
            sd_journal_next(journal_);  // the saved entry itself was already logged
        } else {
            sd_journal_seek_realtime_usec(journal_, uint64_t(time(nullptr) - 86400) * 1000000ULL);  // first run: the last day
        }
    } else {
        journal_ = nullptr;
    }
    exes_seeded_ = store_ && store_->meta("exes_seeded") == "1";
}

LogWatch::~LogWatch() {
    if (monitor_) udev_monitor_unref(monitor_);
    if (udev_) udev_unref(udev_);
    if (journal_) sd_journal_close(journal_);
}

std::vector<LogEntry> LogWatch::poll(const Snapshot& s) {
    std::vector<LogEntry> out;
    devices(out, s.wall);
    journal(out);
    if (s.wall - pacman_t_ >= 5 || first_) {
        pacman(out);
        pacman_t_ = s.wall;
    }
    if (s.wall - mounts_t_ >= 2) {
        mounts(out, s.wall);
        mounts_t_ = s.wall;
    }
    changes(s, out);
    if (!s.procs_reused) {
        tools(s, out);
        first_runs(s, out);
    }
    first_ = false;
    std::sort(out.begin(), out.end(), [](const LogEntry& a, const LogEntry& b) { return a.t < b.t; });
    return repeats_.feed(std::move(out), s.wall);
}

void LogWatch::devices(std::vector<LogEntry>& out, double now) {
    if (!monitor_) return;
    for (int i = 0; i < 256; ++i) {
        udev_device* d = udev_monitor_receive_device(monitor_);
        if (!d) break;
        device_event(d, out, now);
        udev_device_unref(d);
    }
}

void LogWatch::device_event(udev_device* d, std::vector<LogEntry>& out, double now) {
    std::string action = udev_device_get_action(d) ? udev_device_get_action(d) : "";
    std::string sub = udev_device_get_subsystem(d) ? udev_device_get_subsystem(d) : "";
    std::string type = udev_device_get_devtype(d) ? udev_device_get_devtype(d) : "";
    const char* nodep = udev_device_get_devnode(d);
    std::string node = nodep ? nodep : "";
    auto entry = [&](const char* cat, std::string title, std::string detail) {
        out.push_back({now, cat, std::move(title), std::move(detail)});
    };
    std::string ids = "Vendor " + prop(d, "ID_VENDOR_ID") + ", product " + prop(d, "ID_MODEL_ID") +
                      (prop(d, "ID_SERIAL_SHORT").empty() ? "" : ", serial " + prop(d, "ID_SERIAL_SHORT")) + "\nPath " +
                      (udev_device_get_devpath(d) ? udev_device_get_devpath(d) : "");

    if (sub == "usb" && type == "usb_device") {
        if (prop(d, "ID_MODEL").find("root hub") != std::string::npos || prop(d, "ID_MODEL_FROM_DATABASE").find("root hub") != std::string::npos) return;
        if (action == "add") entry("devices", "Plugged in: " + usb_name(d), ids);
        else if (action == "remove") entry("devices", "Unplugged: " + usb_name(d), ids);
    } else if (sub == "tty" && !node.empty() && !prop(d, "ID_VENDOR_ID").empty()) {
        std::string name = usb_name(d);
        if (action == "add") {
            boards_[node] = name;
            entry("devices", "Serial port " + node + " ready: " + name, ids);
        } else if (action == "remove") {
            boards_.erase(node);
            entry("devices", "Serial port " + node + " gone: " + name, ids);
        }
    } else if (sub == "block" && interesting_block(node)) {
        FsState now_fs{prop(d, "ID_FS_TYPE"), prop(d, "ID_FS_UUID"), prop(d, "ID_FS_LABEL")};
        std::string model = prop(d, "ID_MODEL");
        std::replace(model.begin(), model.end(), '_', ' ');
        std::string detail = ids + "\nFilesystem " + fs_text(now_fs.type, now_fs.label) + (now_fs.uuid.empty() ? "" : ", UUID " + now_fs.uuid);
        if (action == "add") {
            fs_[node] = now_fs;
            if (type == "disk") {
                uint64_t sectors = 0;
                if (auto v = read_int(std::string(udev_device_get_syspath(d)) + "/size")) sectors = uint64_t(*v);
                entry("storage", "Drive connected: " + (model.empty() ? node : model) + " (" + node + ", " + human_bytes(double(sectors) * 512) + ")", detail);
            } else if (!now_fs.type.empty()) {
                entry("storage", "Partition " + node + " found: " + fs_text(now_fs.type, now_fs.label), detail);
            }
        } else if (action == "remove") {
            fs_.erase(node);
            if (type == "disk") entry("storage", "Drive removed: " + (model.empty() ? node : model) + " (" + node + ")", detail);
        } else if (action == "change") {
            auto it = fs_.find(node);
            FsState before = it == fs_.end() ? FsState{} : it->second;
            fs_[node] = now_fs;
            if (before.uuid != now_fs.uuid || before.type != now_fs.type) {
                std::string by = recent_tool(now, node);
                std::string title = now_fs.type.empty()
                                        ? "Filesystem removed from " + node + " (was " + fs_text(before.type, before.label) + ")"
                                        : "Reformatted " + node + ": " + fs_text(before.type, before.label) + " became " + fs_text(now_fs.type, now_fs.label);
                entry("storage", title + (by.empty() ? "" : ", by " + by), detail + "\nBefore: " + fs_text(before.type, before.label) +
                                                                              (before.uuid.empty() ? "" : ", UUID " + before.uuid));
            }
        }
    }
}

std::string LogWatch::recent_tool(double now, const std::string& device) const {
    std::string best;
    for (const auto& [t, what] : finished_tools_)
        if (now - t < 120 && (best.empty() || what.find(device) != std::string::npos)) best = what;
    for (const auto& [k, run] : running_)
        if (run.use.category == "storage") best = run.name + " (" + run.cmdline + ")";
    return best;
}

void LogWatch::journal(std::vector<LogEntry>& out) {
    if (!journal_) return;
    int n = 0;
    bool moved = false;
    while (n++ < 20000 && sd_journal_next(journal_) > 0) {
        moved = true;
        JournalLine line;
        uint64_t usec = 0;
        sd_journal_get_realtime_usec(journal_, &usec);
        line.t = double(usec) / 1e6;
        auto field = [&](const char* name) -> std::string {
            const void* data;
            size_t len;
            if (sd_journal_get_data(journal_, name, &data, &len) != 0) return {};
            std::string s(static_cast<const char*>(data), len);
            auto eq = s.find('=');
            return eq == std::string::npos ? std::string() : s.substr(eq + 1);
        };
        line.message = field("MESSAGE");
        line.identifier = field("SYSLOG_IDENTIFIER");
        line.unit = field("_SYSTEMD_UNIT");
        line.kernel = field("_TRANSPORT") == "kernel";
        if (auto e = classify_journal(line)) out.push_back(*e);
    }
    if (moved && store_) {
        char* cursor = nullptr;
        if (sd_journal_get_cursor(journal_, &cursor) == 0 && cursor) {
            store_->set_meta("journal_cursor", cursor);
            free(cursor);
        }
    }
}

void LogWatch::pacman(std::vector<LogEntry>& out) {
    const std::string path = "/var/log/pacman.log";
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return;
    long long offset = store_ ? std::atoll(store_->meta("pacman_offset").c_str()) : 0;
    if (offset > st.st_size) offset = 0;  // the log was rotated
    if (offset == st.st_size) return;
    std::ifstream in(path);
    in.seekg(offset);
    const double keep_from = double(time(nullptr)) - 90 * 86400;
    long long resume = offset;
    for (auto& e : read_pacman(in, offset, &resume))
        if (e.t >= keep_from) out.push_back(std::move(e));
    if (store_) store_->set_meta("pacman_offset", std::to_string(resume));
}

void LogWatch::mounts(std::vector<LogEntry>& out, double now) {
    auto text = read_file("/proc/self/mountinfo");
    if (!text) return;
    std::set<std::string> cur;
    std::map<std::string, std::string> fstype;
    for (const auto& line : split(*text, '\n')) {
        auto dash = line.find(" - ");
        if (dash == std::string::npos) continue;
        auto left = split_ws(std::string_view(line).substr(0, dash));
        auto right = split_ws(std::string_view(line).substr(dash + 3));
        if (left.size() < 5 || right.size() < 2) continue;
        std::string source(right[1]), point(left[4]);
        if (!interesting_block(source)) continue;
        // Skip the system's own volumes; removable drives live under /run/media or /media.
        if (!starts_with(point, "/run/media/") && !starts_with(point, "/media/") && !starts_with(point, "/mnt/")) continue;
        std::string key = source + " on " + point;
        cur.insert(key);
        fstype[key] = std::string(right[0]);
    }
    if (mounts_ready_) {
        for (const auto& k : cur)
            if (!mounts_.count(k)) out.push_back({now, "storage", "Mounted " + k, "Filesystem " + fstype[k]});
        for (const auto& k : mounts_)
            if (!cur.count(k)) out.push_back({now, "storage", "Unmounted " + k, ""});
    }
    mounts_ = std::move(cur);
    mounts_ready_ = true;
}

void LogWatch::changes(const Snapshot& s, std::vector<LogEntry>& out) {
    for (const auto& n : s.nets) {
        if (n.kind == "Loopback" || (n.kind == "Virtual" && n.name.rfind("tailscale", 0) != 0)) continue;
        auto it = nets_.find(n.name);
        if (it != nets_.end() && !first_) {
            if (n.up != it->second.first)
                out.push_back({s.wall, "network", n.name + (n.up ? " connected" : " disconnected") + (n.up && !n.ipv4.empty() ? " (" + n.ipv4 + ")" : ""), n.kind});
            else if (n.up && n.ipv4 != it->second.second && !n.ipv4.empty())
                out.push_back({s.wall, "network", n.name + " has a new address: " + n.ipv4, "Was " + (it->second.second.empty() ? "none" : it->second.second)});
        }
        nets_[n.name] = {n.up, n.ipv4};
    }
    if (s.power.on_ac.ok()) {
        int ac = s.power.on_ac.get() ? 1 : 0;
        if (on_ac_ >= 0 && ac != on_ac_) {
            std::string bat = s.power.battery_pct.ok() ? " at " + std::to_string(int(s.power.battery_pct.get())) + "% battery" : "";
            out.push_back({s.wall, "session", ac ? "Plugged into power" + bat : "Unplugged from power" + bat, ""});
        }
        on_ac_ = ac;
    }
}

void LogWatch::tools(const Snapshot& s, std::vector<LogEntry>& out) {
    std::map<int, std::string> names;
    for (const auto& p : s.procs) names[p.key.pid] = p.name;
    std::set<std::pair<int, uint64_t>> alive;
    for (const auto& p : s.procs) {
        std::pair<int, uint64_t> k{p.key.pid, p.key.start_ticks};
        alive.insert(k);
        if (auto r = running_.find(k); r != running_.end()) {
            r->second.last_seen = s.wall;
            continue;
        }
        if (p.kernel_thread) continue;
        std::string argv0 = base(p.cmdline.substr(0, p.cmdline.find(' ')));
        auto use = recognise_tool(p.name, p.cmdline);
        if (!use && argv0 != p.name) use = recognise_tool(argv0, p.cmdline);
        if (!use) continue;
        auto pn = names.find(p.ppid);
        running_[k] = ToolRun{*use, p.name, p.cmdline, pn == names.end() ? "" : pn->second, p.started_epoch, s.wall};
    }
    for (auto it = running_.begin(); it != running_.end();) {
        if (alive.count(it->first)) {
            ++it;
            continue;
        }
        const ToolRun& r = it->second;
        std::string title = r.use.what;
        if (!r.use.port.empty()) {
            auto b = boards_.find(r.use.port);
            title += " on " + r.use.port + (b != boards_.end() ? " (" + b->second + ")" : "");
        }
        if (!r.use.image.empty()) title += ", image " + base(r.use.image);
        // It ended somewhere between the last scan that saw it and this one.
        double secs = std::max(1.0, (r.last_seen + s.wall) / 2 - r.start);
        title += ", took about " + human_duration(secs);
        std::string detail = "Command: " + r.cmdline + (r.parent.empty() ? "" : "\nStarted by: " + r.parent) +
                             (r.use.image.empty() ? "" : "\nImage: " + r.use.image);
        out.push_back({r.start, r.use.category, title, detail});
        finished_tools_.push_back({s.wall, r.name + " (" + r.cmdline + ")"});
        it = running_.erase(it);
    }
    finished_tools_.erase(std::remove_if(finished_tools_.begin(), finished_tools_.end(),
                                         [&](const auto& f) { return s.wall - f.first > 300; }),
                          finished_tools_.end());
}

void LogWatch::first_runs(const Snapshot& s, std::vector<LogEntry>& out) {
    if (!store_) return;
    // Packaged programs live under /usr and /opt; anything else running for
    // the first time is worth a line (downloads, builds, scripts in /tmp).
    std::vector<std::string> fresh;
    std::map<int, std::string> names;
    for (const auto& p : s.procs) names[p.key.pid] = p.name;
    for (const auto& p : s.procs) {
        if (p.exe.empty() || p.kernel_thread) continue;
        std::string exe = p.exe;
        if (auto d = exe.find(" (deleted)"); d != std::string::npos) exe.resize(d);
        if (starts_with(exe, "/usr/") || starts_with(exe, "/opt/") || starts_with(exe, "/snap/")) continue;
        if (!store_->exe_seen(exe)) {
            store_->mark_exe(exe);
            if (exes_seeded_) {
                auto pn = names.find(p.ppid);
                out.push_back({p.started_epoch > 0 ? p.started_epoch : s.wall, "programs", "First run of " + exe,
                               "Command: " + p.cmdline + "\nUser: " + p.user + (pn == names.end() ? "" : "\nStarted by: " + pn->second)});
            }
        }
    }
    if (!exes_seeded_) {
        // The first time, everything already running is simply remembered.
        store_->set_meta("exes_seeded", "1");
        exes_seeded_ = true;
    }
}

}  // namespace as
