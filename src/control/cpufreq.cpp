#include "control/cpufreq.hpp"

#include <algorithm>

#include "core/sysfs.hpp"

namespace opt::control {
namespace fs = std::filesystem;
using namespace sysfs;
using policy::Bias;

namespace {
bool listed(const std::optional<std::string>& list, std::string_view word) {
  if (!list) return false;
  for (auto w : split_ws(*list))
    if (w == word) return true;
  return false;
}
}  // namespace

// EPP is preferred (intel_pstate/amd-pstate); the governor is only used when EPP is absent.
// POWERSAVE never forces the "powersave" governor: on acpi-cpufreq that pins the minimum frequency.
std::vector<Desired> plan_cpufreq(const ControlContext& c) {
  std::vector<Desired> out;
  const auto& t = c.decision.targets;
  const bool emergency = c.decision.profile == Profile::ThermalEmergency;
  const fs::path cpu = c.root / "sys/devices/system/cpu";

  auto add = [&](std::string target, std::string value, bool raises, std::string why, bool safety = false,
                 std::optional<long long> numeric = std::nullopt) {
    Desired d;
    d.controller = "cpufreq";
    d.subsystem = policy::Subsystem::Cpu;
    d.target = std::move(target);
    d.value = std::move(value);
    d.raises_performance = raises;
    d.safety_action = safety;
    d.reason = std::move(why);
    if (numeric) {
      d.limit_kind = safety::LimitKind::CpuFreqKhz;
      d.numeric = numeric;
    }
    out.push_back(std::move(d));
  };

  for (const auto& pol : list_dir(cpu / "cpufreq", "policy")) {
    const std::string base = "/sys/devices/system/cpu/cpufreq/" + pol.filename().string() + "/";
    const auto epp = read_text(pol / "energy_performance_available_preferences");
    if (epp) {
      if (t.cpu == Bias::Power && listed(epp, "power")) add(base + "energy_performance_preference", "power", false, "power bias");
      else if (t.cpu == Bias::Performance && listed(epp, "performance")) add(base + "energy_performance_preference", "performance", true, "performance bias");
    } else if (t.cpu == Bias::Performance && listed(read_text(pol / "scaling_available_governors"), "performance")) {
      add(base + "scaling_governor", "performance", true, "performance bias (no EPP)");
    }
    if (emergency) {
      const auto hi = read_int(pol / "cpuinfo_max_freq"), lo = read_int(pol / "cpuinfo_min_freq");
      if (hi && lo && *hi > 0 && *lo > 0 && *hi >= *lo) {
        const long long cap = std::max(*lo, *hi / 2);
        add(base + "scaling_max_freq", std::to_string(cap), false, "THERMAL EMERGENCY: cap at 50% of maximum frequency", true, cap);
      }
    }
  }

  if (!t.allow_boost) {
    if (path_exists(cpu / "intel_pstate/no_turbo"))
      add("/sys/devices/system/cpu/intel_pstate/no_turbo", "1", false, "boost not allowed", emergency);
    else if (path_exists(cpu / "cpufreq/boost"))
      add("/sys/devices/system/cpu/cpufreq/boost", "0", false, "boost not allowed", emergency);
  }
  return out;
}

}  // namespace opt::control
