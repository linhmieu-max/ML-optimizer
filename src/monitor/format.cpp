#include <algorithm>
#include <cstdio>

#include "monitor/system_monitor.hpp"

namespace opt::monitor {
namespace {

std::string f(double v, int prec = 1) {
  char b[48];
  std::snprintf(b, sizeof b, "%.*f", prec, v);
  return b;
}
std::string gib(std::uint64_t kb) { return f(static_cast<double>(kb) / 1048576.0) + " GiB"; }
std::string mbs(double bps) { return f(bps / 1e6) + " MB/s"; }
std::string psi(const std::optional<PsiResource>& r) {
  if (!r) return "n/a";
  std::string s = r->some ? f(r->some->avg10, 2) : "-";
  if (r->full) s += "/" + f(r->full->avg10, 2);
  return s;
}

std::string cpu_line(const CpuSample& c) {
  std::string s = c.utilization_pct ? f(*c.utilization_pct) + "% used" : "utilization pending";
  s += ", " + std::to_string(c.online_cpus) + " CPUs";
  if (c.freq_avg_mhz) s += ", " + f(*c.freq_avg_mhz, 0) + " MHz avg (" + f(*c.freq_min_mhz, 0) + "-" + f(*c.freq_max_mhz, 0) + ")";
  if (!c.governor.empty()) s += ", governor " + c.governor;
  if (!c.epp.empty()) s += ", EPP " + c.epp;
  if (c.turbo_enabled) s += *c.turbo_enabled ? ", turbo on" : ", turbo off";
  if (c.load1) s += ", load " + f(*c.load1, 2) + " " + f(c.load5.value_or(0), 2) + " " + f(c.load15.value_or(0), 2);
  return s;
}

std::string mem_line(const MemorySample& m) {
  std::string s = gib(m.total_kb - m.available_kb) + " / " + gib(m.total_kb) + " used (" + f(m.used_pct(), 0) +
                  "%), available " + gib(m.available_kb) + ", swap " + gib(m.swap_total_kb - m.swap_free_kb) + " / " +
                  gib(m.swap_total_kb);
  if (m.swap_in_pages_s) s += ", swap io " + f(*m.swap_in_pages_s + *m.swap_out_pages_s, 0) + " pages/s";
  return s;
}

std::string thermal_line(const ThermalSample& t) {
  const ThermalZone* h = t.hottest();
  if (!h) return std::to_string(t.zones.size()) + " zones, none readable";
  std::string s = "max " + f(*h->temp_c) + " C (" + h->type + "), " + std::to_string(t.zones.size()) + " zones";
  if (auto c = h->critical_c()) s += ", " + f(*c - *h->temp_c) + " C below critical (" + f(*c, 0) + ")";
  return s;
}

std::string power_line(const PowerSample& p) {
  std::string s = p.on_ac ? (*p.on_ac ? "AC" : "battery") : "source unknown";
  if (p.battery_pct) s += ", battery " + std::to_string(*p.battery_pct) + "%";
  for (const auto& ps : p.supplies)
    if (ps.type == "Battery" && !ps.status.empty()) { s += " (" + ps.status + ")"; break; }
  if (p.rapl_pkg_w) s += ", package " + f(*p.rapl_pkg_w) + " W";
  return s;
}

std::string gpu_line(const GpuDevice& g) {
  std::string s = g.card + " " + g.driver;
  if (g.cur_mhz) s += ", " + std::to_string(*g.cur_mhz) + " MHz";
  if (g.max_mhz) s += " (max " + std::to_string(*g.max_mhz) + ")";
  if (g.busy_pct) s += ", " + f(*g.busy_pct, 0) + "% busy" + (g.busy_is_estimate ? " (est. from RC6)" : "");
  if (g.temp_c) s += ", " + f(*g.temp_c, 0) + " C";
  if (g.power_w) s += ", " + f(*g.power_w) + " W";
  if (!g.note.empty()) s += ", " + g.note;
  return s;
}

std::string io_line(const IoSample& io) {
  if (!io.total_read_bps) return std::to_string(io.devices.size()) + " devices, rates pending";
  std::string s = "read " + mbs(*io.total_read_bps) + ", write " + mbs(*io.total_write_bps);
  const IoDevice* busiest = nullptr;
  for (const auto& d : io.devices)
    if (d.util_pct && (!busiest || *d.util_pct > *busiest->util_pct)) busiest = &d;
  if (busiest) s += ", busiest " + busiest->name + " " + f(*busiest->util_pct, 0) + "%";
  return s;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> format_summary(const SystemSnapshot& s) {
  std::vector<std::pair<std::string, std::string>> out;
  auto add = [&](const char* k, std::string v) { out.emplace_back(k, std::move(v)); };
  add("CPU load", s.cpu ? cpu_line(*s.cpu) : "n/a");
  add("Memory", s.memory ? mem_line(*s.memory) : "n/a");
  add("PSI avg10", s.psi ? "cpu " + psi(s.psi->cpu) + " | memory " + psi(s.psi->memory) + " | io " + psi(s.psi->io) + "  (some/full %)" : "n/a");
  add("Thermal now", s.thermal ? thermal_line(*s.thermal) : "n/a");
  add("Power now", s.power ? power_line(*s.power) : "n/a");
  std::string gpu = "n/a";
  if (s.gpu && !s.gpu->devices.empty()) {
    gpu.clear();
    for (const auto& g : s.gpu->devices) gpu += (gpu.empty() ? "" : "; ") + gpu_line(g);
  }
  add("GPU now", gpu);
  add("I/O now", s.io ? io_line(*s.io) : "n/a");
  return out;
}

std::string format_snapshot(const SystemSnapshot& s) {
  std::string out = "sample #" + std::to_string(s.sequence) + "\n";
  for (const auto& [k, v] : format_summary(s)) out += k + ": " + v + "\n";
  if (s.cpu && !s.cpu->per_cpu_pct.empty()) {
    out += "\nper-CPU %:";
    for (double p : s.cpu->per_cpu_pct) out += " " + f(p, 0);
    out += "\n";
  }
  if (s.thermal) {
    out += "\nthermal zones:\n";
    for (const auto& z : s.thermal->zones) {
      out += "  " + z.name + " " + z.type + ": " + (z.temp_c ? f(*z.temp_c) + " C" : std::string("unreadable"));
      if (!z.trips.empty()) {
        out += "  trips:";
        for (const auto& t : z.trips) out += " " + t.type + "=" + f(t.temp_c, 0);
      }
      out += "\n";
    }
    if (!s.thermal->cooling.empty()) {
      out += "cooling devices:\n";
      for (const auto& c : s.thermal->cooling)
        out += "  " + c.name + " " + c.type + ": " + (c.cur_state ? std::to_string(*c.cur_state) : "?") + "/" +
               (c.max_state ? std::to_string(*c.max_state) : "?") + "\n";
    }
  }
  if (s.power) {
    out += "\npower supplies:\n";
    for (const auto& p : s.power->supplies) {
      out += "  " + p.name + " " + p.type;
      if (!p.status.empty()) out += " " + p.status;
      if (p.capacity_pct) out += " " + std::to_string(*p.capacity_pct) + "%";
      if (p.online) out += *p.online ? " online" : " offline";
      if (p.power_w) out += " " + f(*p.power_w) + " W";
      out += "\n";
    }
  }
  if (s.io && !s.io->devices.empty()) {
    out += "\nblock devices:\n";
    for (const auto& d : s.io->devices) {
      out += "  " + d.name + " [" + (d.scheduler.empty() ? "?" : d.scheduler) + (d.rotational ? ", rotational" : ", non-rotational") + "]";
      if (d.read_bps) out += " r " + mbs(*d.read_bps) + " w " + mbs(*d.write_bps) + " util " + f(*d.util_pct, 0) + "%";
      out += "\n";
    }
  }
  if (s.gpu && !s.gpu->devfreq.empty()) {
    out += "\ndevfreq:\n";
    for (const auto& d : s.gpu->devfreq)
      out += "  " + d.name + " " + (d.cur_mhz ? f(*d.cur_mhz, 0) + " MHz" : std::string("?")) + " governor " + d.governor + "\n";
  }
  return out;
}

}  // namespace opt::monitor
