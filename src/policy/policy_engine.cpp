#include "policy/policy_engine.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "core/logging.hpp"
#include "policy/profiles.hpp"
#include "safety/thermal_safety.hpp"

namespace opt::policy {
namespace {

constexpr std::chrono::seconds kBaseDwell{20};
constexpr std::chrono::seconds kMaxDwell{300};
constexpr std::chrono::seconds kFlapWindow{300};

std::string profile_name(Profile p) { return std::string(to_string(p)); }

void compute_control(const PolicyInput& in, workload::WorkloadClass cls, PolicyDecision& d) {
  auto set = [&](Subsystem s, ControlMode m, std::string note) {
    d.control[static_cast<std::size_t>(s)] = {m, std::move(note)};
  };
  const CapabilityReport* caps = in.caps;
  auto has = [&](std::string_view id) { return !caps || caps->available(id); };  // no report: assume yes

  set(Subsystem::Cpu, has("cpufreq") ? ControlMode::Active : ControlMode::Unavailable, has("cpufreq") ? "" : "no cpufreq interface");
  set(Subsystem::Gpu, (has("gpu.devfreq") || has("gpu.drm")) ? ControlMode::Active : ControlMode::Unavailable,
      (has("gpu.devfreq") || has("gpu.drm")) ? "" : "no GPU frequency interface");
  set(Subsystem::Memory, ControlMode::Active, "sysctl changes need validation + rollback (Phase 5/6)");
  set(Subsystem::Io, has("io.scheduler") ? ControlMode::Active : ControlMode::Unavailable, has("io.scheduler") ? "" : "no block queue/scheduler");
  set(Subsystem::Background, has("cgroup.v2") ? ControlMode::Active : ControlMode::Unavailable, has("cgroup.v2") ? "" : "cgroup v2 not mounted");
  set(Subsystem::RuntimePm, ControlMode::Active, "validated per device in Phase 6");

  if (!in.tools) return;
  for (const auto& t : *in.tools) {
    if (t.game_scoped && cls != workload::WorkloadClass::Game) continue;  // GameMode only matters during a game
    for (Subsystem s : t.subsystems) {
      auto& c = d.control[static_cast<std::size_t>(s)];
      if (c.mode == ControlMode::Unavailable) continue;
      c.mode = ControlMode::Observe;
      c.note = t.name + (t.game_scoped ? " is active (not fighting it)" : " owns this subsystem (not contested)");
    }
  }
}

}  // namespace

std::chrono::seconds PolicyEngine::dwell(Clock::time_point now) {
  while (!changes_.empty() && now - changes_.front() > kFlapWindow) changes_.pop_front();
  const auto n = static_cast<int>(changes_.size());
  return std::min(kMaxDwell, kBaseDwell * (1 + n / 2));  // more recent flapping -> longer hold
}

const PolicyDecision& PolicyEngine::evaluate(const PolicyInput& in) {
  const auto now = in.now;
  PolicyDecision d;
  d.sequence = ++seq_;
  d.aggressiveness = in.aggressiveness;

  const workload::WorkloadResult* wl = in.workload;
  const bool wl_known = wl && wl->confidence >= 0.3;
  const auto cls = wl ? wl->cls : workload::WorkloadClass::Idle;
  const unsigned flags = wl ? wl->flags : 0;

  Profile want = Profile::Balanced;
  ProfileSource source = ProfileSource::Auto;
  bool urgent = false;
  const bool user_changed = initialized_ && !(in.user == last_user_);

  const ThermalAdvice thermal = feedback_.thermal(in.sys, now);
  const PressureAdvice pressure = feedback_.pressure(in.sys);
  d.thermal_margin_c = thermal.margin_c;
  d.pressure = pressure;

  std::optional<bool> on_ac;
  std::optional<int> battery;
  if (in.sys && in.sys->power) {
    on_ac = in.sys->power->on_ac;
    battery = in.sys->power->battery_pct;
  }

  if (in.safety == SafetyState::Emergency) {
    want = Profile::ThermalEmergency;
    source = ProfileSource::Safety;
    urgent = true;
    d.reasons.push_back("thermal safety EMERGENCY overrides every profile and lock");
  } else if (in.user.locked) {
    want = in.user.profile;
    source = ProfileSource::UserLocked;
    d.reasons.push_back("profile locked by user: no adaptation");
  } else {
    if (in.user.manual) {
      want = in.user.profile;
      source = ProfileSource::User;
      d.reasons.push_back("profile chosen by user (targets still adapt to the workload)");
    } else {
      RuleInput r;
      r.cls = cls;
      r.flags = flags;
      r.confidence = wl ? wl->confidence : 0.0;
      r.on_ac = on_ac;
      r.battery_pct = battery;
      r.aggressiveness = in.aggressiveness;
      r.thermal_margin_c = thermal.margin_c;
      ProfileChoice c = choose_profile(r);
      want = c.profile;
      urgent = c.urgent;
      d.reasons.insert(d.reasons.end(), c.reasons.begin(), c.reasons.end());
      d.rule_profile = c.profile;  // kept for the ML agreement statistics
      d.rule_valid = true;

      // ML suggestion: strictly validated, one step at most, never under any thermal restriction.
      if (in.ml && in.ml->available) {
        auto rank = [](Profile p) { return static_cast<int>(p); };
        MlOutcome o{in.ml->suggested, in.ml->confidence, false, ""};
        const bool margin_ok = thermal.margin_c && *thermal.margin_c >= 20.0;
        char conf[48];
        std::snprintf(conf, sizeof conf, "%.2f below the %.2f minimum", o.confidence, in.ml_min_confidence);
        if (!in.ml_suggest) o.why = "observe mode: recorded, not applied";
        else if (in.safety != SafetyState::Normal) o.why = "thermal safety state is not NORMAL";
        else if (thermal.derate > 0) o.why = "thermal derating is active";
        else if (c.urgent) o.why = "an urgent rule applies (e.g. critical battery)";
        else if (in.aggressiveness < 2) o.why = "conservative aggressiveness level";
        else if (o.confidence < in.ml_min_confidence) o.why = std::string("effective confidence ") + conf;
        else if (o.suggested == want) o.why = "agrees with the rules";
        else if (std::abs(rank(o.suggested) - rank(want)) != 1) o.why = "differs from the rules by more than one step";
        else if (rank(o.suggested) > rank(want) && !(on_ac == true && margin_ok)) o.why = "more performance only on AC with proven thermal headroom (>= 20 C)";
        else if (rank(o.suggested) < rank(want) && (flags & workload::kLatencySensitive)) o.why = "latency-sensitive workload: no further reduction";
        else {
          o.used = true;
          o.why = "accepted";
          d.reasons.push_back("[ML] suggestion " + profile_name(o.suggested) + " accepted (" + in.ml->note + "), rules said " + profile_name(want));
          want = o.suggested;
        }
        if (!o.used && o.why != "agrees with the rules" && in.ml_suggest) d.reasons.push_back("[ML] suggestion " + profile_name(o.suggested) + " ignored: " + o.why);
        d.ml = std::move(o);
      }
      // Feedback: sustained CPU pressure on AC lifts BALANCED to PERFORMANCE.
      if (pressure.cpu && want == Profile::Balanced && on_ac == true && in.aggressiveness >= 3 && thermal.derate == 0) {
        want = Profile::Performance;
        d.reasons.push_back("sustained CPU pressure (PSI " + std::to_string(static_cast<int>(pressure.cpu10)) + ") on AC: raise to PERFORMANCE");
      }
    }
    if (thermal.derate > 0) {
      const Profile lowered = step_down(want, thermal.derate);
      if (lowered != want) {
        d.reasons.push_back("thermal margin " + std::to_string(static_cast<int>(thermal.margin_c.value_or(0))) + " C: derate " +
                            std::to_string(thermal.derate) + " step(s) to " + profile_name(lowered));
        want = lowered;
        urgent = true;  // protecting hardware is never delayed by dwell
      }
    }
  }
  d.thermal_derate = thermal.derate;
  d.safety = in.safety;
  if (in.ml && in.ml->available && !d.ml)
    d.ml = MlOutcome{in.ml->suggested, in.ml->confidence, false,
                     source == ProfileSource::Auto ? "not evaluated" : (source == ProfileSource::Safety ? "thermal safety decides" : "user profile (manual/locked): ML is never applied")};

  // Thermal safety has the last word, over every mode (auto, manual and locked) and level.
  const safety::SafetyEffects eff = safety::effects_for(in.safety);
  if (!eff.force_emergency) {
    auto rank = [](Profile p) { return static_cast<int>(p); };
    if (eff.profile_cap && rank(want) > rank(*eff.profile_cap)) {
      d.reasons.push_back("[SAFETY] thermal " + std::string(to_string(in.safety)) + ": profile capped at " + profile_name(*eff.profile_cap));
      want = *eff.profile_cap;
      urgent = true;
      d.safety_limited = true;
    }
    if (eff.block_performance_increase) {
      const Profile ceiling = initialized_ ? current_ : Profile::Balanced;
      if (rank(want) > rank(ceiling)) {
        d.reasons.push_back("[SAFETY] performance increase blocked (thermal " + std::string(to_string(in.safety)) + ")");
        want = ceiling;
        d.safety_limited = true;
      }
    }
  } else {
    d.safety_limited = true;
  }

  // Stability: hold the current profile unless the change is urgent, user-initiated or safety-driven.
  if (!initialized_) {
    current_ = want;
    last_change_ = now;
  } else if (want != current_) {
    const bool immediate = urgent || user_changed || source != ProfileSource::Auto;
    if (immediate || now - last_change_ >= dwell(now)) {
      log::info("policy", "Policy changed: " + profile_name(current_) + " -> " + profile_name(want),
                {{"source", std::string(to_string(source))}});
      current_ = want;
      last_change_ = now;
      changes_.push_back(now);
    } else {
      d.held = true;
      d.reasons.push_back("holding " + profile_name(current_) + " for stability (proposed " + profile_name(want) + ", dwell " +
                          std::to_string(dwell(now).count()) + " s)");
    }
  }
  initialized_ = true;
  last_user_ = in.user;
  d.profile = current_;
  d.source = source;
  d.since = last_change_;

  // Targets.
  d.targets = base_targets(current_);
  const bool adapt = source != ProfileSource::UserLocked && source != ProfileSource::Safety;
  if (adapt) {
    if (wl_known) tune_for_workload(d.targets, cls, flags, in.aggressiveness);
    if (pressure.memory) {
      d.targets.background = Restriction::Strong;
      d.targets.memory = MemoryBias::Responsiveness;
      d.reasons.push_back("memory pressure (PSI " + std::to_string(static_cast<int>(pressure.mem10)) + "): restrict background, favour responsiveness");
    }
    if (pressure.io && cls != workload::WorkloadClass::FileCopy && cls != workload::WorkloadClass::Archive) {
      d.targets.io = IoBias::Latency;
      if (d.targets.background == Restriction::None) d.targets.background = Restriction::Light;
      d.reasons.push_back("I/O pressure (PSI " + std::to_string(static_cast<int>(pressure.io10)) + "): favour I/O latency");
    }
    if (in.aggressiveness <= 2 && d.targets.background == Restriction::Strong) d.targets.background = Restriction::Light;
  }

  if (!eff.allow_boost) d.targets.allow_boost = false;
  compute_control(in, cls, d);
  decision_ = std::move(d);
  return decision_;
}

std::string format_user_mode(const UserMode& m) {
  if (!m.manual) return "auto";
  return std::string(user_name(m.profile)) + (m.locked ? " (locked)" : " (manual)");
}

std::string format_decision(const PolicyDecision& d, Clock::time_point now) {
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - d.since).count();
  std::string s = "Profile:      " + profile_name(d.profile) + "  (source: " + std::string(to_string(d.source)) + ", for " +
                  std::to_string(secs) + "s" + (d.held ? ", change held for stability" : "") + ")\n";
  s += "Aggressive:   " + std::to_string(d.aggressiveness) + " (" + std::string(aggressiveness_name(d.aggressiveness)) + ")\n";
  s += "Targets:      cpu=" + std::string(to_string(d.targets.cpu)) + " boost=" + (d.targets.allow_boost ? "yes" : "no") +
       " gpu=" + std::string(to_string(d.targets.gpu)) + " io=" + std::string(to_string(d.targets.io)) +
       " background=" + std::string(to_string(d.targets.background)) + " memory=" + std::string(to_string(d.targets.memory)) +
       " runtime-pm=" + std::string(to_string(d.targets.runtime_pm)) + "\n";
  s += "Control:\n";
  for (std::size_t i = 0; i < kSubsystemCount; ++i) {
    const auto& c = d.control[i];
    s += "  " + std::string(to_string(static_cast<Subsystem>(i)));
    s.append(12 - std::min<std::size_t>(11, to_string(static_cast<Subsystem>(i)).size()), ' ');
    s += std::string(to_string(c.mode));
    if (!c.note.empty()) s += "  - " + c.note;
    s += "\n";
  }
  if (d.safety != SafetyState::Normal)
    s += "Safety:       thermal " + std::string(to_string(d.safety)) + (d.safety_limited ? " (limiting this decision)" : "") + "\n";
  if (d.thermal_margin_c || d.thermal_derate)
    s += "Thermal:      margin " + (d.thermal_margin_c ? std::to_string(static_cast<int>(*d.thermal_margin_c)) + " C" : std::string("n/a")) +
         ", derate " + std::to_string(d.thermal_derate) + "\n";
  s += "Pressure:     cpu " + std::string(d.pressure.cpu ? "HIGH" : "ok") + ", memory " + (d.pressure.memory ? "HIGH" : "ok") + ", io " +
       (d.pressure.io ? "HIGH" : "ok") + "\n";
  if (d.ml) {
    char b[160];
    std::snprintf(b, sizeof b, "ML advice:    %s (effective confidence %.2f): %s\n", std::string(to_string(d.ml->suggested)).c_str(), d.ml->confidence,
                  d.ml->used ? "USED" : ("not used - " + d.ml->why).c_str());
    s += b;
  }
  s += "Because:\n";
  for (const auto& r : d.reasons) s += "  - " + r + "\n";
  s += "(targets are applied by the controllers according to [controllers] mode, default dry-run: see `ml-optimizer controllers`)\n";
  return s;
}

}  // namespace opt::policy
