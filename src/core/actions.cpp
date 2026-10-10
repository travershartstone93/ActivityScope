#include "actions.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>

#include "fs.h"

namespace as {

std::string signal_process(const ProcessKey& key, int sig) {
    int fd = int(syscall(SYS_pidfd_open, key.pid, 0));
    if (fd < 0) return errno == ESRCH ? "It has already ended." : std::string("Could not open it: ") + std::strerror(errno);
    // With the pidfd held, the pid cannot be reused, so this check is final.
    auto stat = read_file("/proc/" + std::to_string(key.pid) + "/stat", 4096);
    bool same = false;
    if (stat) {
        auto close = stat->rfind(')');
        auto f = split_ws(std::string_view(*stat).substr(close + 2));
        same = f.size() > 19 && to_uint(f[19]).value_or(0) == key.start_ticks;
    }
    if (!same) {
        ::close(fd);
        return "It has already ended; another program now has that PID.";
    }
    int rc = int(syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0));
    int err = errno;
    ::close(fd);
    if (rc == 0) return {};
    if (err == EPERM) return "It belongs to another user. Ending it would need administrator rights, which ActivityScope never uses.";
    return std::string("The system refused: ") + std::strerror(err);
}

}  // namespace as
