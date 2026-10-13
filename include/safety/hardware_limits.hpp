#pragma once
// Limits reported by the hardware/firmware/kernel. Controllers (Phase 6) must
// validate every numeric value against these; we never exceed them and never
// overclock. Unknown limit => refuse.

#include <filesystem>
#include <optional>
#include <vector>

#include "core/result.hpp"
#include "safety/thermal_sensors.hpp"

namespace opt::safety {

enum class LimitKind { CpuFreqKhz, GpuFreqMhz, RaplPowerUw };

struct Range {
  long long min = 0, max = 0;
};

struct HardwareLimits {
  std::optional<Range> cpu_khz;            // cpuinfo_min/max_freq across policies
  std::optional<Range> gpu_mhz;            // i915 RPn..RP0
  std::optional<long long> rapl_max_uw;    // largest constraint_*_max_power_uw
  std::vector<ThermalSensor> thermal;      // sensors with their limits

  static HardwareLimits discover(const std::filesystem::path& root = "/");
  Result<void> validate(LimitKind kind, long long value) const;
};

std::string format_limits(const HardwareLimits&);

}  // namespace opt::safety
