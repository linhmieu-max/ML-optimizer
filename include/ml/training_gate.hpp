#pragma once
// Decides whether background learning may use the CPU right now (spec section 17).
//   GAME COMPILE RENDER ENCODE DESKTOP BROWSER SERVER (and every other busy workload) -> BLOCKED
//   BACKGROUND -> LIMITED (reduced duty cycle)      IDLE -> ALLOWED, but only if the machine is REALLY idle:
//   low CPU/GPU/PSI, safe temperature, enough RAM, nothing interactive, preferably on AC, and idle for a
//   sustained period (not just a short lull).

#include <chrono>
#include <optional>
#include <string>

#include "core/types.hpp"
#include "workload/types.hpp"

namespace opt::ml {

using Clock = std::chrono::steady_clock;

enum class TrainingClass { Blocked, Limited, Allowed };
constexpr std::string_view to_string(TrainingClass c) noexcept {
  return c == TrainingClass::Blocked ? "BLOCKED" : (c == TrainingClass::Limited ? "LIMITED" : "ALLOWED");
}

struct GateConfig {
  bool allow_on_battery = false;     // default: AC only
  int battery_min_pct = 80;          // when battery use is allowed
  int idle_seconds = 60;             // sustained idle required before (re)starting
  double max_cpu_pct = 15, max_gpu_pct = 15, max_psi = 5, min_thermal_margin_c = 25;
  double background_max_cpu_pct = 40;
  std::uint64_t min_avail_kb = 1024 * 1024;  // 1 GiB ...
  double min_avail_fraction = 0.20;          // ... and 20 % of RAM
};

struct GateInput {
  workload::WorkloadClass cls = workload::WorkloadClass::Desktop;
  unsigned flags = 0;
  double confidence = 0;
  std::optional<double> cpu_pct, gpu_pct, psi_cpu, psi_mem, psi_io;
  SafetyState safety = SafetyState::Normal;
  std::optional<double> thermal_margin_c;
  std::optional<std::uint64_t> mem_available_kb, mem_total_kb;
  std::optional<bool> on_ac;
  std::optional<int> battery_pct;
  bool audio_running = false;
};

struct GateDecision {
  TrainingClass klass = TrainingClass::Blocked;
  std::string reason = "no data yet";
};

class TrainingGate {
 public:
  explicit TrainingGate(GateConfig cfg = {}) : cfg_(cfg) {}
  /// Stateful only for the "idle for long enough" rule. Any failing condition blocks immediately.
  GateDecision evaluate(const GateInput&, Clock::time_point now);
  const GateConfig& config() const { return cfg_; }

 private:
  GateConfig cfg_;
  std::optional<Clock::time_point> idle_since_;
};

}  // namespace opt::ml
