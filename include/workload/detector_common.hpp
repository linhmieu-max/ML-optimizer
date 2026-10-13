#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "monitor/system_monitor.hpp"
#include "workload/names.hpp"

namespace opt::workload {

/// A process counts as "active" at or above this share of one core.
inline constexpr double kActiveCpuPct = 5.0;

struct Evidence {
  double score = 0.0;  // 0..1
  std::vector<std::string> reasons;
  void add(double points, std::string reason) {
    score = std::min(1.0, score + points);
    reasons.push_back(std::move(reason));
  }
};

/// Everything a detector may look at. `sys` can be null (no monitor sample yet).
struct DetectContext {
  const ProcessSnapshot& procs;
  const monitor::SystemSnapshot* sys = nullptr;

  double cpu_util() const { return sys && sys->cpu && sys->cpu->utilization_pct ? *sys->cpu->utilization_pct : 0.0; }
  double psi_cpu10() const {
    return sys && sys->psi && sys->psi->cpu && sys->psi->cpu->some ? sys->psi->cpu->some->avg10 : 0.0;
  }
  double mem_used_pct() const { return sys && sys->memory ? sys->memory->used_pct() : 0.0; }
  double swap_pages_s() const {
    return sys && sys->memory && sys->memory->swap_in_pages_s ? *sys->memory->swap_in_pages_s + sys->memory->swap_out_pages_s.value_or(0) : 0.0;
  }
  /// Highest GPU busy percentage (i915 value is an RC6-based estimate).
  double gpu_busy() const {
    double b = 0;
    if (sys && sys->gpu)
      for (const auto& g : sys->gpu->devices)
        if (g.busy_pct) b = std::max(b, *g.busy_pct);
    return b;
  }
  double io_read_mbps() const { return sys && sys->io && sys->io->total_read_bps ? *sys->io->total_read_bps / 1e6 : 0.0; }
  double io_write_mbps() const { return sys && sys->io && sys->io->total_write_bps ? *sys->io->total_write_bps / 1e6 : 0.0; }
  bool audio_running() const { return procs.audio_running.value_or(false); }

  template <class Pred>
  double active_cpu(Pred pred) const {
    double s = 0;
    for (const auto& p : procs.procs)
      if (p.cpu_pct && pred(p)) s += *p.cpu_pct;
    return s;
  }
  template <class Pred>
  int count_active(Pred pred, double min_cpu = kActiveCpuPct) const {
    int n = 0;
    for (const auto& p : procs.procs)
      if (p.cpu_pct && *p.cpu_pct >= min_cpu && pred(p)) ++n;
    return n;
  }
  template <class Pred>
  int count(Pred pred) const {
    int n = 0;
    for (const auto& p : procs.procs)
      if (pred(p)) ++n;
    return n;
  }
  /// Busiest matching process or nullptr.
  template <class Pred>
  const ProcessInfo* busiest(Pred pred) const {
    const ProcessInfo* best = nullptr;
    for (const auto& p : procs.procs)
      if (p.cpu_pct && pred(p) && (!best || *p.cpu_pct > *best->cpu_pct)) best = &p;
    return best;
  }
};

}  // namespace opt::workload
