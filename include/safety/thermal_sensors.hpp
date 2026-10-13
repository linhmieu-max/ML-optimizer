#pragma once
// Read-only discovery of temperature sensors and their limits. Limit priority:
//   1. hardware/firmware hwmon limit (tempN_crit, else tempN_max)
//   2. kernel thermal-zone trip point (lowest "hot"/"critical")
//   3. (applied by ThermalSafety) conservative fallback for CPU-class sensors only

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace opt::safety {

enum class SensorKind { Cpu, Gpu, Platform, Other };  // Other = informational (wifi, nvme, battery...)

struct ThermalSensor {
  std::string name;          // "acpitz" or "coretemp:Package id 0"
  std::string source;        // "thermal_zone" | "hwmon"
  SensorKind kind = SensorKind::Other;
  double temp_c = 0;
  std::optional<double> limit_c;
  std::string limit_source;  // "hwmon crit", "kernel trip (hot)"...
};

std::vector<ThermalSensor> read_thermal_sensors(const std::filesystem::path& root = "/");

}  // namespace opt::safety
