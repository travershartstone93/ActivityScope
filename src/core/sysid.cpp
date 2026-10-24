#include "sysid.h"

#include <sys/utsname.h>
#include <unistd.h>

#include <cstdlib>

namespace as {

namespace {

// PRETTY_NAME="CachyOS Linux" -> CachyOS Linux
std::string os_release_pretty(const std::string& etc) {
    auto text = read_file(etc + "/os-release", 1 << 14);
    if (!text) return "";
    for (const auto& line : split(*text, '\n')) {
        if (!starts_with(line, "PRETTY_NAME=")) continue;
        std::string v = line.substr(12);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return "";
}

}  // namespace

SystemId system_identity(const Roots& r) {
    SystemId id;
    id.vendor = read_line(r.sys + "/class/dmi/id/sys_vendor").value_or("");
    id.product = read_line(r.sys + "/class/dmi/id/product_name").value_or("");
    id.os_pretty = os_release_pretty(r.etc);

    struct utsname u {};
    if (uname(&u) == 0) {
        id.kernel = u.release;
        id.arch = u.machine;
        if (id.hostname.empty()) id.hostname = u.nodename;
    }
    char host[256] = {};
    if (gethostname(host, sizeof host - 1) == 0 && host[0]) id.hostname = host;

    if (auto up = read_line(r.proc + "/uptime")) {
        id.uptime_seconds = int64_t(std::strtod(up->c_str(), nullptr));
    }
    return id;
}

}  // namespace as
