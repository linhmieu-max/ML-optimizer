#include "monitor/gpu_monitor.hpp"

#include <algorithm>
#include <initializer_list>

#include "core/sysfs.hpp"

namespace opt::monitor {
namespace fs = std::filesystem;
using namespace sysfs;
namespace {

std::optional<long long> first_int(const fs::path& base, std::initializer_list<const char*> rels) {
  for (const char* r : rels)
    if (auto v = read_int(base / r)) return v;
  return std::nullopt;
}

std::optional<int> as_int(std::optional<long long> v) {
  return v ? std::optional<int>(static_cast<int>(*v)) : std::nullopt;
}

/// amdgpu pp_dpm_sclk: "1: 800Mhz *" -> 800
std::optional<int> parse_active_dpm(const fs::path& p) {
  auto t = read_text(p);
  if (!t) return std::nullopt;
  for (auto line : split_lines(*t)) {
    if (line.find('*') == std::string_view::npos) continue;
    for (auto tok : split_ws(line)) {
      const auto pos = tok.find("Mhz");
      if (pos != std::string_view::npos)
        if (auto v = parse_u64(tok.substr(0, pos))) return static_cast<int>(*v);
    }
  }
  return std::nullopt;
}

void read_hwmon(const fs::path& dev, GpuDevice& g) {
  for (const auto& h : list_dir(dev / "hwmon", "hwmon")) {
    if (auto t = read_int(h / "temp1_input")) g.temp_c = static_cast<double>(*t) / 1000.0;
    if (auto p = first_int(h, {"power1_average", "power1_input"})) g.power_w = static_cast<double>(*p) / 1e6;
    if (g.temp_c || g.power_w) return;
  }
}

}  // namespace

Result<GpuSample> GpuMonitor::sample(Clock::time_point now) {
  GpuSample s;
  for (const auto& card : list_dir(root_ / "sys/class/drm", "card")) {
    const std::string name = card.filename().string();
    if (!all_digits(name.substr(4))) continue;  // skip connectors (card0-eDP-1)
    GpuDevice g;
    g.card = name;
    std::error_code ec;
    const auto drv = fs::read_symlink(card / "device/driver", ec);
    g.driver = ec ? "unknown" : drv.filename().string();

    if (g.driver == "i915") {
      g.cur_mhz = as_int(first_int(card, {"gt_cur_freq_mhz", "gt/gt0/rps_cur_freq_mhz"}));
      g.act_mhz = as_int(first_int(card, {"gt_act_freq_mhz", "gt/gt0/rps_act_freq_mhz"}));
      g.min_mhz = as_int(first_int(card, {"gt_min_freq_mhz", "gt/gt0/rps_min_freq_mhz"}));
      g.max_mhz = as_int(first_int(card, {"gt_max_freq_mhz", "gt/gt0/rps_max_freq_mhz"}));
      // No direct busy counter in sysfs: time NOT in RC6 (GPU sleep state) is an upper-bound load estimate.
      if (auto rc6 = first_int(card, {"power/rc6_residency_ms", "gt/gt0/rc6_residency_ms"}); rc6 && *rc6 >= 0) {
        const auto cur = static_cast<std::uint64_t>(*rc6);
        if (auto p = prev_rc6_.find(name); p != prev_rc6_.end() && cur >= p->second.ms) {
          const double dt_ms = std::chrono::duration<double, std::milli>(now - p->second.at).count();
          if (dt_ms > 0) {
            g.busy_pct = std::clamp(100.0 * (1.0 - static_cast<double>(cur - p->second.ms) / dt_ms), 0.0, 100.0);
            g.busy_is_estimate = true;
          }
        }
        prev_rc6_[name] = {cur, now};
      }
    } else if (g.driver == "xe") {
      const fs::path f = card / "device/tile0/gt0/freq0";
      g.cur_mhz = as_int(read_int(f / "cur_freq"));
      g.act_mhz = as_int(read_int(f / "act_freq"));
      g.min_mhz = as_int(read_int(f / "min_freq"));
      g.max_mhz = as_int(read_int(f / "max_freq"));
    } else if (g.driver == "amdgpu") {
      if (auto b = read_int(card / "device/gpu_busy_percent")) g.busy_pct = static_cast<double>(*b);
      g.cur_mhz = parse_active_dpm(card / "device/pp_dpm_sclk");
      read_hwmon(card / "device", g);
    } else if (g.driver == "nvidia" || g.driver == "nvidia-drm") {
      g.note = "proprietary NVIDIA driver: no sysfs telemetry (NVML backend not implemented)";
    } else {
      g.note = "no known telemetry interface for this driver";
    }
    s.devices.push_back(std::move(g));
  }

  for (const auto& d : list_dir(root_ / "sys/class/devfreq")) {
    DevFreqDevice df;
    df.name = d.filename().string();
    df.governor = read_text(d / "governor").value_or("");
    auto mhz = [&](const char* f) -> std::optional<double> {
      auto v = read_int(d / f);
      return v ? std::optional<double>(static_cast<double>(*v) / 1e6) : std::nullopt;
    };
    df.cur_mhz = mhz("cur_freq");
    df.min_mhz = mhz("min_freq");
    df.max_mhz = mhz("max_freq");
    s.devfreq.push_back(std::move(df));
  }

  if (s.devices.empty() && s.devfreq.empty()) return fail(make_error(Errc::Unsupported, "no GPU interfaces"));
  return s;
}

}  // namespace opt::monitor
