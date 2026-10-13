#include "netlog.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace as {

namespace {

// Strict hex: the exact length expected, hex digits only, or nothing.
// /proc/net lines can be cut mid-line when a read hits its size cap, and a
// parser that throws there takes the whole recorder down.
std::optional<uint64_t> hex_field(std::string_view s) {
    if (s.empty() || s.size() > 16) return std::nullopt;
    uint64_t v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return std::nullopt;
        v = v * 16 + uint64_t(d);
    }
    return v;
}

}  // namespace

std::string format_address(const std::string& hex, bool v6, int* port) {
    auto colon = hex.find(':');
    if (colon == std::string::npos || colon != std::size_t(v6 ? 32 : 8)) return {};
    std::string ip = hex.substr(0, colon);
    auto pv = hex_field(hex.substr(colon + 1));
    if (!pv || *pv > 65535) return {};
    if (port) *port = int(*pv);
    char buf[INET6_ADDRSTRLEN] = {};
    if (!v6) {
        auto w = hex_field(ip);
        if (!w) return {};
        in_addr a{};
        a.s_addr = uint32_t(*w);  // already in network order as stored
        inet_ntop(AF_INET, &a, buf, sizeof buf);
    } else {
        in6_addr a{};
        for (int w = 0; w < 4; ++w) {
            auto word = hex_field(std::string_view(ip).substr(std::size_t(w) * 8, 8));
            if (!word) return {};
            uint32_t v = uint32_t(*word);
            std::memcpy(&a.s6_addr[w * 4], &v, 4);  // each word is in host (little-endian) order
        }
        inet_ntop(AF_INET6, &a, buf, sizeof buf);
        std::string s = buf;
        if (starts_with(s, "::ffff:") && s.find('.') != std::string::npos) return s.substr(7);  // IPv4 inside IPv6
    }
    return buf;
}

std::vector<Socket> parse_proc_net(const std::string& text, const std::string& proto) {
    std::vector<Socket> out;
    const bool v6 = proto.back() == '6';
    bool header = true;
    for (const auto& line : split(text, '\n')) {
        if (header) {
            header = false;
            continue;
        }
        auto f = split_ws(line);
        if (f.size() < 10) continue;
        Socket s;
        s.proto = proto.substr(0, 3) == "tcp" ? "TCP" : "UDP";
        s.local = format_address(std::string(f[1]), v6, &s.local_port);
        s.remote = format_address(std::string(f[2]), v6, &s.remote_port);
        auto state = hex_field(f[3]);
        if (s.local.empty() || s.remote.empty() || !state) continue;  // cut or malformed line
        s.state = int(*state);
        s.uid = int(to_int(f[7]).value_or(-1));
        s.inode = to_uint(f[9]).value_or(0);
        out.push_back(s);
    }
    return out;
}

std::vector<ConnectionRecord> NetLog::feed(const Snapshot& snap) {
    std::vector<ConnectionRecord> out;
    if (snap.wall - last_ < 15) return out;
    last_ = snap.wall;
    std::vector<Socket> socks;
    for (const char* proto : {"tcp", "tcp6", "udp", "udp6"})
        if (auto text = read_file(r_.proc + "/net/" + proto, 4 << 20)) {
            auto v = parse_proc_net(*text, proto);
            socks.insert(socks.end(), v.begin(), v.end());
        }
    // Only connections to somewhere else: established TCP, or UDP with a peer.
    std::vector<const Socket*> remote;
    for (const auto& s : socks) {
        bool established = s.proto == "TCP" ? s.state == 1 : s.remote_port != 0;
        if (!established || s.remote_port == 0) continue;
        if (starts_with(s.remote, "127.") || s.remote == "::1") continue;
        remote.push_back(&s);
    }
    // Search only for your own sockets not looked for yet; other accounts'
    // sockets cannot be traced to a program without administrator rights.
    bool unknown = false;
    for (const auto* s : remote)
        if (s->uid == int(getuid()) && !owner_.count(s->inode) && !looked_.count(s->inode)) unknown = true;
    if (unknown) {
        // Programs that already own sockets (browsers, torrent clients) open
        // new ones all the time: look at them first, and search every program
        // at most every 30 seconds.
        const int me = int(getuid());
        std::set<int> known_pids;
        for (const auto& [inode, o] : owner_) known_pids.insert(o.first);
        auto scan = [&](const ProcessInfo& p, std::map<uint64_t, std::pair<int, std::string>>& into) {
            const std::string fd = r_.proc + "/" + std::to_string(p.key.pid) + "/fd";
            for (const auto& e : list_dir(fd)) {
                std::string target = read_link(fd + "/" + e);
                if (!starts_with(target, "socket:[")) continue;
                into[to_uint(std::string_view(target).substr(8, target.size() - 9)).value_or(0)] = {p.key.pid, p.name};
            }
        };
        std::map<uint64_t, std::pair<int, std::string>> found;
        for (const auto& p : snap.procs)
            if (known_pids.count(p.key.pid)) scan(p, found);
        bool missing = false;
        for (const auto* s : remote)
            if (s->uid == me && !found.count(s->inode) && !looked_.count(s->inode)) missing = true;
        if (missing && snap.wall - last_full_ >= 30) {
            last_full_ = snap.wall;
            for (const auto& p : snap.procs)
                if (p.uid == me && !p.kernel_thread && !known_pids.count(p.key.pid)) scan(p, found);
            if (looked_.size() > 50000) looked_.clear();
            for (const auto* s : remote) looked_.insert(s->inode);
        }
        owner_ = std::move(found);
    }
    std::unordered_map<int, const ProcessInfo*> byPid;
    for (const auto& p : snap.procs) byPid[p.key.pid] = &p;
    for (const auto* s : remote) {
        auto o = owner_.find(s->inode);
        std::string program = o != owner_.end() ? o->second.second : (s->uid == int(getuid()) ? "unknown program" : "another account");
        std::string exe;
        int pid = 0;
        if (o != owner_.end()) {
            pid = o->second.first;
            if (auto p = byPid.find(pid); p != byPid.end()) exe = p->second->exe;
        }
        std::string key = program + "|" + s->remote + "|" + std::to_string(s->remote_port) + "|" + s->proto;
        auto [it, fresh] = pairs_.try_emplace(key);
        ConnectionRecord& r = it->second;
        if (fresh) {
            r = {program, exe, s->remote, s->proto, s->remote_port, pid, snap.wall, snap.wall, true};
        } else {
            r.last_seen = snap.wall;
            r.is_new = false;
        }
        out.push_back(r);
    }
    // The database keeps every pair; memory only needs the recent ones.
    for (auto it = pairs_.begin(); it != pairs_.end();)
        it = snap.wall - it->second.last_seen > 600 ? pairs_.erase(it) : std::next(it);
    return out;
}

}  // namespace as
