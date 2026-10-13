#pragma once
// Plain data produced by the monitors. Every optional means "not available on
// this machine / not yet computable" (e.g. rates need two samples).

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opt::monitor {

using Clock = std::chrono::steady_clock;

struct CpuSample {
  std::optional<double> utilization_pct;  // nullopt on the first sample
  std::vector<double> per_cpu_pct;        // empty on the first sample
  int online_cpus = 0;
  std::optional<double> freq_avg_mhz, freq_min_mhz, freq_max_mhz;
  std::string driver, governor, epp;      // empty when cpufreq is absent
  std::optional<bool> turbo_enabled;
  std::optional<double> load1, load5, load15;
};

struct MemorySample {
  std::uint64_t total_kb = 0, free_kb = 0, available_kb = 0, buffers_kb = 0, cached_kb = 0;
  std::uint64_t swap_total_kb = 0, swap_free_kb = 0, dirty_kb = 0, writeback_kb = 0;
  std::optional<double> swap_in_pages_s, swap_out_pages_s;  // needs two samples
  double used_pct() const { return total_kb ? 100.0 * static_cast<double>(total_kb - available_kb) / static_cast<double>(total_kb) : 0.0; }
};

struct PsiLine {
  double avg10 = 0, avg60 = 0, avg300 = 0;
  std::uint64_t total_us = 0;
};
struct PsiResource {
  std::optional<PsiLine> some, full;
};
struct PsiSample {
  std::optional<PsiResource> cpu, memory, io;
};

struct ThermalTrip {
  double temp_c = 0;
  std::string type;  // critical, hot, passive, active...
};
struct ThermalZone {
  std::string name, type;
  std::optional<double> temp_c;  // nullopt if the zone could not be read
  std::vector<ThermalTrip> trips;
  std::optional<double> critical_c() const {
    std::optional<double> best;
    for (const auto& t : trips)
      if (t.type == "critical" && (!best || t.temp_c < *best)) best = t.temp_c;
    return best;
  }
};
struct CoolingDevice {
  std::string name, type;
  std::optional<long long> cur_state, max_state;
};
struct ThermalSample {
  std::vector<ThermalZone> zones;
  std::vector<CoolingDevice> cooling;
  const ThermalZone* hottest() const {
    const ThermalZone* h = nullptr;
    for (const auto& z : zones)
      if (z.temp_c && (!h || *z.temp_c > *h->temp_c)) h = &z;
    return h;
  }
};

struct PowerSupply {
  std::string name, type, status;
  std::optional<int> capacity_pct;
  std::optional<bool> online;
  std::optional<double> power_w;
};
struct PowerSample {
  std::vector<PowerSupply> supplies;
  std::optional<bool> on_ac;
  std::optional<int> battery_pct;
  std::optional<double> rapl_pkg_w;  // needs two samples and root
};

struct IoDevice {
  std::string name, scheduler;
  bool rotational = false;
  bool in_totals = true;  // false for dm-/md (would double count)
  std::optional<double> read_bps, write_bps, read_iops, write_iops, util_pct;
};
struct IoSample {
  std::vector<IoDevice> devices;
  std::optional<double> total_read_bps, total_write_bps;
};

struct GpuDevice {
  std::string card, driver, note;
  std::optional<int> cur_mhz, act_mhz, min_mhz, max_mhz;
  std::optional<double> busy_pct;
  bool busy_is_estimate = false;  // i915: derived from RC6 residency
  std::optional<double> temp_c, power_w;
};
struct DevFreqDevice {
  std::string name, governor;
  std::optional<double> cur_mhz, min_mhz, max_mhz;
};
struct GpuSample {
  std::vector<GpuDevice> devices;
  std::vector<DevFreqDevice> devfreq;
};

}  // namespace opt::monitor
