#include "safety/safety_gate.hpp"

#include "core/logging.hpp"
#include "safety/thermal_safety.hpp"

namespace opt::safety {
namespace {

// '*' matches any run of characters inside ONE path segment.
bool wildcard(std::string_view pat, std::string_view s) {
  std::size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
  while (i < s.size()) {
    if (p < pat.size() && pat[p] == '*') { star = p++; mark = i; }
    else if (p < pat.size() && pat[p] == s[i]) { ++p; ++i; }
    else if (star != std::string_view::npos) { p = star + 1; i = ++mark; }
    else return false;
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

bool match_path(std::string_view pattern, std::string_view path) {
  auto next = [](std::string_view& v) {
    if (!v.empty() && v.front() == '/') v.remove_prefix(1);
    const auto e = v.find('/');
    auto seg = v.substr(0, e);
    v.remove_prefix(e == std::string_view::npos ? v.size() : e);
    return seg;
  };
  while (!pattern.empty() || !path.empty()) {
    if (pattern.empty() || path.empty()) return false;
    if (!wildcard(next(pattern), next(path))) return false;
  }
  return true;
}

constexpr std::string_view kAllowed[] = {
    "/sys/devices/system/cpu/cpufreq/policy*/scaling_governor",
    "/sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq",
    "/sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq",
    "/sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference",
    "/sys/devices/system/cpu/intel_pstate/no_turbo",
    "/sys/devices/system/cpu/cpufreq/boost",
    "/sys/class/devfreq/*/governor",
    "/sys/class/devfreq/*/min_freq",
    "/sys/class/devfreq/*/max_freq",
    "/sys/class/drm/card*/gt_min_freq_mhz",
    "/sys/class/drm/card*/gt_max_freq_mhz",
    "/sys/class/drm/card*/gt/gt*/rps_min_freq_mhz",
    "/sys/class/drm/card*/gt/gt*/rps_max_freq_mhz",
    "/sys/block/*/queue/scheduler",
    "/proc/sys/vm/swappiness",
    "/proc/sys/vm/vfs_cache_pressure",
    "/proc/sys/vm/dirty_ratio",
    "/proc/sys/vm/dirty_background_ratio",
    "/sys/bus/*/devices/*/power/control",
};
constexpr std::string_view kCgroupFiles[] = {"cpu.weight", "cpu.max", "io.weight", "memory.high", "memory.low"};

bool printable_no_dots(std::string_view p) {
  if (p.empty() || p.front() != '/' || p.size() > 256) return false;
  for (char c : p)
    if (static_cast<unsigned char>(c) < 0x21 || c == 0x7f) return false;
  if (p.find("//") != std::string_view::npos || p.find("/./") != std::string_view::npos || p.find("..") != std::string_view::npos) return false;
  return !p.ends_with("/") && !p.ends_with("/.");
}

Verdict deny(GateStage st, std::string why, const ChangeIntent& c) {
  log::safety("gate", "change blocked", {{"stage", std::string(to_string(st))}, {"target", c.target}, {"reason", why}});
  return {false, st, std::move(why)};
}

}  // namespace

bool is_allowed_target(std::string_view path) {
  if (!printable_no_dots(path)) return false;
  for (auto pat : kAllowed)
    if (match_path(pat, path)) return true;
  if (path.starts_with("/sys/fs/cgroup/")) {
    const auto name = path.substr(path.rfind('/') + 1);
    for (auto f : kCgroupFiles)
      if (name == f) return true;
  }
  return false;
}

Verdict check_change(const ChangeIntent& c, const GateContext& ctx) {
  // 0. Hard security rule: only known tuning files, never arbitrary paths.
  if (!is_allowed_target(c.target)) return deny(GateStage::Target, "target is not on the writable allowlist", c);

  if (c.safety_action) {
    if (c.raises_performance) return deny(GateStage::Thermal, "a safety action may never raise performance", c);
    if (c.limit_kind) {
      if (!c.numeric || !ctx.limits) return deny(GateStage::Limits, "limit check impossible: refusing", c);
      if (auto v = ctx.limits->validate(*c.limit_kind, *c.numeric); !v) return deny(GateStage::Limits, v.error().message, c);
    }
    return {true, GateStage::Allowed, "thermal safety action"};
  }

  // 1. Policy: there must be a current decision, and the subsystem must be drivable.
  if (!ctx.decision) return deny(GateStage::Policy, "no policy decision available", c);
  const auto& ctl = ctx.decision->of(c.subsystem);

  // 2. Capability.
  if (ctl.mode == policy::ControlMode::Unavailable)
    return deny(GateStage::Capability, std::string(to_string(c.subsystem)) + " backend unavailable: " + ctl.note, c);

  // 3. Conflict: never fight a tool that owns the subsystem.
  if (ctl.mode == policy::ControlMode::Observe)
    return deny(GateStage::Conflict, std::string(to_string(c.subsystem)) + " is observe-only: " + ctl.note, c);

  // 4. Thermal safety (cannot be bypassed by any profile, lock or aggressiveness level).
  const SafetyEffects eff = effects_for(ctx.safety);
  if (c.raises_performance && eff.block_performance_increase)
    return deny(GateStage::Thermal, "performance increase blocked: thermal state " + std::string(to_string(ctx.safety)), c);

  // 5. Hardware/firmware limits for numeric values.
  if (c.limit_kind) {
    if (!c.numeric) return deny(GateStage::Limits, "numeric value required for limit check", c);
    if (!ctx.limits) return deny(GateStage::Limits, "hardware limits unknown: refusing", c);
    if (auto v = ctx.limits->validate(*c.limit_kind, *c.numeric); !v) return deny(GateStage::Limits, v.error().message, c);
  }
  return {true, GateStage::Allowed, "ok"};
}

}  // namespace opt::safety
