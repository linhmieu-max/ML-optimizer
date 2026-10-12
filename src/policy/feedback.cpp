#include "policy/feedback.hpp"

#include <algorithm>

namespace opt::policy {

std::optional<double> thermal_margin(const monitor::ThermalSample& t) {
  std::optional<double> best;
  for (const auto& z : t.zones) {
    if (!z.temp_c) continue;
    std::optional<double> limit;
    for (const auto& trip : z.trips)
      if ((trip.type == "hot" || trip.type == "critical") && (!limit || trip.temp_c < *limit)) limit = trip.temp_c;
    if (!limit) continue;
    const double margin = *limit - *z.temp_c;
    if (!best || margin < *best) best = margin;
  }
  return best;
}

ThermalAdvice Feedback::thermal(const monitor::SystemSnapshot* sys, Clock::time_point now) {
  ThermalAdvice a;
  if (sys && sys->thermal) a.margin_c = thermal_margin(*sys->thermal);
  if (a.margin_c) {
    const double m = *a.margin_c;
    const int want = m < 7.0 ? 2 : (m < 15.0 ? 1 : 0);
    if (want > derate_) {
      derate_ = want;
      last_derate_change_ = now;
    } else if (want < derate_ && now - last_derate_change_ >= std::chrono::seconds(15)) {
      const double threshold = derate_ == 2 ? 7.0 : 15.0;
      if (m >= threshold + 5.0) {
        --derate_;
        last_derate_change_ = now;
      }
    }
  }
  a.derate = derate_;
  return a;
}

PressureAdvice Feedback::pressure(const monitor::SystemSnapshot* sys) {
  PressureAdvice p;
  if (sys && sys->psi) {
    auto some10 = [](const std::optional<monitor::PsiResource>& r) { return r && r->some ? r->some->avg10 : 0.0; };
    p.cpu10 = some10(sys->psi->cpu);
    p.mem10 = some10(sys->psi->memory);
    p.io10 = some10(sys->psi->io);
    if (p.cpu10 >= 30.0) { if (++cpu_streak_ >= 3) cpu_ = true; }
    else if (p.cpu10 < 15.0) { cpu_streak_ = 0; cpu_ = false; }
    if (p.mem10 >= 10.0) mem_ = true; else if (p.mem10 < 5.0) mem_ = false;
    if (p.io10 >= 20.0) io_ = true; else if (p.io10 < 10.0) io_ = false;
  }
  p.cpu = cpu_;
  p.memory = mem_;
  p.io = io_;
  return p;
}

}  // namespace opt::policy
