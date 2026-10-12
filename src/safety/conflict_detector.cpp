#include "safety/conflict_detector.hpp"

namespace opt::safety {
using policy::Subsystem;
namespace {

struct Known {
  std::string_view comm_prefix;
  const char* name;
  const char* description;
  std::vector<Subsystem> subsystems;
  bool thermal;
  bool game_scoped;
};

const std::vector<Known>& known() {
  static const std::vector<Known> k = {
      {"power-profiles", "power-profiles-daemon", "CPU energy/performance preference and platform profile",
       {Subsystem::Cpu}, false, false},
      {"tuned", "TuneD", "CPU, I/O, sysctl and runtime PM profiles",
       {Subsystem::Cpu, Subsystem::Io, Subsystem::Memory, Subsystem::RuntimePm}, false, false},
      {"auto-cpufreq", "auto-cpufreq", "CPU governor, EPP and turbo", {Subsystem::Cpu}, false, false},
      {"system76-power", "system76-power", "CPU/GPU power profiles", {Subsystem::Cpu, Subsystem::Gpu, Subsystem::RuntimePm}, false, false},
      {"tlp", "TLP", "CPU, disk and runtime PM settings", {Subsystem::Cpu, Subsystem::Io, Subsystem::RuntimePm}, false, false},
      {"thermald", "thermald", "thermal management (RAPL/cooling devices)", {}, true, false},
      {"gamemoded", "GameMode", "governor, I/O priority and GPU performance while a game runs",
       {Subsystem::Cpu, Subsystem::Gpu, Subsystem::Io}, false, true},
  };
  return k;
}

}  // namespace

std::vector<ExternalTool> detect_running_tools(const workload::ProcessSnapshot& snap) {
  std::vector<ExternalTool> out;
  for (const auto& k : known()) {
    for (const auto& p : snap.procs) {
      const bool exact_only = k.comm_prefix == "tlp";
      const bool match = exact_only ? p.comm == k.comm_prefix : p.comm.starts_with(k.comm_prefix);
      if (!match) continue;
      out.push_back({k.name, k.description, k.subsystems, k.thermal, k.game_scoped, p.pid});
      break;  // one entry per tool
    }
  }
  return out;
}

}  // namespace opt::safety
