#include "workload/container_detector.hpp"

namespace opt::workload {

Evidence detect_container(const DetectContext& c) {
  Evidence e;
  const double cpu = c.active_cpu([](const ProcessInfo& p) { return in_container_cgroup(p); });
  if (cpu < kActiveCpuPct) return e;
  e.add(0.45, "container cgroup CPU " + std::to_string(static_cast<int>(cpu)) + "%");
  if (cpu >= 25.0) e.add(0.20, "heavy container activity");
  if (cpu >= 100.0) e.add(0.15, "multi-core container load");
  if (c.count([](const ProcessInfo& p) { return is_container_runtime(p); }) > 0) e.add(0.10, "container runtime present");
  return e;
}

}  // namespace opt::workload
