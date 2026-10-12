#include "monitor/thermal_monitor.hpp"

#include "core/sysfs.hpp"

namespace opt::monitor {
namespace fs = std::filesystem;
using namespace sysfs;

Result<ThermalSample> ThermalMonitor::sample() {
  const fs::path th = root_ / "sys/class/thermal";
  const auto zone_dirs = list_dir(th, "thermal_zone");
  if (zone_dirs.empty()) return fail(make_error(Errc::Unsupported, "no thermal zones"));

  ThermalSample s;
  for (const auto& z : zone_dirs) {
    ThermalZone zone;
    zone.name = z.filename().string();
    zone.type = read_text(z / "type").value_or("?");
    if (auto t = read_int(z / "temp")) zone.temp_c = static_cast<double>(*t) / 1000.0;

    auto it = trip_cache_.find(zone.name);
    if (it == trip_cache_.end()) {
      std::vector<ThermalTrip> trips;
      for (int i = 0; i < 64; ++i) {
        const auto base = z / ("trip_point_" + std::to_string(i));
        auto temp = read_int(base.string() + "_temp");
        if (!temp) {
          if (!path_exists(base.string() + "_temp")) break;  // end of trip list
          continue;                                          // present but unreadable
        }
        if (*temp < -100000) continue;  // disabled placeholder (-274000)
        trips.push_back({static_cast<double>(*temp) / 1000.0, read_text(base.string() + "_type").value_or("?")});
      }
      it = trip_cache_.emplace(zone.name, std::move(trips)).first;
    }
    zone.trips = it->second;
    s.zones.push_back(std::move(zone));
  }

  for (const auto& c : list_dir(th, "cooling_device")) {
    CoolingDevice cd;
    cd.name = c.filename().string();
    cd.type = read_text(c / "type").value_or("?");
    cd.cur_state = read_int(c / "cur_state");
    cd.max_state = read_int(c / "max_state");
    s.cooling.push_back(std::move(cd));
  }
  return s;
}

}  // namespace opt::monitor
