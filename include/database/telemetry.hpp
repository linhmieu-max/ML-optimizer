#pragma once
// Two-tier local telemetry store (SQLite). Local only: nothing here talks to a network.
//   tier 1  telemetry  : detailed rows (one per sample), kept `detail_days`
//   tier 2  summaries  : hourly roll-ups per (workload, policy), kept `summary_days`
// Growth is bounded by retention AND by a hard size cap. Safe to call from several threads.

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "database/database.hpp"
#include "database/summaries.hpp"

namespace opt::db {

struct TelemetryRow {
  std::int64_t ts = 0;  // unix seconds
  std::string session_id, device_id_hash, kernel_version;
  std::optional<double> interval_s;
  std::string workload;
  std::optional<double> workload_confidence;
  std::optional<double> cpu_utilization, cpu_frequency;        // %, MHz
  std::optional<double> gpu_utilization, gpu_frequency;        // %, MHz
  std::optional<double> ram_used, swap_activity;               // %, pages/s
  std::optional<double> psi_cpu, psi_memory, psi_io;           // some avg10, %
  std::optional<double> temperature, power, energy_delta;      // C, W, J
  std::string selected_policy, policy_parameters;
  std::optional<double> fps, frame_time, latency;              // no source yet: NULL
  std::optional<double> io_throughput;                         // bytes/s
  std::string safety_action;
  std::int64_t rollback = 0;
  std::optional<double> reward;
  bool success = true;
};

/// One training example recorded next to the telemetry (features are quantised to one byte each).
struct MlSampleRow {
  std::int64_t ts = 0;
  int workload = 0;                       // rule classifier class index (the "teacher")
  double wl_confidence = 0;
  std::string features;                   // kFeatureCount bytes
  std::string pfeatures;                  // kProfileFeatureCount bytes
  int rule_profile = -1;                  // what the rule table alone chose (-1 = not AUTO)
  int applied_profile = 1;                // 0 powersave, 1 balanced, 2 performance
  bool auto_source = false;               // the engine (not the user/safety) chose the profile
  bool controllers_apply = false;         // controllers really wrote, so the outcome depends on the profile
  std::optional<double> reward;
  bool success = true;
};

struct MlInsight {
  int workload = 0, profile = 0;
  std::int64_t samples = 0, failures = 0, applied_samples = 0;
  std::optional<double> avg_reward;
};

struct RetentionPolicy {
  int detail_days = 7;
  int summary_days = 365;
  std::int64_t max_db_bytes = 64LL * 1024 * 1024;
  int ml_days = 30;                    // training samples
  std::int64_t ml_max_rows = 300000;
};

struct StoreStats {
  std::int64_t detailed_rows = 0, summary_rows = 0, db_bytes = 0;
  std::optional<std::int64_t> oldest_ts, newest_ts;
};

struct MaintenanceReport {
  int buckets_rolled = 0;
  int pruned_detail = 0, pruned_summaries = 0, trimmed_for_size = 0;
  bool any() const { return buckets_rolled || pruned_detail || pruned_summaries || trimmed_for_size; }
};

class TelemetryStore {
 public:
  /// A corrupt database is moved aside (".corrupt-<time>", at most 3 kept) and replaced by a fresh one.
  static Result<std::unique_ptr<TelemetryStore>> open(const std::filesystem::path& file, RetentionPolicy retention);

  Result<void> insert(const TelemetryRow&);
  /// Roll up complete hours, enforce retention and the size cap, checkpoint the WAL.
  Result<MaintenanceReport> maintain(std::int64_t now_unix);
  Result<StoreStats> stats();
  /// Removes every row (both tiers) and compacts the file.
  Result<void> delete_all();
  Result<std::vector<SummaryRow>> recent_summaries(int limit);
  Result<std::vector<SummaryRow>> recent_detail(std::int64_t since_ts);

  // ---- local-learning samples (same database, deleted together with the telemetry) ----
  Result<void> insert_ml_sample(const MlSampleRow&);
  /// Samples with ts_min <= ts <= ts_max, oldest first, at most the newest `limit`.
  Result<std::vector<MlSampleRow>> ml_samples(std::int64_t ts_min, std::int64_t ts_max, int limit);
  Result<std::int64_t> ml_sample_count(std::int64_t since_ts);
  Result<std::vector<MlInsight>> ml_insights(std::int64_t since_ts);
  void set_retention(RetentionPolicy);
  const std::filesystem::path& path() const { return path_; }

 private:
  TelemetryStore() = default;
  Result<void> create_schema();
  Result<std::int64_t> bytes_locked();
  Result<int> exec_count(const std::string& sql, std::int64_t a = 0, bool bind = false);

  mutable std::mutex mutex_;
  std::filesystem::path path_;
  std::unique_ptr<Database> db_;
  RetentionPolicy retention_;
  std::optional<Statement> insert_;
  std::optional<Statement> insert_ml_;
};

}  // namespace opt::db
