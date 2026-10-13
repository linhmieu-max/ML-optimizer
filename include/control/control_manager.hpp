#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "control/types.hpp"
#include "safety/rollback.hpp"
#include "safety/safety_gate.hpp"

namespace opt::control {

using monitor::Clock;

enum class ControlsMode { Off, DryRun, Apply };
constexpr std::string_view to_string(ControlsMode m) noexcept {
  return m == ControlsMode::Off ? "off" : m == ControlsMode::DryRun ? "dry-run" : "apply";
}
std::optional<ControlsMode> parse_controls_mode(std::string_view);

struct ControlAction {
  std::string controller, target, current, desired, outcome;
};

struct ControlTotals {
  std::uint64_t applied = 0, blocked = 0, failed = 0, reverted = 0;  // cumulative since start
};

struct ControlReport {
  ControlsMode mode = ControlsMode::Off;
  std::uint64_t cycle = 0;
  Clock::time_point at{};
  std::vector<ControlAction> actions;
  int applied = 0, would_apply = 0, blocked = 0, failed = 0, reverted = 0, skipped = 0;
  std::string note;
};

/// Turns a PolicyDecision into real (or, in dry-run, hypothetical) changes:
///   desired changes (controllers) -> ownership filter -> release what is no longer wanted ->
///   gate (allowlist, policy, capability, conflict, thermal, limits) -> journal -> write.
/// Thread-safe: the scheduler thread and the thermal guard thread may both call apply().
class ControlManager {
 public:
  ControlManager(std::filesystem::path root, safety::RollbackManager* rollback, safety::StateWriter& writer,
                 const CapabilityReport& caps, const safety::HardwareLimits& limits, ControlsMode mode, bool runtime_pm);

  ControlReport apply(const policy::PolicyDecision&, SafetyState, const monitor::SystemSnapshot* sys = nullptr,
                      Clock::time_point now = Clock::now());

  /// Leaving APPLY restores everything that was changed.
  void set_mode(ControlsMode);
  void set_runtime_pm(bool);
  ControlsMode mode() const;
  ControlReport last() const;
  ControlTotals totals() const;

  /// Decision used by the thermal guard to throttle immediately, without waiting for the scheduler.
  static policy::PolicyDecision emergency_decision();

 private:
  std::filesystem::path root_;
  safety::RollbackManager* rollback_;
  safety::StateWriter& writer_;
  const CapabilityReport& caps_;
  const safety::HardwareLimits& limits_;
  mutable std::mutex mutex_;
  ControlsMode mode_;
  bool runtime_pm_;
  std::uint64_t cycle_ = 0;
  std::map<std::string, Clock::time_point> backoff_;          // targets someone else keeps changing
  std::map<std::string, Clock::time_point> release_backoff_;  // restores that failed: retried later, not every cycle
  ControlReport last_;
  ControlTotals totals_;
};

/// Several controllers may name the same target. One value must win, deterministically: a protective
/// (safety) action beats everything, otherwise the first request wins. Identical duplicates merge silently;
/// conflicting ones are reported in `notes`.
std::vector<Desired> resolve_duplicate_targets(std::vector<Desired> want, std::vector<ControlAction>* notes);

std::string format_controls(const ControlReport&, const std::vector<safety::ChangeRecord>& outstanding);

}  // namespace opt::control
