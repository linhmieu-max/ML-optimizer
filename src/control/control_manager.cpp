#include "control/control_manager.hpp"

#include "control/cgroup.hpp"
#include "control/cpufreq.hpp"
#include "control/gpu.hpp"
#include "control/io.hpp"
#include "control/memory.hpp"
#include "control/runtime_pm.hpp"
#include "control/uclamp.hpp"

#include <algorithm>
#include "core/logging.hpp"
#include "core/sysfs.hpp"
#include "policy/profiles.hpp"

namespace opt::control {
namespace {
constexpr std::chrono::minutes kBackoff{5};
}

std::optional<ControlsMode> parse_controls_mode(std::string_view s) {
  if (s == "off") return ControlsMode::Off;
  if (s == "dry-run") return ControlsMode::DryRun;
  if (s == "apply") return ControlsMode::Apply;
  return std::nullopt;
}

ControlManager::ControlManager(std::filesystem::path root, safety::RollbackManager* rollback, safety::StateWriter& writer,
                               const CapabilityReport& caps, const safety::HardwareLimits& limits, ControlsMode mode, bool runtime_pm)
    : root_(std::move(root)), rollback_(rollback), writer_(writer), caps_(caps), limits_(limits), mode_(mode), runtime_pm_(runtime_pm) {}

ControlsMode ControlManager::mode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mode_;
}
ControlReport ControlManager::last() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_;
}
ControlTotals ControlManager::totals() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return totals_;
}
void ControlManager::set_runtime_pm(bool on) {
  std::lock_guard<std::mutex> lock(mutex_);
  runtime_pm_ = on;
}
void ControlManager::set_mode(ControlsMode m) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (m == mode_) return;
  log::info("controllers", "mode changed: " + std::string(to_string(mode_)) + " -> " + std::string(to_string(m)));
  if (mode_ == ControlsMode::Apply && rollback_) rollback_->rollback_all("controllers mode changed to " + std::string(to_string(m)));
  mode_ = m;
}

policy::PolicyDecision ControlManager::emergency_decision() {
  policy::PolicyDecision d;
  d.profile = Profile::ThermalEmergency;
  d.source = policy::ProfileSource::Safety;
  d.targets = policy::base_targets(Profile::ThermalEmergency);
  d.safety = SafetyState::Emergency;
  d.safety_limited = true;
  return d;  // control[] defaults to Active; safety actions ignore ownership anyway
}

