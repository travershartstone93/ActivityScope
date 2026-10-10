#include "logbook.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <ctime>
#include <regex>

#include "events.h"
#include "fs.h"

namespace as {

namespace {

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

std::string after(const std::string& s, const std::string& key) {
    auto p = s.find(key);
    if (p == std::string::npos) return {};
    auto start = p + key.size();
    auto end = s.find(" ; ", start);
    return trim(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

}  // namespace

std::optional<LogEntry> classify_journal(const JournalLine& j) {
    const std::string& m = j.message;
    LogEntry e;
    e.t = j.t;
    e.detail = (j.identifier.empty() ? std::string() : j.identifier + ": ") + m;
    auto make = [&](const char* cat, std::string title) {
        e.category = cat;
        e.title = std::move(title);
        return std::optional<LogEntry>(e);
    };

    if (j.identifier == "sudo" && has(m, "COMMAND=")) {
        std::string user = trim(m.substr(0, m.find(':')));
        return make("admin", "Ran as administrator (sudo) by " + user + ": " + after(m, "COMMAND="));
    }
    if (j.identifier == "sudo" && has(m, "incorrect password")) return make("admin", "Wrong password typed for sudo");
    if (j.identifier == "pkexec" && has(m, "Executing command")) return make("admin", "Ran as administrator (pkexec): " + trim(m.substr(m.find("Executing command") + 18)));
    if (j.identifier == "polkitd" && has(m, "Operator of")) return make("admin", "Administrator approval: " + m);

    if (j.identifier == "systemd-sleep" && has(m, "Performing sleep operation")) return make("session", "Went to sleep");
    if (j.identifier == "systemd-sleep" && has(m, "System returned from sleep")) return make("session", "Woke from sleep");
    if (j.identifier == "systemd-logind") {
        if (has(m, "Lid closed")) return make("session", "Lid closed");
        if (has(m, "Lid opened")) return make("session", "Lid opened");
        if (has(m, "Power key pressed")) return make("session", "Power button pressed");
        if (has(m, "System is powering down")) return make("session", "Shutting down");
        if (has(m, "System is rebooting")) return make("session", "Restarting");
        if (has(m, "New session") && has(m, "of user")) return make("session", "Logged in: " + trim(m.substr(m.find("of user") + 8)));
        if (has(m, "Removed session")) return make("session", "Logged out (" + m + ")");
    }

    if (j.identifier == "NetworkManager") {
        static const std::regex def("set '([^']+)' \\(([^)]+)\\) as default for IPv4");
        std::smatch r;
        if (std::regex_search(m, r, def)) return make("network", "Connected to " + r[1].str() + " on " + r[2].str());
        if (has(m, "state change: activated -> deactivating") || has(m, "state change: activated -> disconnected")) {
            static const std::regex dev("device \\(([^)]+)\\)");
            std::string d = std::regex_search(m, r, dev) ? r[1].str() : "a network device";
            if (d.rfind("p2p-", 0) != 0) return make("network", "Disconnected: " + d);
        }
    }
    if (j.identifier == "tailscaled" && (has(m, "Switching ipn state Stopped -> Running") || has(m, "state Running"))) return make("network", "Tailscale connected");

    if (j.identifier == "systemd" && (has(m, "Failed with result") || has(m, "Main process exited, code=dumped"))) {
        std::string unit = trim(m.substr(0, m.find(':')));
        return make("services", unit + " failed: " + trim(m.substr(m.find(':') + 1)));
    }
    if (j.identifier == "systemd-coredump" && has(m, "dumped core")) {
        static const std::regex prog("Process [0-9]+ \\(([^)]+)\\)");
        std::smatch r;
        return make("problems", (std::regex_search(m, r, prog) ? r[1].str() : std::string("A program")) + " crashed");
    }

    if (j.kernel) {
        static const std::regex oom("Killed process [0-9]+ \\(([^)]+)\\)");
        std::smatch r;
        if (std::regex_search(m, r, oom)) return make("problems", "Out of memory: the kernel killed " + r[1].str());
        if (has(m, "segfault at")) return make("problems", trim(m.substr(0, m.find('['))) + " crashed (segmentation fault)");
        if (has(m, "I/O error, dev")) return make("problems", "Disk read or write error: " + m);
        if (has(m, "EXT4-fs error") || has(m, "BTRFS error") || has(m, "BTRFS critical")) return make("problems", "Filesystem error: " + m);
        if (has(m, "critical temperature reached")) return make("problems", "Critical temperature reached; the system is shutting itself down");
        if (has(m, "Hardware Error") || has(m, "Machine check")) return make("problems", "Hardware error reported: " + m);
        if (has(m, "device descriptor read") || has(m, "unable to enumerate USB device") || has(m, "not accepting address"))
            return make("devices", "A USB device failed to connect: " + m);
    }
    return std::nullopt;
}

double parse_pacman_time(const std::string& line) {
    // [2026-10-10T09:36:33-0400]
    if (line.size() < 26 || line[0] != '[') return 0;
    tm t{};
    if (!strptime(line.c_str() + 1, "%Y-%m-%dT%H:%M:%S%z", &t)) return 0;
    long off = t.tm_gmtoff;
    return double(timegm(&t) - off);
}

std::optional<LogEntry> PacmanParser::feed(const std::string& line) {
    double t = parse_pacman_time(line);
    auto msg_at = line.find("] [");
    if (!t || msg_at == std::string::npos) return std::nullopt;
    auto end = line.find(']', msg_at + 3);
    std::string who = line.substr(msg_at + 3, end - msg_at - 3);
    std::string msg = trim(line.substr(end + 1));
    if (who == "PACMAN" && starts_with(msg, "Running '")) {
        command_ = msg.substr(9, msg.size() - 10);
        return std::nullopt;
    }
    if (who != "ALPM") return std::nullopt;
    if (msg == "transaction started") {
        open_ = true;
        t_ = t;
        installed_.clear(), upgraded_.clear(), removed_.clear(), downgraded_.clear();
        return std::nullopt;
    }
    auto pkg = [&](const char* verb) {
        std::string rest = msg.substr(std::string(verb).size() + 1);
        return rest;
    };
    if (starts_with(msg, "installed ")) installed_.push_back(pkg("installed"));
    else if (starts_with(msg, "upgraded ")) upgraded_.push_back(pkg("upgraded"));
    else if (starts_with(msg, "removed ")) removed_.push_back(pkg("removed"));
    else if (starts_with(msg, "downgraded ")) downgraded_.push_back(pkg("downgraded"));
    if (msg != "transaction completed" || !open_) return std::nullopt;
    open_ = false;

    LogEntry e;
    e.t = t_;
    e.category = "software";
    std::vector<std::string> parts;
    auto name = [](const std::string& p) { return p.substr(0, p.find(' ')); };
    auto summarise = [&](const std::vector<std::string>& v, const char* verb) {
        if (v.empty()) return;
        if (v.size() <= 3) {
            std::string names;
            for (std::size_t i = 0; i < v.size(); ++i) names += (i ? ", " : "") + name(v[i]);
            parts.push_back(std::string(verb) + " " + names);
        } else {
            parts.push_back(std::string(verb) + " " + std::to_string(v.size()) + " packages");
        }
    };
    summarise(installed_, "Installed");
    summarise(upgraded_, "Upgraded");
    summarise(downgraded_, "Downgraded");
    summarise(removed_, "Removed");
    if (parts.empty()) return std::nullopt;
    for (std::size_t i = 0; i < parts.size(); ++i) e.title += (i ? "; " : "") + (i ? std::string(1, char(std::tolower(parts[i][0]))) + parts[i].substr(1) : parts[i]);
    if (!command_.empty()) e.detail = "Command: " + command_ + "\n";
    auto list = [&](const std::vector<std::string>& v, const char* head) {
        if (v.empty()) return;
        e.detail += std::string(head) + ":\n";
        for (const auto& p : v) e.detail += "  " + p + "\n";
    };
    list(installed_, "Installed");
    list(upgraded_, "Upgraded");
    list(downgraded_, "Downgraded");
    list(removed_, "Removed");
    command_.clear();
    return e;
}

std::optional<ToolUse> recognise_tool(const std::string& name, const std::string& cmd) {
    // A shell running "esptool.py ..." only mentions the tool; the tool shows
    // up as its own process and is logged from there.
    for (const char* shell : {"bash", "zsh", "sh", "fish", "dash", "ksh", "sudo", "timeout", "nohup", "env"})
        if (name == shell) return std::nullopt;
    auto args = split_ws(cmd);
    std::vector<std::string> a(args.begin(), args.end());
    auto value_after = [&](std::initializer_list<const char*> flags) -> std::string {
        for (std::size_t i = 0; i < a.size(); ++i)
            for (const char* f : flags) {
                std::string fl = f;
                if (a[i] == fl && i + 1 < a.size()) return a[i + 1];
                if (starts_with(a[i], fl + "=")) return a[i].substr(fl.size() + 1);
            }
        return {};
    };
    auto first_with = [&](std::initializer_list<const char*> exts) -> std::string {
        for (const auto& x : a)
            for (const char* e : exts)
                if (x.size() > std::string(e).size() && x.ends_with(e)) return x;
        return {};
    };
    const bool esptool = name == "esptool" || name == "esptool.py" || has(cmd, "esptool.py ") || has(cmd, "/esptool ") ||
                         has(cmd, "esptool write_flash") || has(cmd, " -m esptool");
    if (esptool && (has(cmd, "write_flash") || has(cmd, "write-flash")))
        return ToolUse{"flashing", "Flashed an ESP board with esptool", value_after({"--port", "-p"}), first_with({".bin"})};
    if (esptool && (has(cmd, "erase_flash") || has(cmd, "erase-flash")))
        return ToolUse{"flashing", "Erased an ESP board's flash with esptool", value_after({"--port", "-p"}), {}};
    if (name == "arduino-cli" && a.size() > 1 && (a[1] == "upload" || has(cmd, " upload ")))
        return ToolUse{"flashing", "Uploaded a sketch with arduino-cli", value_after({"--port", "-p"}), value_after({"--input-file", "-i", "--input-dir"})};
    if ((name == "idf.py" || has(cmd, "idf.py")) && has(cmd, " flash"))
        return ToolUse{"flashing", "Flashed a board with ESP-IDF (idf.py)", value_after({"--port", "-p"}), {}};
    if ((name == "pio" || name == "platformio") && has(cmd, "upload"))
        return ToolUse{"flashing", "Uploaded firmware with PlatformIO", value_after({"--upload-port"}), {}};
    if (name == "avrdude") return ToolUse{"flashing", "Flashed a board with avrdude", value_after({"-P"}), value_after({"-U"})};
    if (name == "dfu-util") return ToolUse{"flashing", "Flashed a device over DFU", {}, value_after({"-D", "--download"})};
    if (name == "picotool" && has(cmd, "load")) return ToolUse{"flashing", "Loaded firmware onto a Raspberry Pi Pico", {}, first_with({".uf2", ".elf", ".bin"})};
    if (name == "openocd" && has(cmd, "program")) return ToolUse{"flashing", "Programmed a board with OpenOCD", {}, first_with({".elf", ".bin", ".hex"})};
    if (name == "bossac" || name == "stm32flash" || name == "st-flash")
        return ToolUse{"flashing", "Flashed a board with " + name, {}, first_with({".bin", ".hex", ".elf"})};

    if (starts_with(name, "mkfs") || name == "mkdosfs" || name == "mke2fs") {
        std::string dev;
        for (const auto& x : a)
            if (starts_with(x, "/dev/")) dev = x;
        return ToolUse{"storage", "Formatted " + (dev.empty() ? std::string("a drive") : dev) + " with " + name, dev, {}};
    }
    if (name == "wipefs") return ToolUse{"storage", "Wiped filesystem signatures with wipefs", a.empty() ? "" : a.back(), {}};
    if (name == "dd") {
        std::string of = value_after({"of"}), in = value_after({"if"});
        if (starts_with(of, "/dev/")) return ToolUse{"storage", "Wrote an image to " + of + " with dd", of, in};
    }
    if (name == "parted" || name == "sgdisk" || name == "fdisk" || name == "gdisk" || name == "sfdisk" || name == "cfdisk")
        return ToolUse{"storage", "Changed a partition table with " + name, a.size() > 1 ? a.back() : "", {}};
    if (name == "gparted" || name == "gpartedbin") return ToolUse{"storage", "Opened GParted (partition editor)", {}, {}};
    if (name == "cryptsetup" && (has(cmd, "luksFormat"))) return ToolUse{"storage", "Encrypted a drive with cryptsetup", {}, {}};
    if (name == "balena-etcher" || name == "balenaEtcher" || name == "etcher") return ToolUse{"storage", "Opened balenaEtcher (image writer)", {}, {}};
    return std::nullopt;
}

std::vector<LogEntry> Repeats::feed(std::vector<LogEntry> in, double now) {
    std::vector<LogEntry> out;
    for (auto& e : in) {
        // "zsh crashed (segmentation fault)" from the kernel and "zsh
        // crashed" from the crash reporter describe one crash: pair them once.
        if (e.category == "problems" && e.title.find(" crashed") != std::string::npos) {
            std::string prog = e.title.substr(0, e.title.find(" crashed"));
            bool kernel = e.title.find("(segmentation fault)") != std::string::npos;
            auto partner = std::find_if(crashes_.begin(), crashes_.end(), [&](const Crash& c) {
                return !c.paired && c.program == prog && c.kernel != kernel && std::abs(c.t - e.t) < 5;
            });
            if (partner != crashes_.end()) {
                partner->paired = true;
                continue;
            }
            crashes_.push_back({prog, e.t, kernel, false});
            e.title = prog + " crashed";
        }
        auto it = std::find_if(runs_.begin(), runs_.end(), [&](const Run& r) { return r.first.title == e.title && e.t - r.last < 60; });
        if (it != runs_.end()) {
            it->last = e.t;
            ++it->count;
            continue;
        }
        runs_.push_back({e, e.t, 1});
        out.push_back(e);
    }
    crashes_.erase(std::remove_if(crashes_.begin(), crashes_.end(), [&](const Crash& c) { return now - c.t > 30; }), crashes_.end());
    for (auto it = runs_.begin(); it != runs_.end();) {
        if (now - it->last < 60) {
            ++it;
            continue;
        }
        if (it->count > 1) {
            LogEntry sum = it->first;
            sum.t = it->last;
            sum.title += " " + std::to_string(it->count) + " times between " + clock_time(it->first.t) + " and " + clock_time(it->last);
            out.push_back(sum);
        }
        it = runs_.erase(it);
    }
    return out;
}

std::string board_name(const std::string& vid_in, const std::string& pid_in) {
    std::string vid = vid_in, pid = pid_in;
    std::transform(vid.begin(), vid.end(), vid.begin(), ::tolower);
    std::transform(pid.begin(), pid.end(), pid.begin(), ::tolower);
    if (vid == "303a") {
        if (pid == "1001") return "ESP32 board with built-in USB (S3, C3, C6 or H2)";
        if (pid == "0002") return "ESP32-S2 board";
        return "Espressif ESP32 board";
    }
    if (vid == "10c4" && pid == "ea60") return "CP210x USB-to-serial adapter (common on ESP32 boards)";
    if (vid == "1a86" && (pid == "7523" || pid == "55d4" || pid == "7522")) return "CH340/CH9102 USB-to-serial adapter (common on ESP32 and Arduino clones)";
    if (vid == "0403") return "FTDI USB-to-serial adapter";
    if (vid == "2341" || vid == "2a03") return "Arduino board";
    if (vid == "2e8a") return "Raspberry Pi RP2040/RP2350 board";
    if (vid == "239a") return "Adafruit board";
    if (vid == "1366") return "SEGGER J-Link debugger";
    if (vid == "0483" && pid == "3748") return "ST-Link debugger";
    if (vid == "0483" && pid == "df11") return "STM32 in DFU mode";
    return {};
}

}  // namespace as
