#include "control/io.hpp"

#include "core/sysfs.hpp"

namespace opt::control {
using namespace sysfs;
using policy::IoBias;

// Only ROTATIONAL disks are tuned (bfq for latency, mq-deadline for throughput). NVMe and
// other non-rotational devices keep "none"/their default: no measurable gain, real risk.
std::vector<Desired> plan_io(const ControlContext& c) {
  std::vector<Desired> out;
  const IoBias bias = c.decision.targets.io;
  if (bias == IoBias::Balanced) return out;
  for (const auto& dev : list_dir(c.root / "sys/block")) {
    const std::string name = dev.filename().string();
    if (name.starts_with("loop") || name.starts_with("ram") || name.starts_with("zram") || name.starts_with("dm-") ||
        name.starts_with("md") || name.starts_with("nvme"))
      continue;
    if (read_int(dev / "queue/rotational") != 1) continue;
    const auto sched = read_text(dev / "queue/scheduler");
    if (!sched) continue;
    auto has = [&](std::string_view s) {
      for (auto tok : split_ws(*sched)) {
        if (!tok.empty() && tok.front() == '[') tok = tok.substr(1, tok.size() - 2);
        if (tok == s) return true;
      }
      return false;
    };
    const char* want = bias == IoBias::Latency ? "bfq" : "mq-deadline";
    if (!has(want)) continue;
    Desired d;
    d.controller = "io";
    d.subsystem = policy::Subsystem::Io;
    d.target = "/sys/block/" + name + "/queue/scheduler";
    d.value = want;
    d.reason = std::string("I/O ") + (bias == IoBias::Latency ? "latency" : "throughput") + " bias on rotational disk";
    out.push_back(std::move(d));
  }
  return out;
}

}  // namespace opt::control
