#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opt::workload {

enum class WorkloadClass {
  Idle, Desktop, Browser, VideoPlayback, Game, Compile, Render, Encode,
  FileCopy, Archive, Vm, Container, Server, Background,
};
inline constexpr std::size_t kClassCount = 14;

constexpr std::string_view to_string(WorkloadClass c) noexcept {
  switch (c) {
    case WorkloadClass::Idle: return "IDLE";
    case WorkloadClass::Desktop: return "DESKTOP";
    case WorkloadClass::Browser: return "BROWSER";
    case WorkloadClass::VideoPlayback: return "VIDEO_PLAYBACK";
    case WorkloadClass::Game: return "GAME";
    case WorkloadClass::Compile: return "COMPILE";
    case WorkloadClass::Render: return "RENDER";
    case WorkloadClass::Encode: return "ENCODE";
    case WorkloadClass::FileCopy: return "FILE_COPY";
    case WorkloadClass::Archive: return "ARCHIVE";
    case WorkloadClass::Vm: return "VM";
    case WorkloadClass::Container: return "CONTAINER";
    case WorkloadClass::Server: return "SERVER";
    case WorkloadClass::Background: return "BACKGROUND";
  }
  return "UNKNOWN";
}

/// Side features for the policy engine. They never imply a workload class:
/// "heavy app + lots of RAM" is not a GAME, it is HIGH_LOAD/RESOURCE_HEAVY.
enum Flag : unsigned { kHighLoad = 1, kLatencySensitive = 2, kResourceHeavy = 4 };

inline std::string flags_to_string(unsigned f) {
  std::string s;
  auto add = [&](unsigned bit, const char* n) {
    if (f & bit) s += (s.empty() ? "" : ",") + std::string(n);
  };
  add(kHighLoad, "HIGH_LOAD");
  add(kLatencySensitive, "LATENCY_SENSITIVE");
  add(kResourceHeavy, "RESOURCE_HEAVY");
  return s.empty() ? "none" : s;
}

struct ProcSummary {
  int pid = 0;
  std::string name;  // process name only, never the command line
  double cpu_pct = 0;
};

struct WorkloadResult {
  WorkloadClass cls = WorkloadClass::Idle;
  double confidence = 0;
  unsigned flags = 0;
  std::array<double, kClassCount> scores{};
  std::vector<std::string> reasons;             // why the current class scored
  std::vector<ProcSummary> top;                 // busiest processes
  std::optional<WorkloadClass> pending;         // challenger waiting for confirmation
  int pending_streak = 0;
  std::chrono::steady_clock::time_point since;  // when `cls` became current
  std::uint64_t evaluations = 0;
};

}  // namespace opt::workload
