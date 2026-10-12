#include "monitor/power_monitor.hpp"

#include "core/sysfs.hpp"

namespace opt::monitor {
namespace fs = std::filesystem;
using namespace sysfs;

Result<PowerSample> PowerMonitor::sample(Clock::time_point now) {
  PowerSample s;
  bool have_mains = false, any_online = false;
  std::optional<bool> battery_discharging;

  for (const auto& p : list_dir(root_ / "sys/class/power_supply")) {
    PowerSupply ps;
    ps.name = p.filename().string();
    ps.type = read_text(p / "type").value_or("?");
    ps.status = read_text(p / "status").value_or("");
    if (auto c = read_int(p / "capacity")) ps.capacity_pct = static_cast<int>(*c);
    if (auto o = read_int(p / "online")) ps.online = (*o != 0);
    if (auto pw = read_int(p / "power_now")) {
      ps.power_w = static_cast<double>(*pw) / 1e6;
    } else if (auto cur = read_int(p / "current_now")) {
      if (auto v = read_int(p / "voltage_now")) ps.power_w = static_cast<double>(*cur) * static_cast<double>(*v) / 1e12;
    }
    if (ps.type == "Mains" || ps.type == "USB") {
      have_mains = have_mains || ps.type == "Mains";
      if (ps.online.value_or(false)) any_online = true;
    } else if (ps.type == "Battery") {
      if (!s.battery_pct) s.battery_pct = ps.capacity_pct;
      if (ps.status == "Discharging") battery_discharging = true;
    }
    s.supplies.push_back(std::move(ps));
  }
  if (s.supplies.empty()) return fail(make_error(Errc::Unsupported, "no power_supply devices"));

  if (have_mains || any_online) s.on_ac = any_online;
  else if (battery_discharging) s.on_ac = false;

  // Package power from RAPL energy counters (root only on current kernels).
  double watts = 0;
  bool have_watts = false;
  for (const auto& d : list_dir(root_ / "sys/class/powercap", "intel-rapl:")) {
    const std::string name = d.filename().string();
    if (name.find(':', 11) != std::string::npos) continue;  // package domains only
    auto uj = read_int(d / "energy_uj");
    if (!uj || *uj < 0) continue;
    const auto cur = static_cast<std::uint64_t>(*uj);
    auto it = prev_rapl_.find(name);
    if (it != prev_rapl_.end()) {
      const double dt = std::chrono::duration<double>(now - it->second.at).count();
      std::uint64_t delta = 0;
      if (cur >= it->second.uj) {
        delta = cur - it->second.uj;
      } else if (auto range = read_int(d / "max_energy_range_uj"); range && *range > 0) {
        delta = static_cast<std::uint64_t>(*range) - it->second.uj + cur;  // counter wrapped
      }
      if (dt > 0 && delta > 0) {
        watts += static_cast<double>(delta) / 1e6 / dt;
        have_watts = true;
      } else if (dt > 0) {
        have_watts = true;
      }
    }
    prev_rapl_[name] = {cur, now};
  }
  if (have_watts) s.rapl_pkg_w = watts;
  return s;
}

}  // namespace opt::monitor
