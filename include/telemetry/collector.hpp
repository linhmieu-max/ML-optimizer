#pragma once
// Builds one telemetry row from the data the daemon already has. Privacy by construction:
// only numbers, the workload class name, the profile name and generic target words are stored;
// never process names, command lines, user names, hostnames or paths.

#include <optional>
#include <string>

#include "core/types.hpp"
#include "database/telemetry.hpp"
#include "monitor/system_monitor.hpp"
#include "policy/policy_engine.hpp"
#include "workload/types.hpp"

namespace opt::telemetry {

struct SampleInputs {
  std::int64_t unix_time = 0;
  double interval_s = 0;
  const monitor::SystemSnapshot* sys = nullptr;
  const workload::WorkloadResult* workload = nullptr;
  const policy::PolicyDecision* decision = nullptr;
  SafetyState safety = SafetyState::Normal;
  // controller activity since the previous sample
  std::uint64_t applied = 0, blocked = 0, failed = 0, reverted = 0;
  int profile_changes = 0;
};

class RowBuilder {
 public:
  RowBuilder(std::string session_id, std::string device_id_hash, std::string kernel_version, std::optional<double> cpu_hw_max_mhz)
      : session_(std::move(session_id)), device_(std::move(device_id_hash)), kernel_(std::move(kernel_version)), cpu_max_mhz_(cpu_hw_max_mhz) {}

  /// success = thermal state below HOT and no failed controller change.
  db::TelemetryRow build(const SampleInputs&) const;

 private:
  std::string session_, device_, kernel_;
  std::optional<double> cpu_max_mhz_;
};

}  // namespace opt::telemetry
