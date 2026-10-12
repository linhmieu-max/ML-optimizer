#include "database/telemetry.hpp"

#include <algorithm>
#include <ctime>

#include "core/logging.hpp"

namespace opt::db {
namespace fs = std::filesystem;
namespace {

constexpr int kSchemaVersion = 2;  // 2: ml_samples

const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS telemetry (
  id INTEGER PRIMARY KEY,
  ts INTEGER NOT NULL,
  session_id TEXT NOT NULL,
  device_id_hash TEXT NOT NULL,
  kernel_version TEXT,
  interval_s REAL,
  workload TEXT NOT NULL,
  workload_confidence REAL,
  cpu_utilization REAL, cpu_frequency REAL,
  gpu_utilization REAL, gpu_frequency REAL,
  ram_used REAL, swap_activity REAL,
  psi_cpu REAL, psi_memory REAL, psi_io REAL,
  temperature REAL, power REAL, energy_delta REAL,
  selected_policy TEXT NOT NULL, policy_parameters TEXT,
  fps REAL, frame_time REAL, latency REAL, io_throughput REAL,
  safety_action TEXT, rollback INTEGER NOT NULL DEFAULT 0, reward REAL,
  success INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS idx_telemetry_ts ON telemetry(ts);
CREATE TABLE IF NOT EXISTS summaries (
  hour_ts INTEGER NOT NULL, workload TEXT NOT NULL, policy TEXT NOT NULL,
  samples INTEGER NOT NULL, duration_s REAL,
  avg_cpu REAL, avg_gpu REAL, avg_temp REAL, peak_temp REAL, avg_power REAL, energy REAL,
  avg_latency REAL, avg_fps REAL, avg_frame_time REAL, avg_reward REAL,
  successes INTEGER NOT NULL, failures INTEGER NOT NULL,
  PRIMARY KEY (hour_ts, workload, policy)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS ml_samples (
  id INTEGER PRIMARY KEY,
  ts INTEGER NOT NULL,
  workload INTEGER NOT NULL, wl_confidence REAL,
  features BLOB NOT NULL, pfeatures BLOB NOT NULL,
  rule_profile INTEGER, applied_profile INTEGER NOT NULL,
  auto_source INTEGER NOT NULL, controllers_apply INTEGER NOT NULL,
  reward REAL, success INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ml_ts ON ml_samples(ts);
)sql";

const char* kInsert =
    "INSERT INTO telemetry (ts, session_id, device_id_hash, kernel_version, interval_s, workload, workload_confidence, "
    "cpu_utilization, cpu_frequency, gpu_utilization, gpu_frequency, ram_used, swap_activity, psi_cpu, psi_memory, psi_io, "
    "temperature, power, energy_delta, selected_policy, policy_parameters, fps, frame_time, latency, io_throughput, safety_action, "
    "rollback, reward, success) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29)";

void move_aside(const fs::path& file) {
  std::error_code ec;
  const std::string suffix = ".corrupt-" + std::to_string(std::time(nullptr));
  for (const char* ext : {"", "-wal", "-shm"})
    if (fs::exists(file.string() + ext, ec)) fs::rename(file.string() + ext, file.string() + suffix + ext, ec);
  // keep at most 3 quarantined databases so a recurring fault cannot fill the disk
  std::vector<fs::path> old;
  for (const auto& e : fs::directory_iterator(file.parent_path(), ec))
    if (e.path().filename().string().starts_with(file.filename().string() + ".corrupt-") && e.path().extension() != ".shm" &&
        e.path().string().find("-wal") == std::string::npos && e.path().string().find("-shm") == std::string::npos)
      old.push_back(e.path());
  std::sort(old.begin(), old.end());
  while (old.size() > 3) {
    for (const char* ext : {"", "-wal", "-shm"}) fs::remove(old.front().string() + ext, ec);
    old.erase(old.begin());
  }
}

}  // namespace

Result<std::unique_ptr<TelemetryStore>> TelemetryStore::open(const fs::path& file, RetentionPolicy retention) {
  std::unique_ptr<TelemetryStore> s(new TelemetryStore());
  s->path_ = file;
  s->retention_ = retention;
  auto opened = Database::open(file);
  if (!opened && file != ":memory:") {
    log::warn("telemetry", "database unusable, starting a fresh one", {{"error", describe(opened.error())}, {"file", file.string()}});
    move_aside(file);
    opened = Database::open(file);
  }
  if (!opened) return fail(opened.error());
  s->db_ = std::move(*opened);
  if (auto r = s->create_schema(); !r) return fail(r.error());
  return s;
}

Result<void> TelemetryStore::create_schema() {
  auto ver = db_->scalar_int("PRAGMA user_version");
  if (!ver) return fail(ver.error());
  if (*ver > kSchemaVersion) return fail(make_error(Errc::Unsupported, "telemetry database was written by a newer version"));
  if (auto r = db_->exec(kSchema); !r) return r;
  return db_->exec("PRAGMA user_version=" + std::to_string(kSchemaVersion));
}

void TelemetryStore::set_retention(RetentionPolicy r) {
  std::lock_guard<std::mutex> lock(mutex_);
  retention_ = r;
}

Result<void> TelemetryStore::insert(const TelemetryRow& r) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!insert_) {
    auto st = db_->prepare(kInsert);
    if (!st) return fail(st.error());
    insert_.emplace(std::move(*st));
  }
  Statement& s = *insert_;
  s.reset();
  s.bind(1, r.ts).bind(2, r.session_id).bind(3, r.device_id_hash).bind(4, r.kernel_version).bind(5, r.interval_s).bind(6, r.workload)
      .bind(7, r.workload_confidence).bind(8, r.cpu_utilization).bind(9, r.cpu_frequency).bind(10, r.gpu_utilization)
      .bind(11, r.gpu_frequency).bind(12, r.ram_used).bind(13, r.swap_activity).bind(14, r.psi_cpu).bind(15, r.psi_memory)
      .bind(16, r.psi_io).bind(17, r.temperature).bind(18, r.power).bind(19, r.energy_delta).bind(20, r.selected_policy)
      .bind(21, r.policy_parameters).bind(22, r.fps).bind(23, r.frame_time).bind(24, r.latency).bind(25, r.io_throughput)
      .bind(26, r.safety_action).bind(27, r.rollback).bind(28, r.reward).bind(29, static_cast<std::int64_t>(r.success ? 1 : 0));
  auto step = s.step();
  s.reset();
  if (!step) return fail(step.error());
  return {};
}

Result<std::int64_t> TelemetryStore::bytes_locked() {
  auto pages = db_->scalar_int("PRAGMA page_count");
  auto size = db_->scalar_int("PRAGMA page_size");
  if (!pages || !size) return fail(pages ? size.error() : pages.error());
  std::int64_t total = *pages * *size;
  std::error_code ec;
  if (path_ != ":memory:") {
    const auto wal = fs::file_size(path_.string() + "-wal", ec);
    if (!ec) total += static_cast<std::int64_t>(wal);
  }
  return total;
}

Result<int> TelemetryStore::exec_count(const std::string& sql, std::int64_t a, bool bind) {
  auto st = db_->prepare(sql);
  if (!st) return fail(st.error());
  if (bind) st->bind(1, a);
  if (auto r = st->step(); !r) return fail(r.error());
  return static_cast<int>(db_->changes());
}

Result<MaintenanceReport> TelemetryStore::maintain(std::int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  MaintenanceReport rep;
  auto fail_with = [&](const Error& e) -> Result<MaintenanceReport> {
    (void)db_->exec("ROLLBACK");
    return fail(e);
  };
  if (auto r = db_->exec("BEGIN IMMEDIATE"); !r) return fail(r.error());

  // 1. Roll complete hours up (watermark in meta makes this idempotent and gap-free).
  const std::int64_t upto = (now / kHour) * kHour;
  std::int64_t wm = 0;
  {
    auto st = db_->prepare("SELECT value FROM meta WHERE key='rollup_watermark'");
    if (!st) return fail_with(st.error());
    auto row = st->step();
    if (!row) return fail_with(row.error());
    if (*row) wm = std::atoll(st->col_text(0).c_str());
  }
  if (wm < upto) {
    auto n = rollup(*db_, wm, upto);
    if (!n) return fail_with(n.error());
    rep.buckets_rolled = *n;
    auto st = db_->prepare("INSERT OR REPLACE INTO meta(key, value) VALUES('rollup_watermark', ?1)");
    if (!st) return fail_with(st.error());
    st->bind(1, std::to_string(upto));
    if (auto r = st->step(); !r) return fail_with(r.error());
  }
  const std::int64_t watermark = std::max(wm, upto);

  // 2. Retention. Detailed rows are only deleted once they have been rolled up.
  const std::int64_t detail_cut = std::min(watermark, now - static_cast<std::int64_t>(retention_.detail_days) * 86400);
  if (auto n = exec_count("DELETE FROM telemetry WHERE ts < ?1", detail_cut, true); n) rep.pruned_detail = *n; else return fail_with(n.error());
  const std::int64_t summary_cut = now - static_cast<std::int64_t>(retention_.summary_days) * 86400;
  if (auto n = exec_count("DELETE FROM summaries WHERE hour_ts < ?1", summary_cut, true); n) rep.pruned_summaries = *n; else return fail_with(n.error());
  // Training samples: age limit and row cap (never part of the rollup).
  if (auto n = exec_count("DELETE FROM ml_samples WHERE ts < ?1", now - static_cast<std::int64_t>(retention_.ml_days) * 86400, true); n) rep.pruned_detail += *n;
  else return fail_with(n.error());
  if (auto cnt = db_->scalar_int("SELECT COUNT(*) FROM ml_samples"); cnt && *cnt > retention_.ml_max_rows) {
    if (auto n = exec_count("DELETE FROM ml_samples WHERE id IN (SELECT id FROM ml_samples ORDER BY ts LIMIT ?1)", *cnt - retention_.ml_max_rows, true); n) rep.pruned_detail += *n;
  }
  if (auto r = db_->exec("COMMIT"); !r) return fail_with(r.error());

  // 3. Hard size cap: drop the oldest detailed rows first, then the oldest summaries.
  (void)db_->scalar_text("PRAGMA wal_checkpoint(TRUNCATE)");
  (void)db_->exec("PRAGMA incremental_vacuum");
  for (int round = 0; round < 6; ++round) {
    auto bytes = bytes_locked();
    if (!bytes || *bytes <= retention_.max_db_bytes) break;
    // Aim for 80% of the cap so the next inserts do not immediately exceed it again.
    const double excess = 1.0 - (0.8 * static_cast<double>(retention_.max_db_bytes)) / static_cast<double>(*bytes);
    auto rows = db_->scalar_int("SELECT COUNT(*) FROM telemetry");
    if (!rows) break;
    int n = 0;
    if (auto ml = db_->scalar_int("SELECT COUNT(*) FROM ml_samples"); ml && *ml > 0) {  // training samples are the most disposable
      auto r = exec_count("DELETE FROM ml_samples WHERE id IN (SELECT id FROM ml_samples ORDER BY ts LIMIT ?1)", std::max<std::int64_t>(100, *ml / 2), true);
      if (r) n = *r;
    } else if (*rows > 0) {
      const double ratio = std::clamp(excess, 0.10, 0.90);
      const std::int64_t drop = std::max<std::int64_t>(100, static_cast<std::int64_t>(static_cast<double>(*rows) * ratio));
      auto r = exec_count("DELETE FROM telemetry WHERE id IN (SELECT id FROM telemetry ORDER BY ts LIMIT ?1)", drop, true);
      if (r) n = *r;
    } else {
      auto r = exec_count("DELETE FROM summaries WHERE rowid IN (SELECT rowid FROM summaries ORDER BY hour_ts LIMIT ?1)", 100, true);
      if (r) n = *r;
    }
    if (n == 0) break;
    rep.trimmed_for_size += n;
    (void)db_->scalar_text("PRAGMA wal_checkpoint(TRUNCATE)");
    (void)db_->exec("PRAGMA incremental_vacuum");
  }
  return rep;
}

Result<StoreStats> TelemetryStore::stats() {
  std::lock_guard<std::mutex> lock(mutex_);
  StoreStats s;
  auto d = db_->scalar_int("SELECT COUNT(*) FROM telemetry");
  auto m = db_->scalar_int("SELECT COUNT(*) FROM summaries");
  auto b = bytes_locked();
  if (!d || !m || !b) return fail(!d ? d.error() : (!m ? m.error() : b.error()));
  s.detailed_rows = *d;
  s.summary_rows = *m;
  s.db_bytes = *b;
  auto st = db_->prepare("SELECT MIN(ts), MAX(ts) FROM telemetry");
  if (st) {
    auto row = st->step();
    if (row && *row && !st->col_null(0)) {
      s.oldest_ts = st->col_int(0);
      s.newest_ts = st->col_int(1);
    }
  }
  return s;
}

Result<void> TelemetryStore::delete_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  insert_.reset();
  insert_ml_.reset();
  for (const char* sql : {"DELETE FROM telemetry", "DELETE FROM summaries", "DELETE FROM meta", "DELETE FROM ml_samples"})
    if (auto r = db_->exec(sql); !r) return r;
  if (auto r = db_->exec("VACUUM"); !r) return r;
  (void)db_->scalar_text("PRAGMA wal_checkpoint(TRUNCATE)");
  return {};
}

