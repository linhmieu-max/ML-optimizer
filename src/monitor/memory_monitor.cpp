#include "monitor/memory_monitor.hpp"

#include "core/sysfs.hpp"

namespace opt::monitor {
using namespace sysfs;

Result<MemorySample> MemoryMonitor::sample(Clock::time_point now) {
  auto text = read_text(root_ / "proc/meminfo");
  if (!text) return fail(make_error(Errc::NotFound, "/proc/meminfo unreadable"));

  MemorySample m;
  bool have_avail = false;
  for (auto line : split_lines(*text)) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    const auto key = line.substr(0, colon);
    auto f = split_ws(line.substr(colon + 1));
    if (f.empty()) continue;
    const auto v = parse_u64(f[0]).value_or(0);
    if (key == "MemTotal") m.total_kb = v;
    else if (key == "MemFree") m.free_kb = v;
    else if (key == "MemAvailable") { m.available_kb = v; have_avail = true; }
    else if (key == "Buffers") m.buffers_kb = v;
    else if (key == "Cached") m.cached_kb = v;
    else if (key == "SwapTotal") m.swap_total_kb = v;
    else if (key == "SwapFree") m.swap_free_kb = v;
    else if (key == "Dirty") m.dirty_kb = v;
    else if (key == "Writeback") m.writeback_kb = v;
  }
  if (m.total_kb == 0) return fail(make_error(Errc::Parse, "MemTotal missing in /proc/meminfo"));
  if (!have_avail) m.available_kb = m.free_kb + m.buffers_kb + m.cached_kb;  // kernels < 3.14

  if (auto vm = read_text(root_ / "proc/vmstat", 1 << 20)) {
    std::optional<std::uint64_t> in, out;
    for (auto line : split_lines(*vm)) {
      auto f = split_ws(line);
      if (f.size() != 2) continue;
      if (f[0] == "pswpin") in = parse_u64(f[1]);
      else if (f[0] == "pswpout") out = parse_u64(f[1]);
    }
    if (in && out) {
      if (prev_ && *in >= prev_->in && *out >= prev_->out) {
        const double dt = std::chrono::duration<double>(now - prev_->at).count();
        if (dt > 0) {
          m.swap_in_pages_s = static_cast<double>(*in - prev_->in) / dt;
          m.swap_out_pages_s = static_cast<double>(*out - prev_->out) / dt;
        }
      }
      prev_ = Prev{*in, *out, now};
    }
  }
  return m;
}

}  // namespace opt::monitor
