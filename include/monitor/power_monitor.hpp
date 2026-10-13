#pragma once
#include <filesystem>
#include <map>
#include <optional>

#include "core/result.hpp"
#include "monitor/types.hpp"

namespace opt::monitor {

class PowerMonitor {
 public:
  explicit PowerMonitor(std::filesystem::path root = "/") : root_(std::move(root)) {}
  Result<PowerSample> sample(Clock::time_point now = Clock::now());

 private:
  struct Rapl {
    std::uint64_t uj = 0;
    Clock::time_point at;
  };
  std::filesystem::path root_;
  std::map<std::string, Rapl> prev_rapl_;
};

}  // namespace opt::monitor
