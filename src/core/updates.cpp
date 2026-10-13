#include "updates.h"

#include "fs.h"

namespace as {

std::vector<PendingUpdate> parse_update_list(const std::string& text, bool aur) {
    std::vector<PendingUpdate> out;
    for (const auto& line : split(text, '\n')) {
        auto f = split_ws(line);
        // name old -> new   (paru may append extra notes; they are ignored)
        if (f.size() < 4 || f[2] != "->") continue;
        out.push_back({std::string(f[0]), std::string(f[1]), std::string(f[3]), aur});
    }
    return out;
}

std::set<std::string> pacman_ignored(const std::string& conf) {
    std::set<std::string> out;
    bool options = false;
    for (const auto& raw : split(conf, '\n')) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            options = line == "[options]";
            continue;
        }
        if (!options || !starts_with(line, "IgnorePkg")) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        for (auto name : split_ws(std::string_view(line).substr(eq + 1))) out.insert(std::string(name));
    }
    return out;
}

bool needs_restart(const std::string& n) {
    if (n == "linux" || starts_with(n, "linux-")) return n.find("headers") == std::string::npos && n.find("docs") == std::string::npos &&
                                                         n.find("api-headers") == std::string::npos && n != "linux-firmware-whence";
    for (const char* p : {"nvidia", "systemd", "glibc", "mesa", "lib32-mesa", "dbus", "intel-ucode", "amd-ucode", "plasma-workspace",
                          "kwin", "mkinitcpio", "dracut"})
        if (n == p || starts_with(n, std::string(p) + "-")) return true;
    return n.find("nvidia") != std::string::npos;
}

namespace {
std::string ignore_flag(const std::set<std::string>& held) {
    if (held.empty()) return {};
    std::string s = " --ignore ";
    bool first = true;
    for (const auto& h : held) {
        s += (first ? "" : ",") + h;
        first = false;
    }
    return s;
}
}  // namespace

std::string update_all_command(const std::set<std::string>& held, bool aur) {
    std::string cmd = "sudo pacman -Syu" + ignore_flag(held);
    if (aur) cmd += " && paru -Sua" + ignore_flag(held);
    return cmd;
}

std::string update_one_command(const PendingUpdate& u) {
    // Never -Sy here: refreshing the lists and installing one package is the
    // partial upgrade Arch warns about. -S installs against the current lists.
    return u.aur ? "paru -S " + u.name : "sudo pacman -S --needed " + u.name;
}

}  // namespace as
