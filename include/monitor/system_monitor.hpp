#pragma once
#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "monitor/cpu_monitor.hpp"
#include "monitor/gpu_monitor.hpp"
#include "monitor/io_monitor.hpp"
#include "monitor/memory_monitor.hpp"
#include "monitor/power_monitor.hpp"
#include "monitor/psi_monitor.hpp"
#include "monitor/thermal_monitor.hpp"

namespace opt::monitor {

struct SystemSnapshot {
  std::uint64_t sequence = 0;
  std::chrono::system_clock::time_point wall;
  std::optional<CpuSample> cpu;
  std::optional<MemorySample> memory;
  std::optional<PsiSample> psi;
  std::optional<ThermalSample> thermal;
  std::optional<PowerSample> power;
  std::optional<IoSample> io;
  std::optional<GpuSample> gpu;
};

/// Aggregates all monitors. Not thread-safe: call sample() from one thread.
/// A monitor that is unsupported yields an empty section silently; any other
/// failure is logged once (WARN) and the section is left empty.
class SystemMonitor {
 public:
  explicit SystemMonitor(std::filesystem::path root = "/");
  SystemSnapshot sample(Clock::time_point now = Clock::now());

 private:
  template <class T>
  std::optional<T> take(const char* name, Result<T> r);

  CpuMonitor cpu_;
  MemoryMonitor mem_;
  PsiMonitor psi_;
  ThermalMonitor thermal_;
  PowerMonitor power_;
  IoMonitor io_;
  GpuMonitor gpu_;
  std::uint64_t seq_ = 0;
  std::set<std::string> reported_;
};

/// One (label, value) per subsystem, used by `status`.
std::vector<std::pair<std::string, std::string>> format_summary(const SystemSnapshot&);
/// Multi-line detailed view, used by `monitor`.
std::string format_snapshot(const SystemSnapshot&);

}  // namespace opt::monitor
