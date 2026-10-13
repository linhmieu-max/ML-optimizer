#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.hpp"

namespace opt {

struct Capability {
  std::string id;      // stable identifier, e.g. "cpufreq", "sched.uclamp"
  CapStatus status = CapStatus::Unknown;
  std::string detail;  // human-readable specifics
};

class CapabilityReport {
 public:
  std::string kernel_release;
  int kernel_major = 0;
  int kernel_minor = 0;

  void add(Capability c) { caps_.push_back(std::move(c)); }
  const std::vector<Capability>& all() const noexcept { return caps_; }
  const Capability* find(std::string_view id) const noexcept;
  bool available(std::string_view id) const noexcept;
  bool kernel_at_least(int major, int minor) const noexcept;

 private:
  std::vector<Capability> caps_;
};

/// Read-only probing of /proc and /sys. `root` re-bases every path so tests can
/// use a fake tree; `kernel_release` overrides uname(2) (tests only).
/// Never writes anything and never assumes an interface exists.
CapabilityReport detect_capabilities(const std::filesystem::path& root = "/",
                                     std::string kernel_release = {});

std::string format_report(const CapabilityReport&);

}  // namespace opt
