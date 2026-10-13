#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include "core/capabilities.hpp"
#include "core/types.hpp"
#include "ml/advice.hpp"
#include "monitor/system_monitor.hpp"
#include "policy/feedback.hpp"
#include "policy/rules.hpp"
#include "policy/targets.hpp"
#include "safety/conflict_detector.hpp"
#include "workload/types.hpp"

namespace opt::policy {

/// What the user asked for. `locked` implies `manual`.
struct UserMode {
  bool manual = false;  // false = AUTO (adaptive)
  Profile profile = Profile::Balanced;
  bool locked = false;  // locked: no adaptation of profile *or* targets (only safety may override)
  bool operator==(const UserMode&) const = default;
};

enum class ProfileSource { Auto, User, UserLocked, Safety };
constexpr std::string_view to_string(ProfileSource s) noexcept {
  switch (s) {
    case ProfileSource::Auto: return "auto";
    case ProfileSource::User: return "user";
    case ProfileSource::UserLocked: return "user (locked)";
    case ProfileSource::Safety: return "thermal safety";
  }
  return "?";
}

struct PolicyInput {
  UserMode user;
  int aggressiveness = kAggressivenessDefault;
  SafetyState safety = SafetyState::Normal;   // from the thermal safety layer (Phase 5)
  const workload::WorkloadResult* workload = nullptr;
  const monitor::SystemSnapshot* sys = nullptr;
  const CapabilityReport* caps = nullptr;
  const std::vector<safety::ExternalTool>* tools = nullptr;
  // ML layer (optional): advice is validated here; it can never bypass safety, locks, limits or the gate.
  const ml::MlAdvice* ml = nullptr;
  bool ml_suggest = false;          // false = observe only
  double ml_min_confidence = 0.75;
  Clock::time_point now = Clock::now();
};

struct MlOutcome {
  Profile suggested = Profile::Balanced;
  double confidence = 0;
  bool used = false;
  std::string why;  // why it was (not) used
};

struct PolicyDecision {
  std::uint64_t sequence = 0;
  Profile profile = Profile::Balanced;
  ProfileSource source = ProfileSource::Auto;
  Targets targets;
  std::array<SubsystemControl, kSubsystemCount> control;
  std::vector<std::string> reasons;
  Profile rule_profile = Profile::Balanced;  // what the rule table alone chose (AUTO only)
  bool rule_valid = false;
  std::optional<MlOutcome> ml;
  bool held = false;            // a different profile was proposed but held back for stability
  int thermal_derate = 0;
  SafetyState safety = SafetyState::Normal;
  bool safety_limited = false;  // thermal safety lowered/held the profile
  PressureAdvice pressure;
  std::optional<double> thermal_margin_c;
  Clock::time_point since{};    // when `profile` became current
  int aggressiveness = kAggressivenessDefault;
  const SubsystemControl& of(Subsystem s) const { return control[static_cast<std::size_t>(s)]; }
};

/// Rule-based adaptive policy with feedback. Advisory: produces a decision, never acts.
/// Not thread-safe (single caller: the scheduler thread).
class PolicyEngine {
 public:
  const PolicyDecision& evaluate(const PolicyInput&);
  const PolicyDecision& current() const { return decision_; }

 private:
  std::chrono::seconds dwell(Clock::time_point now);

  Feedback feedback_;
  PolicyDecision decision_;
  bool initialized_ = false;
  UserMode last_user_;
  Profile current_ = Profile::Balanced;
  Clock::time_point last_change_{};
  std::deque<Clock::time_point> changes_;  // for flap damping
  std::uint64_t seq_ = 0;
};

std::string format_decision(const PolicyDecision&, Clock::time_point now = Clock::now());
std::string format_user_mode(const UserMode&);

}  // namespace opt::policy