Result<void> TelemetryStore::insert_ml_sample(const MlSampleRow& r) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!insert_ml_) {
    auto st = db_->prepare("INSERT INTO ml_samples (ts, workload, wl_confidence, features, pfeatures, rule_profile, applied_profile, auto_source, "
                           "controllers_apply, reward, success) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)");
    if (!st) return fail(st.error());
    insert_ml_.emplace(std::move(*st));
  }
  Statement& s = *insert_ml_;
  s.reset();
  s.bind(1, r.ts).bind(2, static_cast<std::int64_t>(r.workload)).bind(3, r.wl_confidence).bind_blob(4, r.features).bind_blob(5, r.pfeatures)
      .bind(6, static_cast<std::int64_t>(r.rule_profile)).bind(7, static_cast<std::int64_t>(r.applied_profile))
      .bind(8, static_cast<std::int64_t>(r.auto_source ? 1 : 0)).bind(9, static_cast<std::int64_t>(r.controllers_apply ? 1 : 0)).bind(10, r.reward)
      .bind(11, static_cast<std::int64_t>(r.success ? 1 : 0));
  auto step = s.step();
  s.reset();
  if (!step) return fail(step.error());
  return {};
}

Result<std::vector<MlSampleRow>> TelemetryStore::ml_samples(std::int64_t ts_min, std::int64_t ts_max, int limit) {
  std::lock_guard<std::mutex> lock(mutex_);
  // the newest `limit` rows, returned oldest first
  auto st = db_->prepare("SELECT ts, workload, wl_confidence, features, pfeatures, rule_profile, applied_profile, auto_source, controllers_apply, reward, "
                         "success FROM (SELECT * FROM ml_samples WHERE ts >= ?1 AND ts <= ?2 ORDER BY ts DESC LIMIT ?3) ORDER BY ts ASC");
  if (!st) return fail(st.error());
  st->bind(1, ts_min).bind(2, ts_max).bind(3, static_cast<std::int64_t>(limit));
  std::vector<MlSampleRow> out;
  while (true) {
    auto row = st->step();
    if (!row) return fail(row.error());
    if (!*row) break;
    MlSampleRow r;
    r.ts = st->col_int(0);
    r.workload = static_cast<int>(st->col_int(1));
    r.wl_confidence = st->col_double(2);
    r.features = st->col_text_blob(3);
    r.pfeatures = st->col_text_blob(4);
    r.rule_profile = static_cast<int>(st->col_int(5));
    r.applied_profile = static_cast<int>(st->col_int(6));
    r.auto_source = st->col_int(7) != 0;
    r.controllers_apply = st->col_int(8) != 0;
    if (!st->col_null(9)) r.reward = st->col_double(9);
    r.success = st->col_int(10) != 0;
    out.push_back(std::move(r));
  }
  return out;
}

