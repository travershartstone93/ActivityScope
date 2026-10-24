#pragma once
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "snapshot.h"

namespace as {

// One line in the investigation log.
struct LogEntry {
    double t = 0;          // seconds since 1970
    std::string category;  // devices, storage, flashing, software, admin, session, network, services, problems, programs
    std::string title;     // one plain sentence
    std::string detail;    // raw evidence: log lines, ids, command lines
};

// A system-log record, as read from the journal.
struct JournalLine {
    double t = 0;
    std::string identifier;  // SYSLOG_IDENTIFIER
    std::string unit;        // _SYSTEMD_UNIT
    std::string message;
    bool kernel = false;
};

// Turns the system-log lines worth keeping into log entries.
std::optional<LogEntry> classify_journal(const JournalLine& line);

// Groups pacman.log lines into one entry per transaction.
class PacmanParser {
public:
    // Feed one line; returns an entry when a transaction completes.
    std::optional<LogEntry> feed(const std::string& line);
    // No transaction open and no command waiting for one: a read may stop here and lose nothing.
    bool idle() const { return !open_ && command_.empty(); }

private:
    double t_ = 0;
    std::string command_;
    std::vector<std::string> installed_, upgraded_, removed_, downgraded_;
    bool open_ = false;
};
double parse_pacman_time(const std::string& line);

// pacman.log read from the byte offset the stream stands at: the transactions completed in what was read, and in
// resume the offset the next read should begin at.
std::vector<LogEntry> read_pacman(std::istream& in, long long offset, long long* resume);

// Flashing, imaging and disk tools worth logging when they run.
struct ToolUse {
    std::string category;  // flashing or storage
    std::string what;      // "Flashed a board", "Formatted a drive"...
    std::string port;      // serial port or device, if given
    std::string image;     // firmware or image file, if given
};
std::optional<ToolUse> recognise_tool(const std::string& name, const std::string& cmdline);

// Folds repeats: the same entry again within a minute is counted, not
// listed, and becomes one "N times between A and B" line when it stops.
// Two reports of one crash (kernel and crash reporter) count once.
class Repeats {
public:
    std::vector<LogEntry> feed(std::vector<LogEntry> in, double now);

private:
    struct Run { LogEntry first; double last = 0; int count = 0; };
    std::vector<Run> runs_;
    struct Crash { std::string program; double t; bool kernel, paired; };
    std::vector<Crash> crashes_;
};

// A friendly name for USB development boards and serial adapters.
std::string board_name(const std::string& vid, const std::string& pid);

}  // namespace as
