#pragma once
#include <string>

#include "snapshot.h"

namespace as {

// Writes flat "key value" lines for conky into $XDG_RUNTIME_DIR, which is in
// RAM, replacing the file atomically so conky never reads half of it.
void write_conky_feed(const Snapshot& s, const std::string& dir);
std::string runtime_dir();

}  // namespace as
