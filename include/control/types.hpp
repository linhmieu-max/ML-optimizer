#pragma once
// Controllers never write anything themselves. Each one turns the policy targets
// into a list of DESIRED changes; the ControlManager runs every change through the
// safety gate and the rollback journal.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/capabilities.hpp"
#include "monitor/system_monitor.hpp"
#include "policy/policy_engine.hpp"
#include "safety/hardware_limits.hpp"

namespace opt::control {

struct Desired {
  std::string controller;                    // "cpufreq", "gpu", ...
  policy::Subsystem subsystem = policy::Subsystem::Cpu;
  std::string target;                        // logical absolute path, e.g. /sys/devices/...
  std::string value;
  bool raises_performance = false;
  std::optional<safety::LimitKind> limit_kind;
  std::optional<long long> numeric;
  bool safety_action = false;                // protective thermal throttle
  // `value` is an upper bound: a current numeric value <= `numeric` already satisfies the intent and is
  // never written (so a lower value is never raised). Requires `numeric`.
  bool ceiling = false;
  // false = the policy still WANTS this target (an owned change is kept, never reverted) but no NEW change
  // may be started right now (e.g. memory pressure). `hold_reason` explains it in the report.
  bool allow_new_write = true;
  std::string hold_reason;
  std::string reason;
};

// A planner states WHAT the policy wants (the desired set). It must not drop a target just because the
// current value already satisfies it: whether a write is needed is decided by the ControlManager
// ("pending changes"). Dropping a satisfied target would make the manager think policy let go of it and
// roll it back, which then makes the planner propose it again (apply/rollback loop).

struct ControlContext {
  std::filesystem::path root;                // "/" (a fake tree in tests)
  const policy::PolicyDecision& decision;
  const CapabilityReport& caps;
  const safety::HardwareLimits& limits;
  const monitor::SystemSnapshot* sys = nullptr;
  bool runtime_pm_enabled = false;           // opt-in
};

/// Profile BALANCED means "hands off": a controller proposes nothing and the manager
/// restores whatever it changed before. Only POWERSAVE / PERFORMANCE / emergency act.
}  // namespace opt::control
