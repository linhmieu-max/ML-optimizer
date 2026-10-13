#pragma once
#include <filesystem>
#include <optional>

#include "core/result.hpp"
#include "monitor/types.hpp"

namespace opt::monitor {

class MemoryMonitor {
 public:
  explicit MemoryMonitor(std::filesystem::path root = "/") : root_(std::move(root)) {}
  Result<MemorySample> sample(Clock::time_point now = Clock::now());

 private:
  std::filesystem::path root_;
  struct Prev {
    std::uint64_t in = 0, out = 0;
    Clock::time_point at;
  };
  std::optional<Prev> prev_;
};

}  // namespace opt::monitor
