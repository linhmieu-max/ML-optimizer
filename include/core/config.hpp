#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/logging.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

namespace opt {

inline constexpr const char* kDefaultConfigPath = "/etc/ml-optimizer/ml-optimizer.conf";
inline constexpr const char* kDefaultSocketPath = "/run/ml-optimizer/control.sock";
inline constexpr const char* kDefaultStateDir = "/var/lib/ml-optimizer";

struct Config {
  // [daemon]
  log::Level log_level = log::Level::Info;
  std::filesystem::path state_dir = kDefaultStateDir;
  // [ipc]
  std::filesystem::path socket_path = kDefaultSocketPath;
  std::string admin_group = "ml-optimizer";
  // [monitor]
  int monitor_interval_ms = 1000;
  // [workload]
  int workload_interval_ms = 2000;
  // [controllers]
  std::string controllers_mode = "dry-run";  // off | dry-run | apply
  bool runtime_pm = false;                   // opt-in
  // [telemetry]
  bool telemetry_enabled = true;
  int telemetry_interval_ms = 10000;
  int telemetry_detail_days = 7;
  int telemetry_summary_days = 365;
  int telemetry_max_db_mb = 64;
  // [ml]
  bool ml_enabled = true;
  std::string ml_mode = "observe";  // observe | suggest
  double ml_min_confidence = 0.75;
  bool ml_training = true;               // local adaptation (idle time only)
  int ml_training_min_samples = 500;
  int ml_training_interval_hours = 6;
  int ml_training_idle_seconds = 60;
  bool ml_training_on_battery = false;
  // [safety]
  int safety_interval_ms = 1000;
  // [policy]
  int aggressiveness = kAggressivenessDefault;
  int policy_interval_ms = 2000;
};

/// Strict INI parser: unknown sections/keys are errors so typos never go unnoticed.
Result<Config> parse_config(std::string_view text, Config base = {});

/// Checks cross-field invariants (absolute paths, ranges, sun_path length...).
Result<void> validate_config(const Config&);

/// A missing file is not an error: defaults are returned and *found is false.
Result<Config> load_config(const std::filesystem::path& file, bool* found = nullptr);

}  // namespace opt
