#pragma once
// Long-term tier: one row per (hour, workload, policy), produced by rolling detailed rows up.

#include <optional>
#include <string>
#include <vector>

#include "database/database.hpp"

namespace opt::db {

struct SummaryRow {
  std::int64_t hour_ts = 0;
  std::string workload, policy;
  std::int64_t samples = 0;
  double duration_s = 0;
  std::optional<double> avg_cpu, avg_gpu, avg_temp, peak_temp, avg_power, energy, avg_latency, avg_fps, avg_frame_time, avg_reward;
  std::int64_t successes = 0, failures = 0;
};

constexpr std::int64_t kHour = 3600;

/// Aggregates detailed rows with from <= ts < upto (whole hours) into the summaries table.
/// Idempotent per bucket (INSERT OR REPLACE). Returns the number of buckets written.
Result<int> rollup(Database&, std::int64_t from, std::int64_t upto);

Result<std::vector<SummaryRow>> query_summaries(Database&, int limit);
/// Same aggregation computed on the fly from detailed rows newer than `since`.
Result<std::vector<SummaryRow>> query_detail_grouped(Database&, std::int64_t since);

std::string format_history(const std::vector<SummaryRow>&, const std::string& title);

}  // namespace opt::db
