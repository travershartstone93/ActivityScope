#include "peerwatch.h"

namespace as {

std::vector<PeerAlert> PeerWatch::feed(const std::vector<ConnectionRecord>& seen, double upload_bps, double now) {
    std::map<std::string, const ConnectionRecord*> latest;
    for (const auto& c : seen) {
        if (c.program == "another account" || c.program == "unknown program") continue;
        // Peer-to-peer traffic goes to high ports. Ordinary use of the web
        // (443, 80, mail, DNS) must never count, or every browser would be
        // accused within its first ten minutes.
        if (c.port < 1024) continue;
        peers_[c.program][c.remote] = now;
        latest[c.program] = &c;
    }
    std::vector<PeerAlert> out;
    for (auto it = peers_.begin(); it != peers_.end();) {
        auto& remotes = it->second;
        for (auto r = remotes.begin(); r != remotes.end();)
            r = now - r->second > window ? remotes.erase(r) : std::next(r);
        if (remotes.empty()) {
            it = peers_.erase(it);
            continue;
        }
        const std::string& program = it->first;
        bool fresh = alerted_.find(program) == alerted_.end() || now - alerted_[program] > 86400;
        if (int(remotes.size()) >= threshold && fresh && !(allowed && allowed(program))) {
            alerted_[program] = now;
            auto l = latest.find(program);
            out.push_back({program, l != latest.end() ? l->second->exe : "", l != latest.end() ? l->second->pid : 0,
                           int(remotes.size()), upload_bps});
        }
        ++it;
    }
    return out;
}

}  // namespace as
