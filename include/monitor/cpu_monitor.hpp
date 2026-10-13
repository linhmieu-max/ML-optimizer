#pragma once
#include <filesystem>
#include <optional>
#include <vector>

#include "core/result.hpp"
#include "monitor/types.hpp"

namespace opt::monitor {

class CpuMonitor {
 public:
  explicit CpuMonitor(std::filesystem::path root = "/") : root_(std::move(root)) {}
  Result<CpuSample> sample();

 private:
  struct Times {
    std::uint64_t total = 0, idle = 0;
  };
  std::filesystem::path root_;
  std::optional<Times> prev_total_;
  std::vector<Times> prev_cpu_;
};

}  // namespace opt::monitor
