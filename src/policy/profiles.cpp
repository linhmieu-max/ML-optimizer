#include "policy/profiles.hpp"

#include <algorithm>

namespace opt::policy {
using workload::WorkloadClass;

Targets base_targets(Profile p) {
  Targets t;
  switch (p) {
    case Profile::Powersave:
      t = {Bias::Power, false, Bias::Power, IoBias::Balanced, Restriction::Light, MemoryBias::Default, RuntimePm::PowerSaving};
      break;
    case Profile::Balanced:
      t = {Bias::Balanced, true, Bias::Balanced, IoBias::Balanced, Restriction::None, MemoryBias::Default, RuntimePm::Auto};
      break;
    case Profile::Performance:
      t = {Bias::Performance, true, Bias::Performance, IoBias::Balanced, Restriction::Light, MemoryBias::Default, RuntimePm::Performance};
      break;
    case Profile::ThermalEmergency:
      t = {Bias::Power, false, Bias::Power, IoBias::Balanced, Restriction::Strong, MemoryBias::Default, RuntimePm::PowerSaving};
      break;
  }
  return t;
}

void tune_for_workload(Targets& t, WorkloadClass c, unsigned flags, int aggr) {
  const bool strong_ok = aggr >= 3;  // conservative levels never use Strong restriction
  switch (c) {
    case WorkloadClass::Game:
      t.io = IoBias::Latency;
      t.memory = MemoryBias::Responsiveness;
      t.background = strong_ok ? Restriction::Strong : Restriction::Light;
      if (t.cpu != Bias::Power) t.gpu = Bias::Performance;
      break;
    case WorkloadClass::Compile:
      t.io = IoBias::Throughput;
      t.memory = MemoryBias::Throughput;
      t.background = Restriction::None;
      break;
    case WorkloadClass::Render:
    case WorkloadClass::Encode:
    case WorkloadClass::Archive:
      t.io = IoBias::Throughput;
      t.memory = MemoryBias::Throughput;
      if (t.background == Restriction::None) t.background = Restriction::Light;
      break;
    case WorkloadClass::FileCopy:
      t.io = IoBias::Throughput;
      t.cpu = std::min(t.cpu, Bias::Balanced);  // I/O bound: CPU headroom is wasted
      break;
    case WorkloadClass::VideoPlayback:
      t.io = IoBias::Balanced;
      t.memory = MemoryBias::Responsiveness;
      if (t.background == Restriction::None) t.background = Restriction::Light;
      break;
    case WorkloadClass::Desktop:
    case WorkloadClass::Browser:
      t.io = IoBias::Latency;
      t.memory = MemoryBias::Responsiveness;
      break;
    case WorkloadClass::Vm:
    case WorkloadClass::Container:
      t.memory = MemoryBias::Throughput;
      break;
    case WorkloadClass::Idle:
      t.runtime_pm = RuntimePm::PowerSaving;
      break;
    case WorkloadClass::Server:
    case WorkloadClass::Background:
      break;
  }
  if (flags & workload::kLatencySensitive) {
    if (t.io == IoBias::Throughput) t.io = IoBias::Balanced;  // never starve interactive I/O
    if (t.memory == MemoryBias::Throughput) t.memory = MemoryBias::Default;
  }
  if (!strong_ok && t.background == Restriction::Strong) t.background = Restriction::Light;
}

std::optional<Profile> parse_user_profile(std::string_view s) {
  if (s == "powersave") return Profile::Powersave;
  if (s == "balanced") return Profile::Balanced;
  if (s == "performance") return Profile::Performance;
  return std::nullopt;
}

std::string_view user_name(Profile p) {
  switch (p) {
    case Profile::Powersave: return "powersave";
    case Profile::Balanced: return "balanced";
    case Profile::Performance: return "performance";
    case Profile::ThermalEmergency: return "thermal-emergency";
  }
  return "?";
}

Profile step_down(Profile p, int steps) {
  if (p == Profile::ThermalEmergency) return p;
  int v = static_cast<int>(p);  // Powersave=0, Balanced=1, Performance=2
  v = std::max(0, v - std::max(0, steps));
  return static_cast<Profile>(v);
}

}  // namespace opt::policy
