#include "safety/thermal_safety.hpp"

#include <algorithm>
#include <cstdio>

#include "core/logging.hpp"

namespace opt::safety {
namespace {
int level(SafetyState s) { return static_cast<int>(s); }
SafetyState from_level(int l) { return static_cast<SafetyState>(std::clamp(l, 0, 3)); }
std::string f1(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}
}  // namespace

SafetyEffects effects_for(SafetyState s) {
  SafetyEffects e;
  switch (s) {
    case SafetyState::Normal: break;
    case SafetyState::Warm:
      e.block_performance_increase = true;
      break;
    case SafetyState::Hot:
      e.block_performance_increase = true;
      e.profile_cap = Profile::Balanced;
      e.allow_boost = false;
      break;
    case SafetyState::Emergency:
      e.block_performance_increase = true;
      e.profile_cap = Profile::ThermalEmergency;
      e.allow_boost = false;
      e.force_emergency = true;
      break;
  }
  return e;
}

int ThermalSafety::severity(double temp, double limit, double extra) const {
  if (temp >= limit - (cfg_.emergency_margin_c + extra)) return 3;
  if (temp >= limit - (cfg_.hot_margin_c + extra)) return 2;
  if (temp >= limit - (cfg_.warm_margin_c + extra)) return 1;
  return 0;
}

void ThermalSafety::transition(SafetyState to, Clock::time_point now, std::string reason) {
  const SafetyState from = status_.state;
  status_.state = to;
  status_.since = now;
  justified_ = now;
  status_.reason = std::move(reason);
  ++status_.transitions;
  status_.restored = false;
  log::safety("thermal", "Thermal state " + std::string(to_string(from)) + " -> " + std::string(to_string(to)),
              {{"reason", status_.reason}});
}

void ThermalSafety::restore(SafetyState s, Clock::time_point now) {
  status_.state = s;
  status_.since = now;
  justified_ = now;
  status_.restored = s != SafetyState::Normal;
  status_.reason = s == SafetyState::Normal ? "no data yet" : "restored from previous run";
  if (s != SafetyState::Normal)
    log::safety("thermal", "restored thermal state " + std::string(to_string(s)) + " after restart; hysteresis decides when to leave it");
}

SafetyState ThermalSafety::update(const std::vector<ThermalSensor>& sensors, Clock::time_point now) {
  // 1. Select usable sensors and their limits (hardware > kernel trip > conservative fallback).
  struct Used { const ThermalSensor* s; double limit; std::string source; };
  std::vector<Used> used;
  for (const auto& s : sensors) {
    if (s.kind == SensorKind::Other) continue;
    if (s.temp_c < cfg_.min_plausible_c || s.temp_c > cfg_.max_plausible_c) continue;  // glitch / bogus reading
    double limit = 0;
    std::string src;
    if (s.limit_c && *s.limit_c >= cfg_.min_plausible_limit_c && *s.limit_c <= cfg_.max_plausible_limit_c) {
      limit = *s.limit_c;
      src = s.limit_source;
    } else if (s.kind == SensorKind::Cpu) {
      limit = cfg_.fallback_limit_c;
      src = "conservative fallback";
    } else {
      continue;
    }
    used.push_back({&s, limit, src});
  }

  status_.sensors.clear();
  if (used.empty()) {
    status_.monitoring = false;
    const int cur = level(status_.state);
    if (had_sensors_) {
      // Sensors that used to work are gone: fail safe, never assume "cool".
      if (cur < 1) transition(SafetyState::Warm, now, "thermal sensors unreadable (fail-safe)");
      return status_.state;
    }
    // Nothing was ever readable (VM, container, no sensors): a restored state decays on its own.
    if (cur > 0 && now - status_.since >= cfg_.deescalate_hold) transition(from_level(cur - 1), now, "no thermal sensors available");
    return status_.state;
  }
  had_sensors_ = true;
  status_.monitoring = true;

  // 2. Per-sensor severity; raw uses entry margins, calm uses the hysteresis-widened margins.
  int raw = 0, calm = 0;
  const Used* worst = nullptr;
  for (const auto& u : used) {
    const int sv = severity(u.s->temp_c, u.limit, 0.0);
    raw = std::max(raw, sv);
    calm = std::max(calm, severity(u.s->temp_c, u.limit, cfg_.exit_hysteresis_c));
    status_.sensors.push_back({u.s->name, u.source, u.s->temp_c, u.limit, sv});
    if (!worst || (u.limit - u.s->temp_c) < (worst->limit - worst->s->temp_c)) worst = &u;
  }
  const std::string why = worst ? worst->s->name + " " + f1(worst->s->temp_c) + " C, limit " + f1(worst->limit) + " C (" + worst->source + ")" : "";

  // 3. Hysteresis state machine.
  const int cur = level(status_.state);
  if (raw > cur) {
    if (raw >= 3 || ++streak_ >= cfg_.escalate_confirmations) {
      streak_ = 0;
      transition(from_level(raw), now, why);
    }
  } else {
    streak_ = 0;
    // The hold counts from the last moment a sensor still warranted this level.
    if (cur > 0 && raw == cur) justified_ = now;
    if (calm < cur && now - std::max(status_.since, justified_) >= cfg_.deescalate_hold) transition(from_level(cur - 1), now, "cooled: " + why);
    else if (status_.state == SafetyState::Normal) status_.reason = "all sensors within limits: " + why;
  }
  return status_.state;
}

std::string format_safety(const SafetyStatus& st, const SafetyConfig& cfg, Clock::time_point now) {
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - st.since).count();
  std::string s = "Thermal safety: " + std::string(to_string(st.state)) + "  (for " + std::to_string(secs) + "s, " +
                  std::to_string(st.transitions) + " transitions" + (st.restored ? ", restored after restart" : "") + ")\n";
  s += "Reason:         " + st.reason + "\n";
  s += std::string("Monitoring:     ") + (st.monitoring ? "active" : "no usable sensors") + "\n";
  s += "Levels:         WARM at limit-" + f1(cfg.warm_margin_c) + ", HOT at limit-" + f1(cfg.hot_margin_c) + ", EMERGENCY at limit-" +
       f1(cfg.emergency_margin_c) + " C; leave a level " + f1(cfg.exit_hysteresis_c) + " C cooler\n";
  for (const auto& a : st.sensors) {
    const char* sev[] = {"ok", "WARM", "HOT", "EMERGENCY"};
    s += "  " + a.name + ": " + f1(a.temp_c) + " C / limit " + f1(a.limit_c) + " C [" + a.limit_source + "] " + sev[std::clamp(a.severity, 0, 3)] + "\n";
  }
  return s;
}

}  // namespace opt::safety