ControlReport ControlManager::apply(const policy::PolicyDecision& d, SafetyState safety, const monitor::SystemSnapshot* sys,
                                    Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  ControlReport rep;
  rep.mode = mode_;
  rep.cycle = ++cycle_;
  rep.at = now;
  if (mode_ == ControlsMode::Off) {
    rep.note = "controllers are off";
    return last_ = rep;
  }

  const ControlContext ctx{root_, d, caps_, limits_, sys, runtime_pm_};
  std::vector<Desired> want;
  for (auto* plan : {&plan_cpufreq, &plan_gpu, &plan_memory, &plan_io, &plan_cgroup, &plan_runtime_pm, &plan_uclamp}) {
    auto part = (*plan)(ctx);
    want.insert(want.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
  }

  want = resolve_duplicate_targets(std::move(want), &rep.actions);

  // 1. Ownership: observe-only / unavailable subsystems are not driven (safety actions excepted).
  //    `keep` is the DESIRED set: everything policy still wants to hold, whether or not a write is needed now.
  std::vector<Desired> accepted;
  std::set<std::string> keep;
  std::set<std::string> noted;
  for (auto& w : want) {
    const auto& ctl = d.of(w.subsystem);
    if (ctl.mode != policy::ControlMode::Active && !w.safety_action) {
      ++rep.skipped;
      if (noted.insert(w.controller).second)
        rep.actions.push_back({w.controller, "", "", "", "skipped: " + std::string(to_string(w.subsystem)) + " is " +
                                                            std::string(to_string(ctl.mode)) + (ctl.note.empty() ? "" : " (" + ctl.note + ")")});
      continue;
    }
    keep.insert(w.target);
    accepted.push_back(std::move(w));
  }

  // 2. Release: anything we changed earlier that is no longer wanted goes back to the original value.
  if (mode_ == ControlsMode::Apply && rollback_) {
    std::set<std::string> stale;
    for (const auto& r : rollback_->outstanding())
      if (!keep.count(r.target)) stale.insert(r.target);
    for (const auto& target : stale) {
      if (auto b = release_backoff_.find(target); b != release_backoff_.end() && now < b->second) {
        rep.actions.push_back({"", target, "", "", "revert backing off: an earlier restore failed"});
        ++rep.skipped;
        continue;
      }
      const auto r = rollback_->rollback_target(target, "no longer wanted by policy");
      rep.reverted += r.restored;
      rep.actions.push_back({"", target, "", "", r.restored ? "reverted to original" : (r.failed ? "revert FAILED" : "released (already original or changed by others)")});
      if (r.failed) {
        ++rep.failed;
        release_backoff_[target] = now + kBackoff;
      } else {
        release_backoff_.erase(target);
      }
    }
  }

  // 3. Apply through the gate.
  const safety::GateContext gate{&d, safety, &limits_};
  for (const auto& a : accepted) {
    ControlAction act{a.controller, a.target, "", a.value, ""};
    safety::ChangeIntent intent;
    intent.subsystem = a.subsystem;
    intent.target = a.target;
    intent.value = a.value;
    intent.raises_performance = a.raises_performance;
    intent.limit_kind = a.limit_kind;
    intent.numeric = a.numeric;
    intent.safety_action = a.safety_action;
    const auto verdict = safety::check_change(intent, gate);
    if (!verdict.allowed) {
      act.outcome = "blocked (" + std::string(to_string(verdict.stage)) + "): " + verdict.reason;
      ++rep.blocked;
      rep.actions.push_back(std::move(act));
      continue;
    }
    auto cur = writer_.read(a.target);
    if (!cur) {
      act.outcome = "failed: " + cur.error().message;
      ++rep.failed;
      rep.actions.push_back(std::move(act));
      continue;
    }
    act.current = *cur;
    if (*cur == a.value) {
      act.outcome = "already set";
      rep.actions.push_back(std::move(act));
      continue;
    }
    if (a.ceiling && a.numeric) {
      const auto n = sysfs::parse_u64(*cur);  // ceilings are for non-negative tunables
      if (n && *a.numeric >= 0 && *n <= static_cast<std::uint64_t>(*a.numeric)) {
        act.outcome = "already satisfied (" + *cur + " <= " + a.value + "); never raised";
        rep.actions.push_back(std::move(act));
        continue;
      }
    }
    if (!a.allow_new_write) {
      act.outcome = "held: " + a.hold_reason + "; no new change is started, existing ones are kept";
      ++rep.skipped;
      rep.actions.push_back(std::move(act));
      continue;
    }
    if (auto b = backoff_.find(a.target); b != backoff_.end()) {
      if (now < b->second) {
        act.outcome = "backing off: another tool changes this value";
        ++rep.skipped;
        rep.actions.push_back(std::move(act));
        continue;
      }
      backoff_.erase(b);
    }
    if (mode_ == ControlsMode::Apply && rollback_) {
      // Someone else changed a value we had set: do not fight them.
      const safety::ChangeRecord* mine = nullptr;
      const auto outs = rollback_->outstanding();
      for (const auto& r : outs)
        if (r.target == a.target && (!mine || r.id > mine->id)) mine = &r;
      if (mine && *cur != mine->new_value) {
        backoff_[a.target] = now + kBackoff;
        act.outcome = "changed externally (" + *cur + "); backing off 5 min";
        ++rep.skipped;
        log::warn("controllers", "another tool changed a value we set; backing off", {{"target", a.target}});
        rep.actions.push_back(std::move(act));
        continue;
      }
    }
    if (mode_ == ControlsMode::DryRun) {
      act.outcome = "would apply: " + a.reason;
      ++rep.would_apply;
      rep.actions.push_back(std::move(act));
      continue;
    }
    if (!rollback_) {
      act.outcome = "refused: rollback journal unavailable";
      ++rep.failed;
      rep.actions.push_back(std::move(act));
      continue;
    }
    if (a.safety_action) log::safety("controllers", "thermal protective action", {{"target", a.target}, {"value", a.value}});
    auto r = rollback_->apply({std::string(to_string(a.subsystem)), a.target, a.value, a.reason, std::string(to_string(d.profile))});
    if (r) {
      act.outcome = "applied: " + a.reason;
      ++rep.applied;
      log::info("controllers", "applied", {{"target", a.target}, {"from", act.current}, {"to", a.value}});
    } else {
      act.outcome = "failed: " + r.error().message;
      ++rep.failed;
      log::warn("controllers", "change failed", {{"target", a.target}, {"error", describe(r.error())}});
    }
    rep.actions.push_back(std::move(act));
  }
  totals_.applied += static_cast<std::uint64_t>(rep.applied);
  totals_.blocked += static_cast<std::uint64_t>(rep.blocked);
  totals_.failed += static_cast<std::uint64_t>(rep.failed);
  totals_.reverted += static_cast<std::uint64_t>(rep.reverted);
  return last_ = rep;
}

std::vector<Desired> resolve_duplicate_targets(std::vector<Desired> want, std::vector<ControlAction>* notes) {
  std::vector<Desired> out;
  for (auto& w : want) {
    auto it = std::find_if(out.begin(), out.end(), [&](const Desired& o) { return o.target == w.target; });
    if (it == out.end()) {
      out.push_back(std::move(w));
      continue;
    }
    if (it->value == w.value) continue;  // identical request: harmless
    const bool replace = w.safety_action && !it->safety_action;
    if (notes)
      notes->push_back({replace ? it->controller : w.controller, w.target, "", replace ? w.value : it->value,
                        "conflict: " + w.controller + " and " + it->controller + " request different values; " +
                            (replace ? w.controller + " (protective action) wins" : it->controller + " keeps the target")});
    if (replace) *it = std::move(w);
  }
  return out;
}

std::string format_controls(const ControlReport& r, const std::vector<safety::ChangeRecord>& outstanding) {
  std::string s = "Mode:        " + std::string(to_string(r.mode));
  if (r.mode == ControlsMode::DryRun) s += "  (computes and logs changes, writes NOTHING; set [controllers] mode = apply or `controllers apply`)";
  if (r.mode == ControlsMode::Off) s += "  (no controller runs)";
  s += "\n";
  s += "Last cycle:  #" + std::to_string(r.cycle) + ": applied " + std::to_string(r.applied) + ", would apply " + std::to_string(r.would_apply) +
       ", blocked " + std::to_string(r.blocked) + ", failed " + std::to_string(r.failed) + ", reverted " + std::to_string(r.reverted) +
       ", skipped " + std::to_string(r.skipped) + "\n";
  if (!r.note.empty()) s += "Note:        " + r.note + "\n";
  if (r.actions.empty() && r.cycle > 0) s += "Actions:     none proposed (BALANCED is hands-off, or no usable interface for this profile)\n";
  for (const auto& a : r.actions) {
    s += "  ";
    if (!a.controller.empty()) s += "[" + a.controller + "] ";
    if (!a.target.empty()) s += a.target + (a.current.empty() ? "" : "  " + a.current + " -> " + a.desired) + "  ";
    s += a.outcome + "\n";
  }
  s += "Outstanding changes (restored on reset, shutdown, thermal emergency or when no longer wanted): " + std::to_string(outstanding.size()) + "\n";
  for (const auto& o : outstanding) s += "  #" + std::to_string(o.id) + " " + o.target + ": " + o.old_value + " -> " + o.new_value + "  (" + o.reason + ")\n";
  return s;
}

}  // namespace opt::control