Result<std::int64_t> TelemetryStore::ml_sample_count(std::int64_t since_ts) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto st = db_->prepare("SELECT COUNT(*) FROM ml_samples WHERE ts >= ?1");
  if (!st) return fail(st.error());
  st->bind(1, since_ts);
  auto row = st->step();
  if (!row) return fail(row.error());
  return st->col_int(0);
}

Result<std::vector<MlInsight>> TelemetryStore::ml_insights(std::int64_t since_ts) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto st = db_->prepare("SELECT workload, applied_profile, COUNT(*), SUM(1 - success), SUM(controllers_apply), AVG(reward) FROM ml_samples "
                         "WHERE ts >= ?1 AND applied_profile BETWEEN 0 AND 2 GROUP BY workload, applied_profile ORDER BY workload, applied_profile");
  if (!st) return fail(st.error());
  st->bind(1, since_ts);
  std::vector<MlInsight> out;
  while (true) {
    auto row = st->step();
    if (!row) return fail(row.error());
    if (!*row) break;
    MlInsight i;
    i.workload = static_cast<int>(st->col_int(0));
    i.profile = static_cast<int>(st->col_int(1));
    i.samples = st->col_int(2);
    i.failures = st->col_int(3);
    i.applied_samples = st->col_int(4);
    if (!st->col_null(5)) i.avg_reward = st->col_double(5);
    out.push_back(i);
  }
  return out;
}

Result<std::vector<SummaryRow>> TelemetryStore::recent_summaries(int limit) {
  std::lock_guard<std::mutex> lock(mutex_);
  return query_summaries(*db_, limit);
}

Result<std::vector<SummaryRow>> TelemetryStore::recent_detail(std::int64_t since_ts) {
  std::lock_guard<std::mutex> lock(mutex_);
  return query_detail_grouped(*db_, since_ts);
}

}  // namespace opt::db
