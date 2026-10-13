#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "netlog.h"

namespace as {

struct PeerAlert {
    std::string program, exe;
    int pid = 0;
    int peers = 0;      // different computers in the last 10 minutes
    double upload_bps = 0;
};

// Spots peer-to-peer sharing (torrent clients and the like): one program
// talking to many different computers. Per-program byte counts need root, but
// who a program is connected to does not.
class PeerWatch {
public:
    int threshold = 8;
    double window = 600;
    std::function<bool(const std::string& program)> allowed;

    // Feed the connections seen in one round and the machine's upload rate.
    std::vector<PeerAlert> feed(const std::vector<ConnectionRecord>& seen, double upload_bps, double now);

private:
    std::map<std::string, std::map<std::string, double>> peers_;  // program -> remote -> last seen
    std::map<std::string, double> alerted_;                       // program -> when
};

}  // namespace as
