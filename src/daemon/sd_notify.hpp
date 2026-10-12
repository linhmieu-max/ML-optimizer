#pragma once

#include <cstdint>
#include <string_view>

namespace opt::sd {

/// Minimal sd_notify(3): sends `state` to $NOTIFY_SOCKET. No libsystemd needed.
/// Returns false when not running under systemd or when sending fails.
bool notify(std::string_view state);

/// Watchdog interval in microseconds from $WATCHDOG_USEC, or 0 if disabled.
std::uint64_t watchdog_usec();

}  // namespace opt::sd
