#pragma once
#include <deque>
#include <map>
#include <string>
#include <utility>

#include "fs.h"
#include "snapshot.h"

namespace as {

// Watches the kernel's critical thermal zone (the one that forces a
// shutdown) and the fans. Limits are read from the system, never assumed.
class ThermalGuard {
public:
    explicit ThermalGuard(Roots r) : r_(std::move(r)) {}
    void update(Snapshot& s);

    // How long a fan must stay still while others work hard and the CPU is
    // hot before it is called out.
    static constexpr int kDeadFanSeconds = 30;

private:
    void find_zone(double now);
    Roots r_;
    std::string zone_path_, zone_name_;
    Reading<double> limit_ = Reading<double>::none("No critical thermal zone found");
    double zone_t_ = -1e9;
    std::deque<std::pair<double, double>> history_;  // (time, smoothed temp)
    std::deque<double> recent_;                       // last raw readings
    std::map<std::string, int> still_;                // fan label -> seconds still while hot
};

// How a previous boot ended, from the last lines of its system log.
enum class BootEnd { Clean, Thermal, Asleep, Sudden };
struct BootEndInfo {
    BootEnd kind = BootEnd::Sudden;
    std::string line;  // the log line that decided it, if any
};
BootEndInfo classify_boot_end(const std::string& last_lines);

}  // namespace as
