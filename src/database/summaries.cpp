#include "database/summaries.hpp"

#include <cstdio>
#include <ctime>

namespace opt::db {
namespace {

const char* kAggregate =
    "SELECT (ts/3600)*3600 AS hour, workload, selected_policy, COUNT(*), COALESCE(SUM(interval_s),0), AVG(cpu_utilization), "
    "AVG(gpu_utilization), AVG(temperature), MAX(temperature), AVG(power), SUM(energy_delta), AVG(latency), AVG(fps), "
    "AVG(frame_time), AVG(reward), COALESCE(SUM(success),0), COUNT(*)-COALESCE(SUM(success),0) FROM telemetry ";

std::optional<double> opt_col(const Statement& s, int i) { return s.col_null(i) ? std::nullopt : std::optional<double>(s.col_double(i)); }

SummaryRow read_row(const Statement& s) {
  SummaryRow r;
  r.hour_ts = s.col_int(0);
  r.workload = s.col_text(1);
  r.policy = s.col_text(2);
  r.samples = s.col_int(3);
  r.duration_s = s.col_double(4);
  r.avg_cpu = opt_col(s, 5); r.avg_gpu = opt_col(s, 6); r.avg_temp = opt_col(s, 7); r.peak_temp = opt_col(s, 8);
  r.avg_power = opt_col(s, 9); r.energy = opt_col(s, 10); r.avg_latency = opt_col(s, 11); r.avg_fps = opt_col(s, 12);
  r.avg_frame_time = opt_col(s, 13); r.avg_reward = opt_col(s, 14);
  r.successes = s.col_int(15);
  r.failures = s.col_int(16);
  return r;
}

std::string f(const std::optional<double>& v, int prec = 1) {
  if (!v) return "-";
  char b[32];
  std::snprintf(b, sizeof b, "%.*f", prec, *v);
  return b;
}

}  // namespace

Result<int> rollup(Database& d, std::int64_t from, std::int64_t upto) {
  if (upto <= from) return 0;
  const std::string sql =
      "INSERT OR REPLACE INTO summaries (hour_ts, workload, policy, samples, duration_s, avg_cpu, avg_gpu, avg_temp, peak_temp, "
      "avg_power, energy, avg_latency, avg_fps, avg_frame_time, avg_reward, successes, failures) " + std::string(kAggregate) +
      "WHERE ts >= ?1 AND ts < ?2 GROUP BY hour, workload, selected_policy";
  auto st = d.prepare(sql);
  if (!st) return fail(st.error());
  st->bind(1, from).bind(2, upto);
  if (auto r = st->step(); !r) return fail(r.error());
  return static_cast<int>(d.changes());
}

Result<std::vector<SummaryRow>> query_summaries(Database& d, int limit) {
  auto st = d.prepare(
      "SELECT hour_ts, workload, policy, samples, duration_s, avg_cpu, avg_gpu, avg_temp, peak_temp, avg_power, energy, avg_latency, "
      "avg_fps, avg_frame_time, avg_reward, successes, failures FROM summaries ORDER BY hour_ts DESC, workload, policy LIMIT ?1");
  if (!st) return fail(st.error());
  st->bind(1, static_cast<std::int64_t>(limit));
  std::vector<SummaryRow> out;
  while (true) {
    auto row = st->step();
    if (!row) return fail(row.error());
    if (!*row) break;
    out.push_back(read_row(*st));
  }
  return out;
}

Result<std::vector<SummaryRow>> query_detail_grouped(Database& d, std::int64_t since) {
  auto st = d.prepare(std::string(kAggregate) + "WHERE ts >= ?1 GROUP BY hour, workload, selected_policy ORDER BY hour DESC, workload, selected_policy");
  if (!st) return fail(st.error());
  st->bind(1, since);
  std::vector<SummaryRow> out;
  while (true) {
    auto row = st->step();
    if (!row) return fail(row.error());
    if (!*row) break;
    out.push_back(read_row(*st));
  }
  return out;
}

std::string format_history(const std::vector<SummaryRow>& rows, const std::string& title) {
  std::string s = title + "\n";
  if (rows.empty()) return s + "  (no data yet)\n";
  s += "  hour (UTC)        workload        policy        n  cpu%  gpu%  avg C  peak C  W     J      reward  ok/fail\n";
  for (const auto& r : rows) {
    const std::time_t t = static_cast<std::time_t>(r.hour_ts);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char when[24];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:00", &tm);
    char line[200];
    std::snprintf(line, sizeof line, "  %-16s  %-14s  %-12s %3lld %5s %5s %6s %7s %5s %6s %7s  %lld/%lld\n", when, r.workload.c_str(),
                  r.policy.c_str(), static_cast<long long>(r.samples), f(r.avg_cpu, 0).c_str(), f(r.avg_gpu, 0).c_str(), f(r.avg_temp).c_str(),
                  f(r.peak_temp).c_str(), f(r.avg_power).c_str(), f(r.energy, 0).c_str(), f(r.avg_reward, 2).c_str(),
                  static_cast<long long>(r.successes), static_cast<long long>(r.failures));
    s += line;
  }
  return s;
}

}  // namespace opt::db
