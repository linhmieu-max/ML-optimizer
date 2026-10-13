#pragma once
#include <filesystem>
#include <map>

#include "core/result.hpp"
#include "monitor/types.hpp"

namespace opt::monitor {

/// DRM cards (i915, xe, amdgpu; NVIDIA reported as unmonitored) and devfreq
/// devices. Only interfaces that exist are read; nothing is invented.
class GpuMonitor {
 public:
  explicit GpuMonitor(std::filesystem::path root = "/") : root_(std::move(root)) {}
  Result<GpuSample> sample(Clock::time_point now = Clock::now());

 private:
  struct Rc6 {
    std::uint64_t ms = 0;
    Clock::time_point at;
  };
  std::filesystem::path root_;
  std::map<std::string, Rc6> prev_rc6_;
};

}  // namespace opt::monitor
