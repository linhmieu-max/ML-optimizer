#pragma once
#include <filesystem>
#include <map>

#include "core/result.hpp"
#include "monitor/types.hpp"

namespace opt::monitor {

/// Reads kernel thermal zones, their trip points (cached after first read) and
/// cooling device states. Disabled trips (-274000 placeholders) are dropped.
class ThermalMonitor {
 public:
  explicit ThermalMonitor(std::filesystem::path root = "/") : root_(std::move(root)) {}
  Result<ThermalSample> sample();

 private:
  std::filesystem::path root_;
  std::map<std::string, std::vector<ThermalTrip>> trip_cache_;
};

}  // namespace opt::monitor
