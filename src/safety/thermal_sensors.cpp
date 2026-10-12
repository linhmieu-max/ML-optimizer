#include "safety/thermal_sensors.hpp"

#include <algorithm>
#include <cctype>

#include "core/sysfs.hpp"

namespace opt::safety {
namespace fs = std::filesystem;
using namespace sysfs;
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool starts_any(const std::string& s, std::initializer_list<std::string_view> p) {
  return std::any_of(p.begin(), p.end(), [&](std::string_view x) { return s.starts_with(x); });
}
bool has_any(const std::string& s, std::initializer_list<std::string_view> p) {
  return std::any_of(p.begin(), p.end(), [&](std::string_view x) { return s.find(x) != std::string::npos; });
}

SensorKind classify(const std::string& type, bool has_limit) {
  const std::string t = lower(type);
  if (starts_any(t, {"iwlwifi", "nvme", "bat", "ath", "wifi", "ucsi", "drivetemp"})) return SensorKind::Other;
  if (has_any(t, {"x86_pkg_temp", "cpu", "soc", "k10temp", "tctl", "coretemp", "package"})) return SensorKind::Cpu;
  if (has_any(t, {"gpu", "i915", "amdgpu", "nouveau", "radeon"})) return SensorKind::Gpu;
  return has_limit ? SensorKind::Platform : SensorKind::Other;
}

}  // namespace

std::vector<ThermalSensor> read_thermal_sensors(const fs::path& root) {
  std::vector<ThermalSensor> out;

  for (const auto& z : list_dir(root / "sys/class/thermal", "thermal_zone")) {
    const auto temp = read_int(z / "temp");
    if (!temp) continue;
    ThermalSensor s;
    s.name = read_text(z / "type").value_or(z.filename().string());
    s.source = "thermal_zone";
    s.temp_c = static_cast<double>(*temp) / 1000.0;
    for (int i = 0; i < 32; ++i) {
      const std::string base = (z / ("trip_point_" + std::to_string(i))).string();
      if (!path_exists(base + "_temp")) break;
      const auto t = read_int(base + "_temp");
      const auto type = read_text(base + "_type").value_or("");
      if (!t || *t < -100000 || (type != "hot" && type != "critical")) continue;
      const double c = static_cast<double>(*t) / 1000.0;
      if (!s.limit_c || c < *s.limit_c) {
        s.limit_c = c;
        s.limit_source = "kernel trip (" + type + ")";
      }
    }
    s.kind = classify(s.name, s.limit_c.has_value());
    out.push_back(std::move(s));
  }

  for (const auto& h : list_dir(root / "sys/class/hwmon", "hwmon")) {
    const std::string hname = read_text(h / "name").value_or(h.filename().string());
    const std::string hl = lower(hname);
    for (const auto& f : list_dir(h, "temp")) {
      const std::string fn = f.filename().string();
      if (!fn.ends_with("_input")) continue;
      const std::string base = fn.substr(0, fn.size() - 6);
      const auto temp = read_int(f);
      if (!temp) continue;
      ThermalSensor s;
      s.name = hname + ":" + read_text(h / (base + "_label")).value_or(base);
      s.source = "hwmon";
      s.temp_c = static_cast<double>(*temp) / 1000.0;
      if (auto crit = read_int(h / (base + "_crit")); crit && *crit > 0) {
        s.limit_c = static_cast<double>(*crit) / 1000.0;
        s.limit_source = "hardware crit";
      } else if (auto mx = read_int(h / (base + "_max")); mx && *mx > 0) {
        s.limit_c = static_cast<double>(*mx) / 1000.0;
        s.limit_source = "hardware max";
      }
      if (hl == "coretemp" || hl == "k10temp" || hl == "zenpower" || hl == "cpu_thermal" || hl == "cpu-thermal") s.kind = SensorKind::Cpu;
      else if (has_any(hl, {"gpu", "i915", "nouveau", "radeon"})) s.kind = SensorKind::Gpu;
      else if (hl == "acpitz") s.kind = s.limit_c ? SensorKind::Platform : SensorKind::Other;
      else s.kind = SensorKind::Other;
      out.push_back(std::move(s));
    }
  }
  return out;
}

}  // namespace opt::safety
