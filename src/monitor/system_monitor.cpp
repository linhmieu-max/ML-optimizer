#include "monitor/system_monitor.hpp"

#include "core/logging.hpp"

namespace opt::monitor {

SystemMonitor::SystemMonitor(std::filesystem::path root)
    : cpu_(root), mem_(root), psi_(root), thermal_(root), power_(root), io_(root), gpu_(root) {}

template <class T>
std::optional<T> SystemMonitor::take(const char* name, Result<T> r) {
  if (r) return std::move(*r);
  const Error& e = r.error();
  if (e.code != Errc::Unsupported && reported_.insert(name).second)
    log::warn("monitor", "sampling failed; section disabled until it recovers",
              {{"monitor", name}, {"error", describe(e)}});
  return std::nullopt;
}

SystemSnapshot SystemMonitor::sample(Clock::time_point now) {
  SystemSnapshot s;
  s.sequence = ++seq_;
  s.wall = std::chrono::system_clock::now();
  s.cpu = take("cpu", cpu_.sample());
  s.memory = take("memory", mem_.sample(now));
  s.psi = take("psi", psi_.sample());
  s.thermal = take("thermal", thermal_.sample());
  s.power = take("power", power_.sample(now));
  s.io = take("io", io_.sample(now));
  s.gpu = take("gpu", gpu_.sample(now));
  return s;
}

}  // namespace opt::monitor
