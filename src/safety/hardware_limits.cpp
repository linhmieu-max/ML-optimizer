#include "safety/hardware_limits.hpp"

#include <algorithm>
#include <cstdio>

#include "core/sysfs.hpp"

namespace opt::safety {
namespace fs = std::filesystem;
using namespace sysfs;

HardwareLimits HardwareLimits::discover(const fs::path& root) {
  HardwareLimits h;

  for (const auto& p : list_dir(root / "sys/devices/system/cpu/cpufreq", "policy")) {
    const auto lo = read_int(p / "cpuinfo_min_freq"), hi = read_int(p / "cpuinfo_max_freq");
    if (!lo || !hi || *lo <= 0 || *hi < *lo) continue;
    if (!h.cpu_khz) h.cpu_khz = Range{*lo, *hi};
    else h.cpu_khz = Range{std::min(h.cpu_khz->min, *lo), std::max(h.cpu_khz->max, *hi)};
  }

  for (const auto& c : list_dir(root / "sys/class/drm", "card")) {
    if (!all_digits(c.filename().string().substr(4))) continue;
    std::optional<long long> hi, lo;
    for (const char* rel : {"gt_RP0_freq_mhz", "gt/gt0/rps_RP0_freq_mhz"})
      if ((hi = read_int(c / rel))) break;
    for (const char* rel : {"gt_RPn_freq_mhz", "gt/gt0/rps_RPn_freq_mhz"})
      if ((lo = read_int(c / rel))) break;
    if (!hi || !lo || *lo <= 0 || *hi < *lo) continue;
    if (!h.gpu_mhz) h.gpu_mhz = Range{*lo, *hi};
    else h.gpu_mhz = Range{std::min(h.gpu_mhz->min, *lo), std::max(h.gpu_mhz->max, *hi)};
  }

  for (const auto& d : list_dir(root / "sys/class/powercap", "intel-rapl:")) {
    for (int i = 0; i < 4; ++i)
      if (auto mx = read_int(d / ("constraint_" + std::to_string(i) + "_max_power_uw")); mx && *mx > 0)
        h.rapl_max_uw = std::max(h.rapl_max_uw.value_or(0), *mx);
  }

  for (auto& s : read_thermal_sensors(root))
    if (s.limit_c) h.thermal.push_back(std::move(s));
  return h;
}

Result<void> HardwareLimits::validate(LimitKind kind, long long value) const {
  auto in_range = [&](const std::optional<Range>& r, const char* what, const char* unit) -> Result<void> {
    if (!r) return fail(make_error(Errc::Unsupported, std::string("no hardware limit known for ") + what + ": refusing"));
    if (value < r->min || value > r->max)
      return fail(make_error(Errc::InvalidArgument, std::string(what) + " " + std::to_string(value) + " " + unit + " outside hardware range " +
                                                        std::to_string(r->min) + ".." + std::to_string(r->max)));
    return {};
  };
  switch (kind) {
    case LimitKind::CpuFreqKhz: return in_range(cpu_khz, "CPU frequency", "kHz");
    case LimitKind::GpuFreqMhz: return in_range(gpu_mhz, "GPU frequency", "MHz");
    case LimitKind::RaplPowerUw:
      if (!rapl_max_uw) return fail(make_error(Errc::Unsupported, "no firmware power limit known: refusing to change power limits"));
      if (value <= 0 || value > *rapl_max_uw)
        return fail(make_error(Errc::InvalidArgument, "power limit " + std::to_string(value) + " uW exceeds firmware maximum " + std::to_string(*rapl_max_uw)));
      return {};
  }
  return fail(make_error(Errc::Internal, "unknown limit kind"));
}

std::string format_limits(const HardwareLimits& h) {
  std::string s = "Hardware limits (never exceeded, no overclocking):\n";
  auto r = [&](const char* name, const std::optional<Range>& v, const char* unit) {
    s += std::string("  ") + name + ": " + (v ? std::to_string(v->min) + ".." + std::to_string(v->max) + " " + unit : std::string("unknown (writes refused)")) + "\n";
  };
  r("cpu frequency ", h.cpu_khz, "kHz");
  r("gpu frequency ", h.gpu_mhz, "MHz");
  s += "  rapl max power: " + (h.rapl_max_uw ? std::to_string(*h.rapl_max_uw) + " uW" : std::string("unknown (writes refused)")) + "\n";
  s += "  thermal limits: " + std::to_string(h.thermal.size()) + " sensors with limits\n";
  return s;
}

}  // namespace opt::safety
