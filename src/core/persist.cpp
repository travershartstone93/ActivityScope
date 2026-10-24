#include "persist.h"

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "fs.h"

namespace as {

std::string line_diff(const std::string& before, const std::string& after, int max_lines) {
    auto a = split(before, '\n'), b = split(after, '\n');
    std::multiset<std::string> in_a(a.begin(), a.end()), in_b(b.begin(), b.end());
    std::string out;
    int n = 0;
    for (const auto& l : b) {
        auto it = in_a.find(l);
        if (it != in_a.end()) {
            in_a.erase(it);
            continue;
        }
        if (trim(l).empty()) continue;
        if (n++ < max_lines) out += "+ " + l + "\n";
    }
    for (const auto& l : a) {
        auto it = in_b.find(l);
        if (it != in_b.end()) {
            in_b.erase(it);
            continue;
        }
        if (trim(l).empty()) continue;
        if (n++ < max_lines) out += "- " + l + "\n";
    }
    if (n > max_lines) out += "(and " + std::to_string(n - max_lines) + " more changed lines)\n";
    return out;
}

StartupWatch::StartupWatch(std::string home, std::string etc) {
    if (home.empty()) home = std::getenv("HOME") ? std::getenv("HOME") : "";
    fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ < 0) return;
    const std::string c = home + "/.config";
    add(c + "/autostart", "Autostart entry");
    add(c + "/systemd/user", "Your background service", {}, true);
    add(home + "/.local/bin", "Program in ~/.local/bin");
    add(c + "/autostart-scripts", "Login script");
    add(c + "/plasma-workspace/env", "Plasma login script");
    add(c + "/plasma-workspace/shutdown", "Plasma logout script");
    add(c + "/environment.d", "Session environment file");
    add(c + "/fish", "Fish shell setting", {"config.fish"});
    add(c + "/fish/conf.d", "Fish shell startup file");
    add(home, "Shell startup file", {".bashrc", ".bash_profile", ".profile", ".zshrc", ".zprofile", ".zshenv", ".xprofile", ".xinitrc", ".pam_environment"});
    add(etc, "System file", {"hosts", "environment", "ld.so.preload", "profile", "bash.bashrc", "crontab", "fstab", "resolv.conf",
                             "passwd", "group", "shadow", "sudoers", "nsswitch.conf", "pacman.conf", "rc.local"});
    add(etc + "/systemd/system", "System service", {}, true);
    add(etc + "/systemd/user", "Service for every user", {}, true);
    add(etc + "/xdg/autostart", "System autostart entry");
    add(etc + "/profile.d", "System shell startup file");
    add(etc + "/cron.d", "Scheduled job");
    for (const char* p : {"/cron.hourly", "/cron.daily", "/cron.weekly", "/cron.monthly"}) add(etc + p, "Scheduled job");
    add(etc + "/udev/rules.d", "Device rule");
    add(etc + "/modules-load.d", "Kernel module loaded at boot");
    add(etc + "/modprobe.d", "Kernel module setting");
    add(etc + "/pacman.d/hooks", "Package manager hook");
}

StartupWatch::~StartupWatch() {
    if (fd_ >= 0) ::close(fd_);
}

