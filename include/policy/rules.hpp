#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "workload/types.hpp"

namespace opt::policy {

struct RuleInput {
  workload::WorkloadClass cls = workload::WorkloadClass::Idle;
  unsigned flags = 0;
  double confidence = 0;              // classifier confidence
  std::optional<bool> on_ac;
  std::optional<int> battery_pct;
  int aggressiveness = kAggressivenessDefault;
  std::optional<double> thermal_margin_c;  // to the nearest hot/critical trip, if known
};

struct ProfileChoice {
  Profile profile = Profile::Balanced;
  std::vector<std::string> reasons;
  bool urgent = false;  // bypasses the dwell time (e.g. critical battery)
};

/// Pure function: explainable rule table for the AUTO mode.
///  1 conservative: stays near BALANCED, PERFORMANCE only with proven thermal headroom
///  2 balanced:     PERFORMANCE for game/render/encode/compile on AC
///  3 aggressive:   + idle/background -> POWERSAVE, heavy server/VM/container -> PERFORMANCE
///  4 experimental: + PERFORMANCE for loaded desktop/browser, gaming on battery (>= 40 %)
ProfileChoice choose_profile(const RuleInput&);

}  // namespace opt::policy
