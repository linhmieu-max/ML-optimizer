#include "safety/thermal_guard.hpp"

#include <ctime>

#include "core/atomic_file.hpp"
#include "core/logging.hpp"
#include "core/sysfs.hpp"

namespace opt::safety {
namespace {

std::optional<SafetyState> parse_state(std::string_view s) {
  if (s == "NORMAL") return SafetyState::Normal;
  if (s == "WARM") return SafetyState::Warm;
  if (s == "HOT") return SafetyState::Hot;
  if (s == "EMERGENCY") return SafetyState::Emergency;
  return std::nullopt;
}

}  // namespace

ThermalGuard::ThermalGuard(GuardOptions options, ChangeFn on_change)
    : options_(std::move(options)), on_change_(std::move(on_change)), safety_(options_.config) {}

SafetyStatus ThermalGuard::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return safety_.status();
}

void ThermalGuard::persist(SafetyState s, const std::string& reason) {
  if (options_.state_file.empty()) return;
  std::string clean = reason;
  for (char& c : clean)
    if (c == '\n' || c == '\r') c = ' ';
  const std::string content = "state=" + std::string(to_string(s)) + "\nwall=" + std::to_string(std::time(nullptr)) + "\nreason=" + clean + "\n";
  if (auto r = write_file_atomic(options_.state_file, content); !r)
    log::warn("thermal", "cannot persist safety state", {{"error", describe(r.error())}});
}

void ThermalGuard::restore_saved(Clock::time_point now) {
  if (options_.state_file.empty()) return;
  auto text = sysfs::read_text(options_.state_file, 4096);
  if (!text) return;
  std::optional<SafetyState> st;
  std::optional<std::uint64_t> wall;
  for (auto line : sysfs::split_lines(*text)) {
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    const auto k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "state") st = parse_state(v);
    else if (k == "wall") wall = sysfs::parse_u64(v);
  }
  if (!st || !wall) {
    log::warn("thermal", "ignoring malformed saved safety state");
    return;
  }
  const auto age = static_cast<std::int64_t>(std::time(nullptr)) - static_cast<std::int64_t>(*wall);
  if (*st == SafetyState::Normal || age < 0 || age > options_.restore_max_age.count()) return;  // stale: sensors decide
  std::lock_guard<std::mutex> lock(mutex_);
  safety_.restore(*st, now);
  state_.store(static_cast<int>(*st), std::memory_order_release);
}

void ThermalGuard::step(Clock::time_point now) {
  const auto sensors = read_thermal_sensors(options_.root);  // own reads: no dependency on the monitor
  SafetyState from, to;
  std::string reason;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    from = safety_.state();
    to = safety_.update(sensors, now);
    reason = safety_.status().reason;
    state_.store(static_cast<int>(to), std::memory_order_release);
  }
  if (from != to) {
    persist(to, reason);
    if (on_change_) {
      try {
        on_change_(from, to, reason);
      } catch (const std::exception& e) {
        log::error("thermal", "state-change handler threw", {{"what", e.what()}});
      }
    }
  }
}

void ThermalGuard::start() {
  if (thread_.joinable()) return;
  restore_saved();
  thread_ = std::jthread([this](std::stop_token token) { loop(token); });
}

void ThermalGuard::stop() {
  if (!thread_.joinable()) return;
  thread_.request_stop();
  thread_.join();
}

void ThermalGuard::loop(std::stop_token token) {
  std::mutex wait_mutex;
  std::unique_lock<std::mutex> lock(wait_mutex);
  while (!token.stop_requested()) {
    step();
    cv_.wait_for(lock, token, options_.interval, [] { return false; });
  }
}

}  // namespace opt::safety
