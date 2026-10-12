#include "workload/game_detector.hpp"

namespace opt::workload {

Evidence detect_game(const DetectContext& c) {
  Evidence e;
  auto steam_app = [](const ProcessInfo& p) { return (p.cmd_flags & (kCmdSteamApp | kCmdProton)) != 0; };
  auto launcher_ancestor = [&](const ProcessInfo& p) {
    return c.procs.has_ancestor(p, [](const ProcessInfo& a) { return is_launcher(a); });
  };

  if (const auto* p = c.busiest([&](const ProcessInfo& x) { return steam_app(x) && *x.cpu_pct >= kActiveCpuPct; }))
    e.add(0.40, "Steam/Proton game process " + p->comm + " (pid " + std::to_string(p->pid) + ")");

  if (const auto* p = c.busiest([](const ProcessInfo& x) { return is_wine_game_candidate(x) && *x.cpu_pct >= 10.0; })) {
    e.add(0.25, "Wine process " + p->comm + " active");
    if (launcher_ancestor(*p)) e.add(0.15, "started under a game launcher");
  }
  if (const auto* p = c.busiest([](const ProcessInfo& x) { return is_emulator(x) && *x.cpu_pct >= 10.0; }))
    e.add(0.30, "emulator " + p->comm + " active");
  if (c.count_active([](const ProcessInfo& x) { return x.comm.starts_with("gamescope"); }, 2.0) > 0)
    e.add(0.20, "gamescope compositor active");

  const bool launcher_running = c.count([](const ProcessInfo& x) { return x.comm == "steam" || x.comm == "lutris" || x.comm == "heroic"; }) > 0;
  const double gpu = c.gpu_busy();
  if (gpu >= 60.0) {
    auto candidate = [](const ProcessInfo& x) {
      return *x.cpu_pct >= 30.0 && !is_compositor(x) && !is_browser(x) && !is_compile_worker(x) && !is_render(x) &&
             !is_encoder(x) && !is_archiver(x) && !is_vm(x) && !is_media_player(x) && !is_server_daemon(x);
    };
    if (const auto* p = c.busiest(candidate)) {
      e.add(0.25, "GPU " + std::to_string(static_cast<int>(gpu)) + "% busy with dominant process " + p->comm);
      if (launcher_running) e.add(0.15, "game launcher running");
    }
  }
  if (e.score > 0 && c.count([](const ProcessInfo& x) { return x.comm == "gamemoded"; }) > 0) e.add(0.10, "GameMode daemon present (not controlled)");
  return e;
}

}  // namespace opt::workload
