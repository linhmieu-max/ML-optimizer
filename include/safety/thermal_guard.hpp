#pragma once
// Runs ThermalSafety on its OWN thread with its OWN sensor reads, independent of
// the monitor, scheduler and policy engine. Persists the state so that a daemon
// restart cannot "forget" a HOT/EMERGENCY condition.

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

#include "safety/thermal_safety.hpp"

namespace opt::safety {

struct GuardOptions {
  std::filesystem::path root = "/";
  std::filesystem::path state_file;                 // empty = do not persist
  std::chrono::milliseconds interval{1000};
  std::chrono::seconds restore_max_age{600};        // older saved HOT/EMERGENCY states are ignored
  SafetyConfig config;
};

class ThermalGuard {
 public:
  /// Called from the guard thread after every state change (keep it short).
  using ChangeFn = std::function<void(SafetyState from, SafetyState to, const std::string& reason)>;

  ThermalGuard(GuardOptions options, ChangeFn on_change);
  ~ThermalGuard() { stop(); }
  ThermalGuard(const ThermalGuard&) = delete;
  ThermalGuard& operator=(const ThermalGuard&) = delete;

  void start();   // restores the saved state, takes a first reading immediately, then loops
  void stop();
  SafetyState state() const { return static_cast<SafetyState>(state_.load(std::memory_order_acquire)); }
  SafetyStatus status() const;
  const SafetyConfig& config() const { return options_.config; }

  /// One iteration (public for tests and --check).
  void step(Clock::time_point now = Clock::now());
  void restore_saved(Clock::time_point now = Clock::now());

 private:
  void loop(std::stop_token);
  void persist(SafetyState, const std::string& reason);

  GuardOptions options_;
  ChangeFn on_change_;
  mutable std::mutex mutex_;
  ThermalSafety safety_;
  std::atomic<int> state_{0};
  std::condition_variable_any cv_;
  std::jthread thread_;
};

}  // namespace opt::safety
