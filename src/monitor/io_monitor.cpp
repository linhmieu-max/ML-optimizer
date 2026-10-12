#include "monitor/io_monitor.hpp"

#include <algorithm>

#include "core/sysfs.hpp"

namespace opt::monitor {
namespace fs = std::filesystem;
using namespace sysfs;

Result<IoSample> IoMonitor::sample(Clock::time_point now) {
  auto text = read_text(root_ / "proc/diskstats", 1 << 20);
  if (!text) return fail(make_error(Errc::Unsupported, "/proc/diskstats unreadable"));

  const double dt = prev_at_ ? std::chrono::duration<double>(now - *prev_at_).count() : 0.0;
  IoSample s;
  std::map<std::string, Raw> cur;
  double tr = 0, tw = 0;
  bool any_rate = false;

  for (auto line : split_lines(*text)) {
    auto f = split_ws(line);
    if (f.size() < 14) continue;
    const std::string name(f[2]);
    if (name.starts_with("loop") || name.starts_with("ram") || name.starts_with("zram")) continue;
    const fs::path dev = root_ / "sys/block" / name;
    if (!path_exists(dev)) continue;  // partitions live below their parent, not in /sys/block

    Raw r{parse_u64(f[3]).value_or(0), parse_u64(f[5]).value_or(0), parse_u64(f[7]).value_or(0),
          parse_u64(f[9]).value_or(0), parse_u64(f[12]).value_or(0)};
    IoDevice d;
    d.name = name;
    d.rotational = read_int(dev / "queue/rotational").value_or(0) == 1;
    d.in_totals = !(name.starts_with("dm-") || name.starts_with("md"));
    if (auto sched = read_text(dev / "queue/scheduler")) {
      const auto a = sched->find('['), b = sched->find(']');
      d.scheduler = (a != std::string::npos && b != std::string::npos && b > a) ? sched->substr(a + 1, b - a - 1) : *sched;
    }
    if (auto p = prev_.find(name); p != prev_.end() && dt > 0 && r.reads >= p->second.reads &&
                                   r.writes >= p->second.writes && r.io_ms >= p->second.io_ms) {
      d.read_bps = static_cast<double>(r.sectors_r - p->second.sectors_r) * 512.0 / dt;
      d.write_bps = static_cast<double>(r.sectors_w - p->second.sectors_w) * 512.0 / dt;
      d.read_iops = static_cast<double>(r.reads - p->second.reads) / dt;
      d.write_iops = static_cast<double>(r.writes - p->second.writes) / dt;
      d.util_pct = std::clamp(static_cast<double>(r.io_ms - p->second.io_ms) / (dt * 1000.0) * 100.0, 0.0, 100.0);
      if (d.in_totals) {
        tr += *d.read_bps;
        tw += *d.write_bps;
        any_rate = true;
      }
    }
    cur[name] = r;
    s.devices.push_back(std::move(d));
  }
  if (any_rate) {
    s.total_read_bps = tr;
    s.total_write_bps = tw;
  }
  prev_ = std::move(cur);
  prev_at_ = now;
  return s;
}

}  // namespace opt::monitor