void StartupWatch::add(const std::string& path, const std::string& what, std::set<std::string> only, bool recurse) {
    struct stat st {};
    if (fd_ < 0 || ::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
    // IN_MODIFY keeps a file that is still being written from going quiet.
    int wd = inotify_add_watch(fd_, path.c_str(), IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_ATTRIB);
    if (wd < 0) return;  // not readable as this user
    dirs_[wd] = {path, what, only, recurse};
    for (const auto& name : list_dir(path)) {
        if (!only.empty() && !only.count(name)) continue;
        std::string full = path + "/" + name;
        struct stat s2 {};
        if (::lstat(full.c_str(), &s2) != 0) continue;
        present_.insert(full);
        if (S_ISDIR(s2.st_mode)) {
            if (recurse) add(full, what, {}, true);
        } else {
            remember(full);
        }
    }
}

void StartupWatch::remember(const std::string& file) {
    struct stat st {};
    if (::stat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 64 * 1024) return;
    auto text = read_file(file, 64 * 1024);
    if (!text || text->find('\0') != std::string::npos) return;  // binary or unreadable
    contents_[file] = *text;
}

std::string StartupWatch::describe(const Dir& d, const std::string& name, const std::string& change) const {
    std::string where = d.path + "/" + name;
    if (name == "ld.so.preload" && change != "removed")
        return "Warning: " + where + " was " + change + ". Every program will now load the libraries it lists.";
    std::string what = d.what;
    if (name.ends_with(".wants") || d.path.ends_with(".wants")) what = "Service enabled to start automatically";
    return what + " " + change + ": " + where;
}

std::vector<LogEntry> StartupWatch::poll(double now) {
    std::vector<LogEntry> out;
    if (fd_ < 0) return out;
    alignas(inotify_event) char buf[16384];
    for (;;) {
        ssize_t n = ::read(fd_, buf, sizeof buf);
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            p += sizeof(inotify_event) + ev->len;
            auto d = dirs_.find(ev->wd);
            if (d == dirs_.end() || ev->len == 0) continue;
            std::string name = ev->name;
            if (!d->second.only.empty() && !d->second.only.count(name)) continue;
            if (name.ends_with("~") || name.ends_with(".swp") || name.ends_with(".tmp") || name[0] == '.') {
                if (d->second.only.empty() || !d->second.only.count(name)) continue;  // editor scratch files
            }
            std::string full = d->second.path + "/" + name;
            std::string change = (ev->mask & (IN_CREATE | IN_MOVED_TO)) ? "added"
                               : (ev->mask & (IN_DELETE | IN_MOVED_FROM)) ? "removed"
                               : (ev->mask & IN_ATTRIB) ? "permissions changed"
                                                        : "changed";
            if ((ev->mask & IN_ISDIR) && d->second.recurse && change == "added") add(full, d->second.what, {}, true);
            auto& pend = pending_[full];
            // The verb is settled once the file goes quiet; only a change of
            // permissions alone is told apart here.
            if (pend.second.empty() || change != "permissions changed") pend.second = change;
            pend.first = now;
            wd_of_file_[full] = ev->wd;
        }
    }
    // Report a file once it has been quiet for 2 s.
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second.first < 2) {
            ++it;
            continue;
        }
        const std::string& file = it->first;
        std::string change = it->second.second;
        const Dir& d = dirs_[wd_of_file_[file]];
        std::string name = file.substr(file.rfind('/') + 1);
        struct stat st {};
        bool gone = ::lstat(file.c_str(), &st) != 0;
        bool was = present_.count(file) > 0;
        if (gone) present_.erase(file);
        else present_.insert(file);
        // Judged by the file's life, not by the events or its name: one that
        // came and went within this quiet spell (an installer's or sed's
        // temporary file) changed nothing; one there before and after was
        // changed, however it was written (in place, or renamed over).
        if (!was && gone) {
            wd_of_file_.erase(file);
            it = pending_.erase(it);
            continue;
        }
        if (gone) change = "removed";
        else if (!was) change = "added";
        else if (change != "permissions changed") change = "changed";
        LogEntry e;
        e.t = it->second.first;
        e.category = name == "ld.so.preload" && !gone ? "problems" : "startup";
        e.title = describe(d, name, change);
        auto before = contents_.find(file);
        if (!gone) {
            if (S_ISLNK(st.st_mode)) e.detail = "Link to " + read_link(file) + "\n";
            auto text = read_file(file, 64 * 1024);
            if (text && text->find('\0') == std::string::npos) {
                e.detail += line_diff(before == contents_.end() ? "" : before->second, *text);
                contents_[file] = *text;
            } else if (!text) {
                e.detail += "Its contents are not readable as your user.\n";
            }
            if (st.st_mode & S_IXUSR) e.detail += "The file can be run as a program.\n";
        } else if (before != contents_.end()) {
            e.detail = "It contained:\n" + line_diff("", before->second);
            contents_.erase(before);
        }
        out.push_back(std::move(e));
        wd_of_file_.erase(file);
        it = pending_.erase(it);
    }
    return out;
}

}  // namespace as
