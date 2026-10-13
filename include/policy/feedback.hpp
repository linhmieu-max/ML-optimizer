#pragma once
// Feedback from the monitor to the policy: bounded, explainable corrections.
// Stateful (hysteresis); not thread-safe.

#include <optional>

#include "monitor/system_monitor.hpp"

namespace opt::policy {

using monitor::Clock;

struct ThermalAdvice {
  int derate = 0;                    // 0 none, 1 = one profile step down, 2 = two steps
  std::optional<double> margin_c;    // min distance to the nearest hot/critical trip
};

struct PressureAdvice {
  bool cpu = false, memory = false, io = false;  // sustained PSI pressure (with hysteresis)
  double cpu10 = 0, mem10 = 0, io10 = 0;         // PSI "some" avg10 values
};

class Feedback {
 public:
  /// Margin < 15 C -> derate 1, < 7 C -> derate 2. Raising is immediate; lowering needs
  /// +5 C hysteresis and 15 s since the last change. Advisory only: the real thermal
  /// safety layer (Phase 5) acts independently and always wins.
  ThermalAdvice thermal(const monitor::SystemSnapshot*, Clock::time_point now);

  /// CPU: >= 30 for 3 consecutive calls (clears < 15). Memory: >= 10 (clears < 5).
  /// I/O: >= 20 (clears < 10).
  PressureAdvice pressure(const monitor::SystemSnapshot*);

 private:
  int derate_ = 0;
  Clock::time_point last_derate_change_{};
  int cpu_streak_ = 0;
  bool cpu_ = false, mem_ = false, io_ = false;
};

/// Smallest (limit - temperature) over zones that expose a hot/critical trip.
std::optional<double> thermal_margin(const monitor::ThermalSample&);

}  // namespace opt::policy
