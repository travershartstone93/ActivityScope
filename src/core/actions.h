#pragma once
#include <string>

#include "snapshot.h"

namespace as {

// Sends a signal only if the pid still belongs to the same process (same start
// time). Returns an empty string on success, or a plain reason on failure.
std::string signal_process(const ProcessKey& key, int sig);

}  // namespace as
