#include "ml/feature_vector.hpp"

#include <algorithm>
#include <cmath>

#include "workload/detector_common.hpp"

namespace opt::ml {
using namespace workload;
namespace {
float clip(double v) { return static_cast<float>(std::clamp(std::isfinite(v) ? v : 0.0, 0.0, 1.0)); }
double log_scale(double v, double cap) { return std::log1p(std::max(0.0, v)) / std::log1p(cap); }
}  // namespace

const char* feature_name(int i) {
  static const char* names[kFeatureCount] = {
      "cpu_util", "cpu_freq_ratio", "load", "ram_used", "swap", "psi_cpu", "psi_mem", "psi_io", "io_read", "io_write",
      "gpu_busy", "audio", "compositor", "session", "system", "top_proc", "active_procs", "listen_ports",
      "browser_cpu", "compile_cpu", "compile_workers", "compile_orch", "player_present", "player_cpu", "container_cpu",
      "vm_cpu", "server_cpu", "archiver_cpu", "encoder_cpu", "render_cpu", "copy_tool", "game_cpu", "launcher", "browser_procs"};
  return i >= 0 && i < kFeatureCount ? names[i] : "?";
}

FeatureVector extract_features(const ProcessSnapshot& ps, const monitor::SystemSnapshot* sys, const FeatureEnv& env) {
  FeatureVector f{};
  if (sys) {
    if (sys->cpu) {
      if (sys->cpu->utilization_pct) f[F_CPU_UTIL] = clip(*sys->cpu->utilization_pct / 100.0);
      if (sys->cpu->freq_avg_mhz && env.cpu_hw_max_mhz && *env.cpu_hw_max_mhz > 0) f[F_CPU_FREQ_RATIO] = clip(*sys->cpu->freq_avg_mhz / *env.cpu_hw_max_mhz);
      if (sys->cpu->load1 && sys->cpu->online_cpus > 0) f[F_LOAD] = clip(*sys->cpu->load1 / sys->cpu->online_cpus);
    }
    if (sys->memory) {
      f[F_RAM_USED] = clip(sys->memory->used_pct() / 100.0);
      if (sys->memory->swap_in_pages_s) f[F_SWAP] = clip(log_scale(*sys->memory->swap_in_pages_s + sys->memory->swap_out_pages_s.value_or(0), 5000));
    }
    if (sys->psi) {
      auto some10 = [](const std::optional<monitor::PsiResource>& r) { return r && r->some ? r->some->avg10 : 0.0; };
      f[F_PSI_CPU] = clip(some10(sys->psi->cpu) / 100.0);
      f[F_PSI_MEM] = clip(some10(sys->psi->memory) / 100.0);
      f[F_PSI_IO] = clip(some10(sys->psi->io) / 100.0);
    }
    if (sys->io) {
      if (sys->io->total_read_bps) f[F_IO_READ] = clip(log_scale(*sys->io->total_read_bps / 1e6, 500));
      if (sys->io->total_write_bps) f[F_IO_WRITE] = clip(log_scale(*sys->io->total_write_bps / 1e6, 500));
    }
    if (sys->gpu) {
      double b = 0;
      for (const auto& g : sys->gpu->devices)
        if (g.busy_pct) b = std::max(b, *g.busy_pct);
      f[F_GPU_BUSY] = clip(b / 100.0);
    }
  }
  f[F_AUDIO] = ps.audio_running.value_or(false) ? 1.0f : 0.0f;
  f[F_LISTEN_PORTS] = clip(log_scale(ps.listen_ports_nonlocal, 50));

  double comp = 0, session = 0, system = 0, top = 0, browser = 0, compile = 0, container = 0, vm = 0, server = 0, archiver = 0,
         encoder = 0, render = 0, player = 0, game = 0;
  int active = 0, workers = 0, browser_procs = 0;
  bool orch = false, player_present = false, copy = false, launcher = false;
  for (const auto& p : ps.procs) {
    if (is_browser(p)) ++browser_procs;
    if (is_media_player(p)) player_present = true;
    if (is_launcher(p) && (p.comm == "steam" || p.comm == "lutris" || p.comm == "heroic")) launcher = true;
    if (!p.cpu_pct) continue;
    const double c = *p.cpu_pct;
    if (c >= kActiveCpuPct) ++active;
    top = std::max(top, c);
    // "session"/"system" measure ordinary applications/services only: processes that belong to a specific
    // category are counted in that category (otherwise a compile would look like a busy desktop session).
    const bool special = is_browser(p) || is_compile_worker(p) || is_compile_orchestrator(p) || is_media_player(p) || is_vm(p) ||
                         is_server_daemon(p) || is_archiver(p) || is_encoder(p) || is_render(p) || is_emulator(p) || is_copy_tool(p) ||
                         is_container_runtime(p) || in_container_cgroup(p) || is_launcher(p) || is_wine_game_candidate(p) ||
                         (p.cmd_flags & (kCmdSteamApp | kCmdProton)) != 0;
    if (is_compositor(p)) comp += c;
    else if (in_user_slice(p) && !special) session += c;
    if (in_system_slice(p) && !special) system += c;
    if (is_browser(p)) browser += c;
    if (is_compile_worker(p)) { compile += c; if (c >= kActiveCpuPct) ++workers; }
    if (is_compile_orchestrator(p)) orch = true;
    if (in_container_cgroup(p)) container += c;
    if (is_vm(p)) vm += c;
    if (is_server_daemon(p)) server += c;
    if (is_archiver(p)) archiver += c;
    if (is_encoder(p)) encoder += c;
    if (is_render(p)) render += c;
    if (is_media_player(p)) player += c;
    if (is_copy_tool(p) && c >= 1.0) copy = true;
    if ((p.cmd_flags & (kCmdSteamApp | kCmdProton)) || is_wine_game_candidate(p) || is_emulator(p) || p.comm.starts_with("gamescope")) game += c;
  }
  f[F_COMPOSITOR] = clip(comp / 100.0);
  f[F_SESSION] = clip(session / 200.0);
  f[F_SYSTEM] = clip(system / 200.0);
  f[F_TOP_PROC] = clip(top / 400.0);
  f[F_ACTIVE_PROCS] = clip(active / 20.0);
  f[F_BROWSER_CPU] = clip(browser / 300.0);
  f[F_COMPILE_CPU] = clip(compile / 300.0);
  f[F_COMPILE_WORKERS] = clip(workers / 8.0);
  f[F_COMPILE_ORCH] = orch ? 1.0f : 0.0f;
  f[F_PLAYER_PRESENT] = player_present ? 1.0f : 0.0f;
  f[F_PLAYER_CPU] = clip(player / 100.0);
  f[F_CONTAINER_CPU] = clip(container / 300.0);
  f[F_VM_CPU] = clip(vm / 300.0);
  f[F_SERVER_CPU] = clip(server / 100.0);
  f[F_ARCHIVER_CPU] = clip(archiver / 300.0);
  f[F_ENCODER_CPU] = clip(encoder / 300.0);
  f[F_RENDER_CPU] = clip(render / 300.0);
  f[F_COPY_TOOL] = copy ? 1.0f : 0.0f;
  f[F_GAME_CPU] = clip(game / 300.0);
  f[F_LAUNCHER] = launcher ? 1.0f : 0.0f;
  f[F_BROWSER_PROCS] = clip(browser_procs / 10.0);
  return f;
}

ProfileFeatures profile_features(const ProfileFeatureInput& in) {
  ProfileFeatures f{};
  if (in.confidence >= 0.3) f[static_cast<std::size_t>(in.cls)] = 1.0f;  // same threshold as the rule table
  f[14] = (in.flags & kHighLoad) ? 1.0f : 0.0f;
  f[15] = (in.flags & kLatencySensitive) ? 1.0f : 0.0f;
  f[16] = (in.flags & kResourceHeavy) ? 1.0f : 0.0f;
  f[17] = in.on_ac ? (*in.on_ac ? 1.0f : 0.0f) : 0.5f;
  f[18] = clip(in.battery_pct.value_or(100) / 100.0);
  f[19] = in.thermal_margin_c ? clip(*in.thermal_margin_c / 60.0) : 0.5f;
  f[20] = clip(in.psi_cpu10 / 100.0);
  f[21] = clip(in.cpu_util / 100.0);
  f[22] = clip((in.aggressiveness - 1) / 3.0);
  return f;
}

}  // namespace opt::ml
