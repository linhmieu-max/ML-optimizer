#pragma once
// Multi-objective reward (spec section 20). All components are normalised to [0, 1] BEFORE they are
// combined, so no single metric can dominate or be traded away silently:
//   reward = ( 0.35 performance + 0.30 responsiveness - 0.15 power - 0.15 thermal - 0.05 instability )
//            / (sum of the weights of the components that are actually known)        -> about [-0.35, 0.65]
//                                                                                      when everything is known
// Unknown inputs drop out (and the weights are renormalised) instead of being guessed.

#include <optional>

namespace opt::telemetry {

struct RewardInput {
  std::optional<double> cpu_util_pct, cpu_freq_mhz, cpu_max_mhz;
  std::optional<double> psi_cpu10, psi_io10, psi_mem10;  // PSI "some" avg10
  std::optional<double> power_w;
  std::optional<double> thermal_margin_c;                // distance to the nearest hot/critical limit
  int profile_changes = 0;                               // since the previous sample
  bool safety_event = false;                             // thermal state != NORMAL, blocked/failed change
};

struct RewardBreakdown {
  std::optional<double> performance, responsiveness, power_cost, thermal_cost;
  double instability = 0;
  double total = 0;
};

RewardBreakdown compute_reward(const RewardInput&);

}  // namespace opt::telemetry
