#include "telemetry/reward.hpp"

#include <algorithm>

namespace opt::telemetry {
namespace {
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
}  // namespace

RewardBreakdown compute_reward(const RewardInput& in) {
  RewardBreakdown r;
  double num = 0, den = 0;
  int known = 0;
  auto gain = [&](const std::optional<double>& v, double w) { if (v) { num += w * *v; den += w; ++known; } };
  auto cost = [&](const std::optional<double>& v, double w) { if (v) { num -= w * *v; den += w; ++known; } };

  // Performance delivered: an idle machine has nothing to deliver (neutral 1); a busy one is
  // judged by how much of its maximum frequency it can use.
  if (in.cpu_util_pct && in.cpu_freq_mhz && in.cpu_max_mhz && *in.cpu_max_mhz > 0) {
    const double busy = clamp01(*in.cpu_util_pct / 60.0);
    r.performance = clamp01((1.0 - busy) + busy * (*in.cpu_freq_mhz / *in.cpu_max_mhz));
  }
  // Responsiveness: stall pressure (PSI) on CPU, I/O and memory.
  if (in.psi_cpu10 || in.psi_io10 || in.psi_mem10) {
    const double stall = std::max({in.psi_cpu10.value_or(0) / 40.0, in.psi_io10.value_or(0) / 40.0, in.psi_mem10.value_or(0) / 20.0});
    r.responsiveness = 1.0 - clamp01(stall);
  }
  if (in.power_w) r.power_cost = clamp01(*in.power_w / 28.0);                         // 28 W ~ a mobile CPU's sustained limit
  if (in.thermal_margin_c) r.thermal_cost = clamp01((30.0 - *in.thermal_margin_c) / 30.0);  // costs start 30 C below the limit
  r.instability = clamp01(in.profile_changes / 2.0 + (in.safety_event ? 1.0 : 0.0));

  gain(r.performance, 0.35);
  gain(r.responsiveness, 0.30);
  cost(r.power_cost, 0.15);
  cost(r.thermal_cost, 0.15);
  num -= 0.05 * r.instability;
  den += 0.05;
  // With nothing measurable, only the (small) instability penalty remains; never blow it up by renormalising.
  r.total = known > 0 ? num / den : num;
  return r;
}

}  // namespace opt::telemetry
