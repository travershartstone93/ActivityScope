#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "providers.h"

namespace as {

void NetProvider::sample(std::vector<NetSample>& out, double now) {
    auto dev = read_file(r_.proc + "/net/dev");
    if (!dev) throw std::runtime_error("cannot read /proc/net/dev");
    double dt = prev_t_ > 0 ? now - prev_t_ : 0;
    std::map<std::string, Prev> next;

    for (const auto& line : split(*dev, '\n')) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        NetSample n;
        n.name = trim(line.substr(0, colon));
        auto f = split_ws(std::string_view(line).substr(colon + 1));
        if (f.size() < 16) continue;
        Prev cur{to_uint(f[0]).value_or(0), to_uint(f[8]).value_or(0)};
        n.rx_total = cur.rx;
        n.tx_total = cur.tx;
        auto it = prev_.find(n.name);
        if (it != prev_.end() && dt > 0) {
            n.rx_bps = double(cur.rx - it->second.rx) / dt;
            n.tx_bps = double(cur.tx - it->second.tx) / dt;
        }
        next[n.name] = cur;

        const std::string cls = r_.sys + "/class/net/" + n.name;
        if (n.name == "lo") n.kind = "Loopback";
        else if (exists(cls + "/wireless") || exists(cls + "/phy80211")) n.kind = "Wi-Fi";
        else if (exists(cls + "/device")) n.kind = "Ethernet";
        else n.kind = "Virtual";
        auto state = read_line(cls + "/operstate").value_or("");
        n.up = state == "up" || (state == "unknown" && read_int(cls + "/carrier").value_or(0) == 1);
        n.mac = read_line(cls + "/address").value_or("");
        if (auto sp = read_int(cls + "/speed"); sp && *sp > 0) n.speed_mbps = Reading<int>::of(int(*sp));
        else n.speed_mbps = Reading<int>::none(n.kind == "Wi-Fi" ? "Wi-Fi does not report a fixed speed" : "Not reported");
        n.ssid = Reading<std::string>::none("Not read");
        out.push_back(std::move(n));
    }
    prev_ = std::move(next);
    prev_t_ = now;

    // Addresses only come from the live system, never from the test roots,
    // and change rarely, so they are refreshed every 10 s.
    if (r_.proc != "/proc") return;
    if (now - addrs_t_ < 10) {
        for (auto& n : out)
            if (auto it = addrs_.find(n.name); it != addrs_.end()) n.ipv4 = it->second.first, n.ipv6 = it->second.second;
        return;
    }
    addrs_t_ = now;
    addrs_.clear();
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr) continue;
        char buf[INET6_ADDRSTRLEN] = {};
        int fam = a->ifa_addr->sa_family;
        if (fam == AF_INET) inet_ntop(fam, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, buf, sizeof buf);
        else if (fam == AF_INET6) inet_ntop(fam, &reinterpret_cast<sockaddr_in6*>(a->ifa_addr)->sin6_addr, buf, sizeof buf);
        else continue;
        for (auto& n : out) {
            if (n.name != a->ifa_name) continue;
            if (fam == AF_INET && n.ipv4.empty()) n.ipv4 = buf;
            if (fam == AF_INET6 && n.ipv6.empty() && !starts_with(buf, "fe80")) n.ipv6 = buf;
        }
    }
    freeifaddrs(list);
    for (const auto& n : out) addrs_[n.name] = {n.ipv4, n.ipv6};
}

}  // namespace as
