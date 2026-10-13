#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "core/capabilities.hpp"
#include "control/control_manager.hpp"
#include "database/telemetry.hpp"
#include "ml/adaptation.hpp"
#include "ml/predictor.hpp"
#include "telemetry/collector.hpp"
#include "control/file_state_writer.hpp"
#include "core/config.hpp"
#include "daemon/scheduler.hpp"
#include "monitor/system_monitor.hpp"
#include "policy/policy_engine.hpp"
#include "policy/policy_store.hpp"
#include "safety/conflict_detector.hpp"
#include "safety/tool_inventory.hpp"
#include "safety/hardware_limits.hpp"
#include "safety/rollback.hpp"
#include "safety/thermal_guard.hpp"
#include "workload/classifier.hpp"
#include "workload/process_scanner.hpp"
#include "ipc/protocol.hpp"
#include "ipc/server.hpp"

namespace opt {

struct DaemonOptions {
  std::filesystem::path config_path = kDefaultConfigPath;
  bool check_only = false;  // validate config, print capabilities, exit
};

/// Phase 1 daemon: lifecycle, signals, config reload, IPC. It observes and
/// reports; it never writes to /sys, /proc/sys, cgroups or services.
class Daemon {
 public:
  explicit Daemon(DaemonOptions options);

  /// Blocks until SIGINT/SIGTERM. SIGINT/SIGTERM/SIGHUP must already be
  /// blocked in every thread (see main.cpp) so they are consumed here.
  int run();

 public:
  // Command handlers (commands.cpp). Public only so the dispatch table can
  // reference them; they are not part of any external API.
  ipc::Response cmd_ping(const ipc::Request&);
  ipc::Response cmd_version(const ipc::Request&);
  ipc::Response cmd_help(const ipc::Request&);
  ipc::Response cmd_status(const ipc::Request&);
  ipc::Response cmd_detect(const ipc::Request&);
  ipc::Response cmd_monitor(const ipc::Request&);
  ipc::Response cmd_workload(const ipc::Request&);
  ipc::Response cmd_profile(const ipc::Request&);
  ipc::Response cmd_policy(const ipc::Request&);
  ipc::Response cmd_conflicts(const ipc::Request&);
  ipc::Response cmd_safety(const ipc::Request&);
  ipc::Response cmd_reset(const ipc::Request&);
  ipc::Response cmd_controllers(const ipc::Request&);
  ipc::Response cmd_history(const ipc::Request&);
  ipc::Response cmd_telemetry(const ipc::Request&);
  ipc::Response cmd_model(const ipc::Request&);
  ipc::Response cmd_training(const ipc::Request&);
  ipc::Response cmd_set_log_level(const ipc::Request&);
  ipc::Response cmd_reload_config(const ipc::Request&);

 private:
  ipc::Response handle(const ipc::Request&, const ipc::PeerCred&);
  bool is_admin(const ipc::PeerCred&) const;
  std::string render_status() const;

  // daemon.cpp
  Result<void> reload_config();
  Config config_snapshot() const;
  void sample_once();
  void classify_once();
  void policy_once();
  void telemetry_once();
  ml::ProfileFeatureInput make_profile_input(const workload::WorkloadResult&, const monitor::SystemSnapshot*, int aggressiveness) const;
  void telemetry_maintain();
  std::string render_telemetry_status();
  std::filesystem::path policy_state_path() const;

  DaemonOptions options_;
  mutable std::mutex mutex_;  // guards config_
  Config config_;
  CapabilityReport startup_caps_;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  std::unique_ptr<monitor::SystemMonitor> monitor_;  // used only from the scheduler thread
  mutable std::mutex snapshot_mutex_;
  std::shared_ptr<const monitor::SystemSnapshot> latest_;
  workload::ProcessScanner scanner_;      // scheduler thread only
  workload::Classifier classifier_;       // scheduler thread only
  bool scan_failure_reported_ = false;
  mutable std::mutex workload_mutex_;
  std::shared_ptr<const workload::WorkloadResult> workload_latest_;
  std::vector<safety::ExternalTool> tools_latest_;  // effective active owners; guarded by workload_mutex_
  safety::ToolInventoryReport tool_inventory_latest_; // package/unit inventory; guarded by workload_mutex_
  std::chrono::steady_clock::time_point last_tool_inventory_probe_{};
  std::chrono::steady_clock::time_point last_tool_unit_probe_{};
  policy::PolicyEngine engine_;                     // scheduler thread only
  mutable std::mutex policy_mutex_;                 // guards user_mode_, policy_latest_
  policy::UserMode user_mode_;
  std::shared_ptr<const policy::PolicyDecision> policy_latest_;
  // ML advisor (optional). Declared before scheduler_ so its background build thread is joined first.
  std::unique_ptr<ml::Predictor> predictor_;
  std::atomic<int> ml_mode_{0};                              // 0 off, 1 observe, 2 suggest
  std::shared_ptr<const ml::FeatureVector> features_latest_;  // guarded by workload_mutex_
  std::optional<double> cpu_hw_max_mhz_;
  // Telemetry (local SQLite). Declared before scheduler_ so the scheduler stops first.
  std::unique_ptr<db::TelemetryStore> telemetry_;          // internally synchronised
  std::mutex telemetry_mutex_;                             // guards row_builder_ (replaced on delete)
  std::unique_ptr<telemetry::RowBuilder> row_builder_;
  std::atomic<bool> telemetry_on_{false};
  std::atomic<std::int64_t> last_insert_unix_{0};
  std::string session_id_, device_hash_;
  // scheduler-thread-only state
  monitor::Clock::time_point last_telemetry_{};
  control::ControlTotals last_totals_{};
  std::optional<Profile> last_profile_;
  int telemetry_failures_ = 0;
  monitor::Clock::time_point telemetry_backoff_until_{};
  std::unique_ptr<ml::Adapter> adapter_;                    // after predictor_ and telemetry_: destroyed first
  safety::HardwareLimits limits_;                   // read-only after startup
  control::FileStateWriter writer_;                 // the only code that writes /sys and /proc/sys
  std::unique_ptr<safety::RollbackManager> rollback_;
  std::unique_ptr<control::ControlManager> controls_;  // declared after rollback_: destroyed first
  std::unique_ptr<safety::ThermalGuard> guard_;     // independent thermal safety thread
  Scheduler scheduler_;
  std::unique_ptr<ipc::IpcServer> server_;
};

}  // namespace opt
