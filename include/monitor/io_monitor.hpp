#pragma once
#include <filesystem>
#include <map>
#include <optional>

#include "core/result.hpp"
#include "monitor/types.hpp"

namespace opt::monitor {

/// Whole block devices from /proc/diskstats (partitions, loop, ram, zram skipped).
class IoMonitor {
 public:
  explicit IoMonitor(std::filesystem::path root = "/") : root_(std::move(root)) {}
  Result<IoSample> sample(Clock::time_point now = Clock::now());

 private:
  struct Raw {
    std::uint64_t reads = 0, sectors_r = 0, writes = 0, sectors_w = 0, io_ms = 0;
  };
  std::filesystem::path root_;
  std::map<std::string, Raw> prev_;
  std::optional<Clock::time_point> prev_at_;
};

}  // namespace opt::monitor
