#include "ml/training_gate.hpp"

#include <cstdio>

namespace opt::ml {
using workload::WorkloadClass;

GateDecision TrainingGate::evaluate(const GateInput& in, Clock::time_point now) {
  auto block = [&](std::string why) {
    idle_since_.reset();
    return GateDecision{TrainingClass::Blocked, std::move(why)};
  };
  char b[160];

  if (in.safety != SafetyState::Normal) return block("thermal state is " + std::string(to_string(in.safety)));
  if (in.confidence < 0.3) return block("workload not classified yet");

  const bool background = in.cls == WorkloadClass::Background;
  if (in.cls != WorkloadClass::Idle && !background)
    return block("workload " + std::string(to_string(in.cls)) + " is active");
  if ((in.flags & workload::kLatencySensitive) || in.audio_running) return block("interactive/latency-sensitive activity");

  const double cpu_limit = background ? cfg_.background_max_cpu_pct : cfg_.max_cpu_pct;
  if (in.cpu_pct && *in.cpu_pct > cpu_limit) {
    std::snprintf(b, sizeof b, "CPU %.0f%% above %.0f%%", *in.cpu_pct, cpu_limit);
    return block(b);
  }
  if (in.gpu_pct && *in.gpu_pct > cfg_.max_gpu_pct) return block("GPU busy");
  for (const auto& p : {in.psi_cpu, in.psi_mem, in.psi_io})
    if (p && *p > cfg_.max_psi) return block("resource pressure (PSI)");
  if (in.thermal_margin_c && *in.thermal_margin_c < cfg_.min_thermal_margin_c) {
    std::snprintf(b, sizeof b, "thermal margin %.0f C below %.0f C", *in.thermal_margin_c, cfg_.min_thermal_margin_c);
    return block(b);
  }
  if (in.mem_available_kb && in.mem_total_kb && *in.mem_total_kb > 0) {
    const bool enough = *in.mem_available_kb >= cfg_.min_avail_kb &&
                        static_cast<double>(*in.mem_available_kb) >= cfg_.min_avail_fraction * static_cast<double>(*in.mem_total_kb);
    if (!enough) return block("not enough free memory");
  }
  if (in.on_ac == false) {
    if (!cfg_.allow_on_battery) return block("on battery (AC power preferred)");
    if (in.battery_pct.value_or(0) < cfg_.battery_min_pct) return block("battery below the training minimum");
  }

  if (!idle_since_) idle_since_ = now;
  const auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - *idle_since_).count();
  if (idle < cfg_.idle_seconds) {
    std::snprintf(b, sizeof b, "waiting for sustained idle (%lld of %d s)", static_cast<long long>(idle), cfg_.idle_seconds);
    return {TrainingClass::Blocked, b};
  }
  return background ? GateDecision{TrainingClass::Limited, "background-only activity: reduced duty cycle"}
                    : GateDecision{TrainingClass::Allowed, "machine is idle"};
}

}  // namespace opt::ml
