#include "workload/compile_detector.hpp"

namespace opt::workload {

Evidence detect_compile(const DetectContext& c) {
  Evidence e;
  auto worker = [](const ProcessInfo& p) { return is_compile_worker(p); };
  const int workers = c.count_active(worker);
  if (workers == 0) return e;
  const auto* top = c.busiest(worker);
  e.add(0.50, "compiler/linker active: " + top->comm + " (" + std::to_string(workers) + " workers)");
  if (workers >= 2) e.add(0.20, "parallel compile jobs");
  if (c.count_active([](const ProcessInfo& p) { return is_compile_orchestrator(p); }, 0.0) > 0) e.add(0.20, "build orchestrator running");
  if (c.cpu_util() >= 50.0) e.add(0.10, "system CPU busy");
  return e;
}

}  // namespace opt::workload
