#include "control/cgroup.hpp"

#include "core/sysfs.hpp"

namespace opt::control {
using namespace sysfs;
using policy::Restriction;

// Background restriction = lower CPU weight of system.slice relative to the default 100 of
// user.slice. Weights only matter under contention; nothing is stopped, disabled or capped.
// (io.weight is not touched: it needs bfq/io.cost to have any effect.)
// Note: systemd owns this tree; if it resets the value we back off rather than fight it.
std::vector<Desired> plan_cgroup(const ControlContext& c) {
  std::vector<Desired> out;
  const Restriction r = c.decision.targets.background;
  if (r == Restriction::None || !c.caps.available("cgroup.v2")) return out;
  if (!path_exists(c.root / "sys/fs/cgroup/system.slice/cpu.weight")) return out;  // cpu controller not enabled there
  Desired d;
  d.controller = "cgroup";
  d.subsystem = policy::Subsystem::Background;
  d.target = "/sys/fs/cgroup/system.slice/cpu.weight";
  d.value = r == Restriction::Light ? "50" : "20";
  d.reason = std::string("background restriction ") + (r == Restriction::Light ? "light" : "strong") + ": system.slice cpu.weight";
  out.push_back(std::move(d));
  return out;
}

}  // namespace opt::control
