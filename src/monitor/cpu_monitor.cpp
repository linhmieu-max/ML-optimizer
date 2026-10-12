#include "monitor/cpu_monitor.hpp"

#include <algorithm>

#include "core/sysfs.hpp"

namespace opt::monitor {
namespace fs = std::filesystem;
using namespace sysfs;

static double pct(std::uint64_t d_idle, std::uint64_t d_total) {
  if (d_total == 0) return 0.0;
  return std::clamp(100.0 * (1.0 - static_cast<double>(d_idle) / static_cast<double>(d_total)), 0.0, 100.0);
}

Result<CpuSample> CpuMonitor::sample() {
  auto stat = read_text(root_ / "proc/stat", 1 << 20);
  if (!stat) return fail(make_error(Errc::NotFound, "/proc/stat unreadable"));

  Times total;
  bool have_total = false;
  std::vector<Times> cpus;
  for (auto line : split_lines(*stat)) {
    auto f = split_ws(line);
    if (f.size() < 5 || !f[0].starts_with("cpu")) continue;
    std::uint64_t sum = 0, idle = 0;
    for (std::size_t i = 1; i < std::min<std::size_t>(f.size(), 9); ++i) {  // user..steal (guest is in user)
      const auto v = parse_u64(f[i]).value_or(0);
      sum += v;
      if (i == 4 || i == 5) idle += v;  // idle + iowait
    }
    if (f[0] == "cpu") {
      total = {sum, idle};
      have_total = true;
    } else {
      cpus.push_back({sum, idle});
    }
  }
  if (!have_total) return fail(make_error(Errc::Parse, "no aggregate cpu line in /proc/stat"));

  CpuSample s;
  s.online_cpus = static_cast<int>(cpus.size());
  if (prev_total_ && total.total > prev_total_->total) {
    s.utilization_pct = pct(total.idle - prev_total_->idle, total.total - prev_total_->total);
    if (prev_cpu_.size() == cpus.size()) {
      for (std::size_t i = 0; i < cpus.size(); ++i) {
        const auto dt = cpus[i].total >= prev_cpu_[i].total ? cpus[i].total - prev_cpu_[i].total : 0;
        const auto di = cpus[i].idle >= prev_cpu_[i].idle ? cpus[i].idle - prev_cpu_[i].idle : 0;
        s.per_cpu_pct.push_back(pct(di, dt));
      }
    }
  }
  prev_total_ = total;
  prev_cpu_ = std::move(cpus);

  // Frequencies: scaling_cur_freq is world-readable; offline CPUs simply lack it.
  const fs::path cpu_dir = root_ / "sys/devices/system/cpu";
  double sum = 0, mn = 0, mx = 0;
  int n = 0;
  for (const auto& c : list_dir(cpu_dir, "cpu")) {
    if (!all_digits(c.filename().string().substr(3))) continue;
    if (auto khz = read_int(c / "cpufreq/scaling_cur_freq")) {
      const double mhz = static_cast<double>(*khz) / 1000.0;
      sum += mhz;
      mn = n ? std::min(mn, mhz) : mhz;
      mx = n ? std::max(mx, mhz) : mhz;
      ++n;
    }
  }
  if (n > 0) {
    s.freq_avg_mhz = sum / n;
    s.freq_min_mhz = mn;
    s.freq_max_mhz = mx;
  }

  fs::path pol = cpu_dir / "cpufreq/policy0";
  if (!path_exists(pol)) pol = cpu_dir / "cpu0/cpufreq";
  s.driver = read_text(pol / "scaling_driver").value_or("");
  s.governor = read_text(pol / "scaling_governor").value_or("");
  s.epp = read_text(pol / "energy_performance_preference").value_or("");
  if (auto nt = read_int(cpu_dir / "intel_pstate/no_turbo")) s.turbo_enabled = (*nt == 0);
  else if (auto b = read_int(cpu_dir / "cpufreq/boost")) s.turbo_enabled = (*b != 0);

  if (auto la = read_text(root_ / "proc/loadavg")) {
    auto f = split_ws(*la);
    if (f.size() >= 3) {
      s.load1 = parse_double(f[0]);
      s.load5 = parse_double(f[1]);
      s.load15 = parse_double(f[2]);
    }
  }
  return s;
}

}  // namespace opt::monitor
