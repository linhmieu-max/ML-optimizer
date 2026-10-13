#pragma once
// Thermal safety state machine: NORMAL -> WARM -> HOT -> EMERGENCY with hysteresis.
// Pure logic (no I/O): feed it sensors, get a state. It is independent of the
// policy engine and the ML layer, and nothing may override it.

#include <chrono>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "safety/thermal_sensors.hpp"

namespace opt::safety {

using Clock = std::chrono::steady_clock;

struct SafetyConfig {
  // Distance below a sensor's limit at which each level is entered.
  double warm_margin_c = 15.0;
  double hot_margin_c = 8.0;
  double emergency_margin_c = 3.0;
  double exit_hysteresis_c = 5.0;   // must be this much cooler than the entry point to leave a level
  int escalate_confirmations = 2;   // consecutive readings for WARM/HOT (EMERGENCY is immediate)
  std::chrono::seconds deescalate_hold{10};
  double fallback_limit_c = 100.0;  // CPU-class sensors with no hardware/kernel limit
  double min_plausible_c = -20.0, max_plausible_c = 150.0;
  double min_plausible_limit_c = 60.0, max_plausible_limit_c = 130.0;
};

/// What a safety state forbids. Applied by the policy engine and the change gate.
struct SafetyEffects {
  bool block_performance_increase = false;
  std::optional<Profile> profile_cap;  // highest profile allowed
  bool allow_boost = true;
  bool force_emergency = false;
};
SafetyEffects effects_for(SafetyState);

struct SensorAssessment {
  std::string name, limit_source;
  double temp_c = 0, limit_c = 0;
  int severity = 0;  // 0 normal .. 3 emergency
};

struct SafetyStatus {
  SafetyState state = SafetyState::Normal;
  std::string reason = "no data yet";
  std::vector<SensorAssessment> sensors;
  bool monitoring = false;     // at least one usable sensor in the last update
  bool restored = false;       // state was restored from the previous run
  std::uint64_t transitions = 0;
  Clock::time_point since{};
};

class ThermalSafety {
 public:
  explicit ThermalSafety(SafetyConfig cfg = {}) : cfg_(cfg) { status_.since = Clock::now(); }
  SafetyState update(const std::vector<ThermalSensor>& sensors, Clock::time_point now);
  /// Start in a previously saved state; normal hysteresis decides when to leave it.
  void restore(SafetyState, Clock::time_point now);
  const SafetyStatus& status() const { return status_; }
  SafetyState state() const { return status_.state; }
  const SafetyConfig& config() const { return cfg_; }

 private:
  void transition(SafetyState to, Clock::time_point now, std::string reason);
  int severity(double temp, double limit, double extra_margin) const;

  SafetyConfig cfg_;
  SafetyStatus status_;
  bool had_sensors_ = false;
  int streak_ = 0;
  Clock::time_point justified_{};  // last time the current level was still warranted by a sensor
};

std::string format_safety(const SafetyStatus&, const SafetyConfig&, Clock::time_point now = Clock::now());

}  // namespace opt::safety
