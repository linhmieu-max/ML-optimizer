#include "workload/simple_rules.hpp"

#include <algorithm>

namespace opt::workload {
namespace {
std::string pct(double v) { return std::to_string(static_cast<int>(v)) + "%"; }
std::string mbps(double v) { return std::to_string(static_cast<int>(v)) + " MB/s"; }
double compositor_cpu(const DetectContext& c) { return c.active_cpu([](const ProcessInfo& p) { return is_compositor(p); }); }
}  // namespace

Evidence detect_render(const DetectContext& c) {
  Evidence e;
  auto match = [](const ProcessInfo& p) { return is_render(p); };
  const auto* p = c.busiest([&](const ProcessInfo& x) { return match(x) && *x.cpu_pct >= 10.0; });
  if (!p) return e;
  e.add(0.55, "renderer " + p->comm + " active");
  if (c.cpu_util() >= 60.0 || c.gpu_busy() >= 60.0) e.add(0.20, "sustained CPU/GPU load");
  if (c.active_cpu(match) >= 150.0) e.add(0.10, "multi-threaded render");
  return e;
}

Evidence detect_encode(const DetectContext& c) {
  Evidence e;
  auto match = [](const ProcessInfo& p) { return is_encoder(p); };
  const auto* p = c.busiest([&](const ProcessInfo& x) { return match(x) && *x.cpu_pct >= 10.0; });
  if (!p) return e;
  e.add(0.55, "encoder " + p->comm + " active");
  if (c.cpu_util() >= 50.0) e.add(0.20, "sustained CPU load");
  if (c.active_cpu(match) >= 150.0) e.add(0.10, "multi-threaded encode");
  return e;
}

Evidence detect_archive(const DetectContext& c) {
  Evidence e;
  const auto* p = c.busiest([](const ProcessInfo& x) { return is_archiver(x) && *x.cpu_pct >= 5.0; });
  if (!p) return e;
  e.add(0.55, "archiver " + p->comm + " active");
  if (c.io_read_mbps() + c.io_write_mbps() >= 20.0) e.add(0.20, "disk I/O " + mbps(c.io_read_mbps() + c.io_write_mbps()));
  if (c.cpu_util() >= 30.0) e.add(0.10, "CPU " + pct(c.cpu_util()));
  return e;
}

Evidence detect_file_copy(const DetectContext& c) {
  Evidence e;
  const double r = c.io_read_mbps(), w = c.io_write_mbps();
  const bool both = std::min(r, w) >= 20.0;
  const auto* tool = c.busiest([](const ProcessInfo& x) { return is_copy_tool(x) && (x.state == 'R' || x.state == 'D' || *x.cpu_pct >= 1.0); });
  if (tool) e.add(0.45, "copy tool " + tool->comm + " running");
  if (both) e.add(tool ? 0.35 : 0.40, "concurrent read/write " + mbps(r) + " / " + mbps(w));
  if ((tool || both) && c.cpu_util() < 50.0) e.add(0.10, "I/O-bound (CPU " + pct(c.cpu_util()) + ")");
  return e;
}

Evidence detect_vm(const DetectContext& c) {
  Evidence e;
  auto match = [](const ProcessInfo& p) { return is_vm(p); };
  const auto* p = c.busiest([&](const ProcessInfo& x) { return match(x) && *x.cpu_pct >= 5.0; });
  if (!p) return e;
  e.add(0.60, "hypervisor process " + p->comm + " active");
  if (c.active_cpu(match) >= 50.0) e.add(0.20, "vCPU load " + pct(c.active_cpu(match)));
  if (p->rss_kb >= 1024ull * 1024) e.add(0.10, "large guest memory footprint");
  return e;
}

Evidence detect_server(const DetectContext& c) {
  Evidence e;
  const auto* p = c.busiest([](const ProcessInfo& x) { return is_server_daemon(x) && *x.cpu_pct >= 3.0; });
  if (!p) return e;
  e.add(0.35, "server daemon " + p->comm + " active");
  if (c.procs.listen_ports_nonlocal > 0) e.add(0.20, std::to_string(c.procs.listen_ports_nonlocal) + " non-loopback listening sockets");
  if (in_system_slice(*p)) e.add(0.15, "runs as a systemd system service");
  if (compositor_cpu(c) < 3.0) e.add(0.15, "no interactive desktop activity");
  return e;
}

Evidence detect_video(const DetectContext& c) {
  Evidence e;
  const bool audio = c.audio_running();
  if (const auto* p = c.busiest([](const ProcessInfo& x) { return is_media_player(x); })) {
    e.add(0.40, "media player " + p->comm + " running");
    if (*p->cpu_pct >= 3.0) e.add(0.20, "player decoding (CPU " + pct(*p->cpu_pct) + ")");
    if (audio) e.add(0.20, "audio stream running");
    if (c.gpu_busy() >= 5.0) e.add(0.10, "GPU active");
  } else if (audio && c.active_cpu([](const ProcessInfo& x) { return is_browser(x); }) >= kActiveCpuPct && c.gpu_busy() >= 10.0) {
    e.add(0.35, "browser with audio stream and GPU activity (possible video)");
  }
  return e;
}

Evidence detect_desktop(const DetectContext& c) {
  Evidence e;
  auto comp = [](const ProcessInfo& p) { return is_compositor(p); };
  if (c.count(comp) == 0) return e;
  e.add(0.20, "compositor present");
  const double cc = compositor_cpu(c);
  if (cc >= 3.0) e.add(0.35, "compositor active (" + pct(cc) + ")");
  if (cc >= 8.0) e.add(0.15, "high UI activity");
  const double session = c.active_cpu([&](const ProcessInfo& p) { return in_user_slice(p) && !comp(p); });
  if (session >= kActiveCpuPct) e.add(0.15, "session applications active (" + pct(session) + ")");
  return e;
}

Evidence detect_background(const DetectContext& c) {
  Evidence e;
  const double sys = c.active_cpu([](const ProcessInfo& p) { return in_system_slice(p); });
  if (sys < kActiveCpuPct) return e;
  const double base = std::min(0.7, sys / 50.0);
  const double scaled = compositor_cpu(c) < 3.0 ? base : base * 0.3;
  e.add(scaled, "system services active (" + pct(sys) + ")");
  return e;
}

Evidence detect_idle(const DetectContext& c) {
  Evidence e;
  const double comp = compositor_cpu(c);
  const double io = c.io_read_mbps() + c.io_write_mbps();
  // Any non-UI process using >= 2% of a core argues against "idle".
  const double busy = c.active_cpu([](const ProcessInfo& p) { return !is_compositor(p) && *p.cpu_pct >= 2.0; });
  double s = 0.9 - c.cpu_util() / 50.0 - busy / 60.0 - std::max(0.0, comp - 3.0) / 10.0 - io / 50.0 - c.psi_cpu10() / 50.0;
  s = std::clamp(s, 0.0, 0.9);
  if (s > 0.0) e.add(s, "low CPU/PSI/I-O, quiet UI");
  return e;
}

}  // namespace opt::workload
