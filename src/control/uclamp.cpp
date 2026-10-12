#include "control/uclamp.hpp"

namespace opt::control {

// uclamp is a per-task attribute (sched_setattr). Without a reliable foreground-task identity
// (root daemon, Wayland) any clamp would be a guess, so this backend deliberately proposes nothing.
// On kernels without CONFIG_UCLAMP_TASK (e.g. this Debian kernel) it is reported unsupported.
std::vector<Desired> plan_uclamp(const ControlContext&) { return {}; }

}  // namespace opt::control
