#include "workload/browser_detector.hpp"

namespace opt::workload {

Evidence detect_browser(const DetectContext& c) {
  Evidence e;
  const int total = c.count([](const ProcessInfo& p) { return is_browser(p); });
  if (total == 0) return e;
  e.add(0.10, std::to_string(total) + " browser processes");
  const double cpu = c.active_cpu([](const ProcessInfo& p) { return is_browser(p); });
  if (cpu >= kActiveCpuPct) e.add(0.30, "browser CPU " + std::to_string(static_cast<int>(cpu)) + "%");
  if (cpu >= 20.0) e.add(0.15, "sustained browser CPU");
  if (total >= 3 && cpu >= kActiveCpuPct) e.add(0.10, "multi-process browser");
  if (c.count([](const ProcessInfo& p) { return (p.cmd_flags & kCmdChromiumRenderer) != 0; }) > 0 && cpu >= kActiveCpuPct)
    e.add(0.10, "renderer processes active");
  return e;
}

}  // namespace opt::workload
