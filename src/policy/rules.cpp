#include "policy/rules.hpp"

namespace opt::policy {
using workload::WorkloadClass;

ProfileChoice choose_profile(const RuleInput& in) {
  ProfileChoice c;
  const bool battery = in.on_ac == false;
  const int pct = in.battery_pct.value_or(100);
  const bool latency = (in.flags & workload::kLatencySensitive) != 0;
  const bool high_load = (in.flags & workload::kHighLoad) != 0;
  const int a = in.aggressiveness;
  auto say = [&](std::string s) { c.reasons.push_back(std::move(s)); };

  // 1. Battery emergency beats everything.
  if (battery && pct <= 15) {
    c.profile = Profile::Powersave;
    c.urgent = true;
    say("battery critical (" + std::to_string(pct) + "%)");
    return c;
  }
  // 2. Power-source default.
  if (battery) {
    c.profile = pct <= 30 ? Profile::Powersave : Profile::Balanced;
    say("on battery (" + std::to_string(pct) + "%)");
  } else {
    c.profile = Profile::Balanced;
    say(in.on_ac ? "on AC power" : "power source unknown");
  }
  if (in.confidence < 0.3) {
    say("workload unclear (confidence " + std::to_string(in.confidence).substr(0, 4) + "): power-source default");
    return c;
  }

  const std::string cls(to_string(in.cls));
  switch (in.cls) {
    case WorkloadClass::Game:
    case WorkloadClass::Render:
    case WorkloadClass::Encode:
    case WorkloadClass::Compile: {
      const bool game = in.cls == WorkloadClass::Game;
      if (!battery) {
        const bool headroom = in.thermal_margin_c && *in.thermal_margin_c >= 20.0;
        if (a >= 2 || headroom) {
          c.profile = Profile::Performance;
          say(cls + " on AC: throughput/latency matters");
        } else {
          say(cls + " on AC, but conservative level without proven thermal headroom: stay BALANCED");
        }
      } else if (game && a >= 4 && pct >= 40) {
        c.profile = Profile::Performance;
        say("GAME on battery at experimental level (battery >= 40%)");
      } else if (game && pct > 30) {
        c.profile = Profile::Balanced;
        say("GAME on battery: not below BALANCED");
      }
      break;
    }
    case WorkloadClass::Vm:
    case WorkloadClass::Container:
    case WorkloadClass::Archive:
    case WorkloadClass::FileCopy:
    case WorkloadClass::Server:
      if (!battery && a >= 3 && high_load) {
        c.profile = Profile::Performance;
        say(cls + " under high load on AC");
      }
      break;
    case WorkloadClass::Desktop:
    case WorkloadClass::Browser:
      if (!battery && a >= 4 && high_load) {
        c.profile = Profile::Performance;
        say(cls + " under high load on AC (experimental level)");
      }
      break;
    case WorkloadClass::VideoPlayback:
      say("video playback: smooth decoding needs at least BALANCED");
      break;
    case WorkloadClass::Idle:
      if (battery || a >= 3) {
        c.profile = Profile::Powersave;
        say("idle: allow deepest idle/power saving");
      }
      break;
    case WorkloadClass::Background:
      if (!high_load && (battery || a >= 3)) {
        c.profile = Profile::Powersave;
        say("background-only activity: power saving");
      }
      break;
  }

  // 3. Latency-sensitive work never drops below BALANCED (except low battery).
  if (latency && c.profile == Profile::Powersave && !(battery && pct <= 30)) {
    c.profile = Profile::Balanced;
    say("latency-sensitive: not below BALANCED");
  }
  return c;
}

}  // namespace opt::policy
