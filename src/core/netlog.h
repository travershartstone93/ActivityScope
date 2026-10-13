#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fs.h"
#include "snapshot.h"

namespace as {

// One program talking to one remote address and port.
struct ConnectionRecord {
    std::string program, exe, remote, proto;
    int port = 0, pid = 0;
    double first_seen = 0, last_seen = 0;
    bool is_new = false;  // first time this pair was seen
};

struct Socket {
    std::string proto, local, remote;
    int local_port = 0, remote_port = 0, state = 0;
    uint64_t inode = 0;
    int uid = -1;
};

// Parses /proc/net/{tcp,tcp6,udp,udp6} content.
std::vector<Socket> parse_proc_net(const std::string& text, const std::string& proto);
std::string format_address(const std::string& hex, bool v6, int* port);

// Every 15 s, notes which program has a connection open to where. The owner
// of a socket is found by looking through your own programs' open files,
// only when a socket appears that was not seen before.
class NetLog {
public:
    explicit NetLog(Roots r = {}) : r_(std::move(r)) {}
    std::vector<ConnectionRecord> feed(const Snapshot& s);

private:
    Roots r_;
    double last_ = -1e9, last_full_ = -1e9;
    std::map<uint64_t, std::pair<int, std::string>> owner_;  // socket inode -> (pid, program)
    std::set<uint64_t> looked_;                              // sockets already searched for
    std::map<std::string, ConnectionRecord> pairs_;          // program|remote|port|proto -> record
};

}  // namespace as
