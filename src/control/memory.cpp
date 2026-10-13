#include "control/memory.hpp"

namespace opt::control {

// Responsiveness: vm.swappiness should be at most 30, but only when swap exists. Throughput: hands off
// (dirty ratios are data-loss relevant and are deliberately not touched).
//
// This planner states INTENT only; it does not look at the current value. The ControlManager decides:
//   - current value already <= 30  -> satisfied: nothing is written, a value we own stays owned;
//   - current value > 30           -> a pending change (133 -> 30);
//   - memory pressure is high      -> an owned change is HELD (kept), but no new change is started
//                                     (reclaiming cache under pressure thrashes).
// Leaving Responsiveness, losing swap, or an observe-only/unavailable subsystem removes the target from
// the desired set, which is the one place a rollback is triggered.
std::vector<Desired> plan_memory(const ControlContext& c) {
  std::vector<Desired> out;
  const auto& d = c.decision;
  if (d.targets.memory != policy::MemoryBias::Responsiveness) return out;
  if (!c.caps.available("mem.swap")) return out;
  Desired x;
  x.controller = "memory";
  x.subsystem = policy::Subsystem::Memory;
  x.target = "/proc/sys/vm/swappiness";
  x.value = "30";
  x.numeric = 30;
  x.ceiling = true;
  x.allow_new_write = !d.pressure.memory;
  x.hold_reason = "memory pressure is high";
  x.reason = "favour responsiveness: swappiness at most 30";
  out.push_back(std::move(x));
  return out;
}

}  // namespace opt::control
