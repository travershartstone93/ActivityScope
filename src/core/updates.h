#pragma once
#include <set>
#include <string>
#include <vector>

namespace as {

struct PendingUpdate {
    std::string name, installed, available;
    bool aur = false;
};

// "linux-cachyos 7.2.2-1 -> 7.2.3-1" lines, as printed by checkupdates and paru -Qua.
std::vector<PendingUpdate> parse_update_list(const std::string& text, bool aur);

// Packages pacman itself will not upgrade: every IgnorePkg in [options].
std::set<std::string> pacman_ignored(const std::string& pacman_conf);

// Kernels, graphics drivers and core system pieces only take effect after a restart.
bool needs_restart(const std::string& name);

// The commands an update runs in a terminal. Held packages are passed as
// --ignore so they are never touched.
std::string update_all_command(const std::set<std::string>& held, bool aur);
std::string update_one_command(const PendingUpdate& u);

}  // namespace as
