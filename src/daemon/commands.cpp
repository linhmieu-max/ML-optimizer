#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <array>
#include <vector>

#include "core/version.hpp"
#include "daemon/daemon.hpp"
#include "core/sysfs.hpp"
#include "policy/profiles.hpp"
#include "telemetry/ids.hpp"

namespace opt {
namespace {

enum class Access { Read, Admin };

struct CommandSpec {
  std::string_view name;
  std::string_view usage;
  Access access;
  bool admin_if_args;  // read-only without arguments, admin when arguments are given
  int phase;           // 0 = implemented now, otherwise the phase that delivers it
  ipc::Response (Daemon::*fn)(const ipc::Request&);
};

}  // namespace

// The table lives in a function so it can name private members through Daemon.
#define OPT_COMMANDS(X)                                                                            \
  X("ping", "ping", Access::Read, false, 0, &Daemon::cmd_ping)                                     \
  X("version", "version", Access::Read, false, 0, &Daemon::cmd_version)                            \
  X("help", "help", Access::Read, false, 0, &Daemon::cmd_help)                                     \
  X("status", "status", Access::Read, false, 0, &Daemon::cmd_status)                               \
  X("detect", "detect", Access::Read, false, 0, &Daemon::cmd_detect)                               \
  X("set-log-level", "set-log-level <trace|debug|info|warn|error|safety>  [admin]", Access::Admin, \
    false, 0, &Daemon::cmd_set_log_level)                                                          \
  X("reload-config", "reload-config  [admin]", Access::Admin, false, 0, &Daemon::cmd_reload_config) \
  X("monitor", "monitor", Access::Read, false, 0, &Daemon::cmd_monitor)                            \
  X("workload", "workload", Access::Read, false, 0, &Daemon::cmd_workload)                         \
  X("profile", "profile [powersave|balanced|performance|auto] [--lock|--unlock]  [admin when setting]", \
    Access::Read, true, 0, &Daemon::cmd_profile)                                                   \
  X("policy", "policy", Access::Read, false, 0, &Daemon::cmd_policy)                               \
  X("reset", "reset  [admin]  (profile -> auto, roll back all changes)", Access::Admin, false, 0, &Daemon::cmd_reset) \
  X("safety", "safety", Access::Read, false, 0, &Daemon::cmd_safety)                               \
  X("controllers", "controllers [off|dry-run|apply]  [admin when setting]", Access::Read, true, 0,  \
    &Daemon::cmd_controllers)                                                                      \
  X("history", "history [N]", Access::Read, false, 0, &Daemon::cmd_history)                         \
  X("telemetry", "telemetry [on|off|delete --yes]  [admin when changing]", Access::Read, true, 0,   \
    &Daemon::cmd_telemetry)                                                                        \
  X("model", "model [off|observe|suggest]  [admin when setting]", Access::Read, true, 0,            \
    &Daemon::cmd_model)                                                                            \
  X("training", "training [on|off|run|reset-local]  [admin when changing]", Access::Read, true, 0,   \
    &Daemon::cmd_training)                                                                         \
  X("conflicts", "conflicts", Access::Read, false, 0, &Daemon::cmd_conflicts)

namespace {
#define OPT_ROW(n, u, a, ai, p, f) CommandSpec{n, u, a, ai, p, f},
const std::array kCommands = {OPT_COMMANDS(OPT_ROW)};
#undef OPT_ROW

const CommandSpec* find_command(std::string_view name) {
  for (const auto& c : kCommands)
    if (c.name == name) return &c;
  return nullptr;
}
}  // namespace

bool Daemon::is_admin(const ipc::PeerCred& peer) const {
  if (peer.uid == 0) return true;
  const std::string group = config_snapshot().admin_group;

  std::vector<char> buf(4096);
  struct group grp{};
  struct group* gres = nullptr;
  if (::getgrnam_r(group.c_str(), &grp, buf.data(), buf.size(), &gres) != 0 || !gres) return false;
  const gid_t admin_gid = gres->gr_gid;

  struct passwd pw{};
  struct passwd* pres = nullptr;
  std::vector<char> pbuf(4096);
  if (::getpwuid_r(peer.uid, &pw, pbuf.data(), pbuf.size(), &pres) != 0 || !pres) return false;
  if (pres->pw_gid == admin_gid) return true;

  std::vector<gid_t> groups(32);
  int n = static_cast<int>(groups.size());
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (::getgrouplist(pres->pw_name, pres->pw_gid, groups.data(), &n) >= 0) {
      groups.resize(static_cast<std::size_t>(n));
      return std::find(groups.begin(), groups.end(), admin_gid) != groups.end();
    }
    groups.resize(static_cast<std::size_t>(n));
  }
  return false;
}

ipc::Response Daemon::handle(const ipc::Request& req, const ipc::PeerCred& peer) {
  const CommandSpec* cmd = find_command(req.command);
  if (!cmd) return ipc::error_response(Errc::InvalidArgument, "unknown command '" + req.command + "' (try: help)");

  // Authorization comes first, so unprivileged callers learn nothing about admin commands.
  const bool needs_admin = cmd->access == Access::Admin || (cmd->admin_if_args && !req.args.empty());
  if (needs_admin && !is_admin(peer)) {
    log::warn("ipc", "denied privileged command",
              {{"command", req.command}, {"uid", std::to_string(peer.uid)}, {"pid", std::to_string(peer.pid)}});
    return ipc::error_response(Errc::PermissionDenied,
                               "'" + req.command + "' requires root or membership of the '" +
                                   config_snapshot().admin_group + "' group");
  }
  if (cmd->phase > 0 || !cmd->fn)
    return ipc::error_response(Errc::Unsupported, "'" + req.command + "' is not available yet (planned for Phase " +
                                                      std::to_string(cmd->phase) + ")");
  log::debug("ipc", "command", {{"command", req.command}, {"uid", std::to_string(peer.uid)}});
  return (this->*(cmd->fn))(req);
}

ipc::Response Daemon::cmd_ping(const ipc::Request&) { return ipc::ok_response("pong\n"); }

ipc::Response Daemon::cmd_version(const ipc::Request&) {
  return ipc::ok_response(std::string("ml-optimizerd ") + kVersion + "\nprotocol " +
                          std::to_string(kProtocolVersion) + "\nphase " + std::to_string(kCurrentPhase) + "\n");
}

ipc::Response Daemon::cmd_help(const ipc::Request&) {
  std::string out = "commands (Phase " + std::to_string(kCurrentPhase) + "):\n";
  for (const auto& c : kCommands) {
    out += "  ";
    out += c.usage;
    if (c.phase > 0) out += "   (Phase " + std::to_string(c.phase) + ")";
    out += '\n';
  }
  out += "  packages                   local CLI: detect native package manager and optional packages\n";
  out += "  install <sensors|thermald>  local CLI: optional allowlisted install after typed confirmation\n";
  return ipc::ok_response(std::move(out));
}

ipc::Response Daemon::cmd_detect(const ipc::Request&) {
  const CapabilityReport fresh = detect_capabilities();  // live re-probe, read-only
  return ipc::ok_response("capabilities:\n" + format_report(fresh));
}

ipc::Response Daemon::cmd_monitor(const ipc::Request&) {
  std::shared_ptr<const monitor::SystemSnapshot> snap;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snap = latest_;
  }
  if (!snap) return ipc::error_response(Errc::Unsupported, "no sample yet, try again in a moment");
  return ipc::ok_response(monitor::format_snapshot(*snap));
}

ipc::Response Daemon::cmd_workload(const ipc::Request&) {
  std::shared_ptr<const workload::WorkloadResult> r;
  {
    std::lock_guard<std::mutex> lock(workload_mutex_);
    r = workload_latest_;
  }
  if (!r) return ipc::error_response(Errc::Unsupported, "no classification yet, try again in a moment");
  return ipc::ok_response(workload::format_workload(*r));
}

ipc::Response Daemon::cmd_policy(const ipc::Request&) {
  std::shared_ptr<const policy::PolicyDecision> d;
  policy::UserMode mode;
  {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    d = policy_latest_;
    mode = user_mode_;
  }
  if (!d) return ipc::error_response(Errc::Unsupported, "no policy decision yet, try again in a moment");
  return ipc::ok_response("User mode:    " + policy::format_user_mode(mode) + "\n" + policy::format_decision(*d));
}

ipc::Response Daemon::cmd_profile(const ipc::Request& req) {
  using policy::UserMode;
  UserMode cur;
  std::shared_ptr<const policy::PolicyDecision> d;
  {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    cur = user_mode_;
    d = policy_latest_;
  }
  auto show = [&](const UserMode& m) {
    std::string s = "Mode:      " + policy::format_user_mode(m) + "\n";
    if (d) s += "Effective: " + std::string(to_string(d->profile)) + " (source: " + std::string(to_string(d->source)) + ")\n";
    return s;
  };
  if (req.args.empty()) return ipc::ok_response(show(cur));

  std::optional<std::string> name;
  bool lock = false, unlock = false;
  for (const auto& a : req.args) {
    if (a == "--lock") lock = true;
    else if (a == "--unlock") unlock = true;
    else if (!name) name = a;
    else return ipc::error_response(Errc::InvalidArgument, "usage: profile [powersave|balanced|performance|auto] [--lock|--unlock]");
  }
  auto bad = [](const std::string& m) { return ipc::error_response(Errc::InvalidArgument, m); };
  if (lock && unlock) return bad("--lock and --unlock are mutually exclusive");

  UserMode next = cur;
  if (name) {
    if (cur.locked && !unlock && !lock) return bad("profile is locked: add --unlock (or --lock to keep it locked)");
    if (*name == "auto") {
      if (lock) return bad("AUTO cannot be locked; pick a profile to lock");
      next = UserMode{};
    } else {
      auto p = policy::parse_user_profile(*name);
      if (!p) return bad("unknown profile '" + *name + "' (powersave, balanced, performance, auto)");
      next = {true, *p, lock};
    }
  } else if (lock) {
    if (!cur.manual) return bad("choose a profile to lock, e.g. profile performance --lock");
    next.locked = true;
  } else {
    next.locked = false;
  }
  if (next == cur) return ipc::ok_response("unchanged\n" + show(cur));

  std::string note;
  if (auto saved = policy::save_user_mode(policy_state_path(), next); !saved) {
    note = "warning: not persisted across restarts (" + describe(saved.error()) + ")\n";
    log::warn("policy", "cannot persist profile choice", {{"error", describe(saved.error())}});
  }
  {
    std::lock_guard<std::mutex> lock_guard(policy_mutex_);
    user_mode_ = next;
  }
  log::info("policy", "user profile mode changed", {{"mode", policy::format_user_mode(next)}, {"uid", "ipc"}});
  scheduler_.run_soon("policy.evaluate");
  return ipc::ok_response("profile mode set to " + policy::format_user_mode(next) + "\n" + note +
                          "(re-evaluating now; see `policy` for the effective decision)\n");
}

ipc::Response Daemon::cmd_conflicts(const ipc::Request&) {
  // Re-scan /proc for this explicit request so short-lived launchers and
  // per-user GameMode processes are not limited to the scheduler's last sample.
  std::vector<safety::ExternalTool> tools;
  workload::ProcessScanner live_scanner;
  if (auto live = live_scanner.scan()) {
    tools = safety::detect_running_tools(*live);
  } else {
    std::lock_guard<std::mutex> lock(workload_mutex_);
    tools = tools_latest_;
  }
  // Refresh package and systemd state too (TLP can remain the owner after its
  // one-shot process has exited). This path is read-only.
  const auto report = safety::detect_tool_inventory(tools);
  return ipc::ok_response(safety::format_tool_inventory(report));
}

ipc::Response Daemon::cmd_safety(const ipc::Request&) {
  std::string s;
  if (guard_) s += safety::format_safety(guard_->status(), guard_->config());
  else s += "Thermal safety: not running\n";
  s += "\n" + safety::format_limits(limits_);
  s += "\nRollback journal: ";
  if (rollback_) {
    s += std::to_string(rollback_->outstanding_count()) + " outstanding change(s)";
    if (rollback_->corrupt_lines() > 0) s += ", " + std::to_string(rollback_->corrupt_lines()) + " damaged line(s) skipped";
    s += "\n";
    for (const auto& r : rollback_->outstanding())
      s += "  #" + std::to_string(r.id) + " " + r.subsystem + " " + r.target + ": " + r.old_value + " -> " + r.new_value + " (" + r.reason + ")\n";
  } else {
    s += "UNAVAILABLE (system changes are refused)\n";
  }
  s += "\nEvery future write passes: target allowlist -> policy -> capability -> conflict -> thermal state -> hardware limits.\n";
  return ipc::ok_response(std::move(s));
}

ipc::Response Daemon::cmd_reset(const ipc::Request&) {
  std::string out;
  const policy::UserMode fresh{};
  if (auto saved = policy::save_user_mode(policy_state_path(), fresh); !saved)
    out += "warning: not persisted (" + describe(saved.error()) + ")\n";
  {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    user_mode_ = fresh;
  }
  out += "profile mode reset to auto (unlocked)\n";
  if (rollback_) {
    const auto rep = rollback_->rollback_all("user reset");
    out += "rollback: restored " + std::to_string(rep.restored) + ", skipped " + std::to_string(rep.skipped) + ", failed " + std::to_string(rep.failed) + "\n";
    for (const auto& n : rep.notes) out += "  " + n + "\n";
  } else {
    out += "rollback journal unavailable\n";
  }
  out += "thermal safety state cannot be reset by the user";
  if (guard_) out += " (currently " + std::string(to_string(guard_->state())) + ")";
  out += "\n";
  log::info("daemon", "user reset performed");
  scheduler_.run_soon("policy.evaluate");
  return ipc::ok_response(std::move(out));
}

ipc::Response Daemon::cmd_controllers(const ipc::Request& req) {
  if (!controls_) return ipc::error_response(Errc::Unsupported, "controllers are not running");
  if (!req.args.empty()) {
    if (req.args.size() != 1) return ipc::error_response(Errc::InvalidArgument, "usage: controllers [off|dry-run|apply]");
    const auto mode = control::parse_controls_mode(req.args[0]);
    if (!mode) return ipc::error_response(Errc::InvalidArgument, "mode must be off, dry-run or apply");
    if (*mode == control::ControlsMode::Apply && ::geteuid() != 0)
      return ipc::error_response(Errc::PermissionDenied, "apply mode needs the daemon to run as root");
    controls_->set_mode(*mode);
    scheduler_.run_soon("policy.evaluate");
    return ipc::ok_response("controllers mode set to " + std::string(to_string(*mode)) +
                            " (runtime only; the configuration file decides after a restart)\n");
  }
  return ipc::ok_response(control::format_controls(controls_->last(), rollback_ ? rollback_->outstanding() : std::vector<safety::ChangeRecord>{}));
}

ipc::Response Daemon::cmd_history(const ipc::Request& req) {
  if (!telemetry_) return ipc::error_response(Errc::Unsupported, "telemetry is disabled or unavailable");
  int n = 24;
  if (!req.args.empty()) {
    auto v = sysfs::parse_u64(req.args[0]);
    if (!v || *v < 1 || *v > 500 || req.args.size() > 1) return ipc::error_response(Errc::InvalidArgument, "usage: history [N]  (N = 1..500 summary rows)");
    n = static_cast<int>(*v);
  }
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  auto recent = telemetry_->recent_detail(now - 24 * 3600);
  auto older = telemetry_->recent_summaries(n);
  if (!recent) return ipc::error_response(Errc::Io, recent.error().message);
  if (!older) return ipc::error_response(Errc::Io, older.error().message);
  return ipc::ok_response(db::format_history(*recent, "Last 24 hours (from detailed samples, per hour/workload/policy):") + "\n" +
                          db::format_history(*older, "Hourly summaries (rolled up from older samples, newest first):"));
}

std::string Daemon::render_telemetry_status() {
  const auto cfg = config_snapshot();
  std::string s;
  s += std::string("State:        ") + (telemetry_on_.load() ? "recording" : "paused") + " (local only; never uploaded)\n";
  s += "Database:     " + telemetry_->path().string() + "\n";
  if (auto st = telemetry_->stats()) {
    auto when = [](std::int64_t t) {
      const std::time_t tt = static_cast<std::time_t>(t);
      std::tm tm{};
      gmtime_r(&tt, &tm);
      char b[32];
      std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%SZ", &tm);
      return std::string(b);
    };
    s += "Size:         " + std::to_string(st->db_bytes / 1024) + " KiB of " + std::to_string(cfg.telemetry_max_db_mb) + " MiB cap\n";
    s += "Rows:         " + std::to_string(st->detailed_rows) + " detailed, " + std::to_string(st->summary_rows) + " hourly summaries\n";
    if (st->oldest_ts) s += "Detailed span: " + when(*st->oldest_ts) + " .. " + when(*st->newest_ts) + "\n";
  }
  s += "Retention:    detailed " + std::to_string(cfg.telemetry_detail_days) + " d, summaries " + std::to_string(cfg.telemetry_summary_days) +
       " d, interval " + std::to_string(cfg.telemetry_interval_ms / 1000) + " s\n";
  s += "Session:      " + session_id_ + "   Device id (salted hash): " + device_hash_ + "\n";
  s += "Stored:       numbers, workload class, profile name, generic target words. Never: process names, command lines, users, hostnames.\n";
  return s;
}

ipc::Response Daemon::cmd_telemetry(const ipc::Request& req) {
  if (!telemetry_) return ipc::error_response(Errc::Unsupported, "telemetry is disabled or unavailable (see [telemetry] in the configuration)");
  if (req.args.empty()) return ipc::ok_response(render_telemetry_status());
  const std::string& a = req.args[0];
  if (req.args.size() == 1 && (a == "on" || a == "off")) {
    telemetry_on_ = a == "on";
    log::info("telemetry", std::string("telemetry ") + (telemetry_on_ ? "resumed" : "paused") + " by user (runtime only)");
    return ipc::ok_response(std::string("telemetry ") + (telemetry_on_ ? "recording" : "paused") + " (runtime only; [telemetry] enabled decides after a restart)\n");
  }
  if (a == "delete") {
    if (req.args.size() != 2 || req.args[1] != "--yes")
      return ipc::error_response(Errc::InvalidArgument, "this permanently deletes all telemetry; repeat with: telemetry delete --yes");
    if (auto r = telemetry_->delete_all(); !r) return ipc::error_response(Errc::Io, r.error().message);
    // Rotating the salt makes any future data unlinkable to what was just deleted.
    std::string note;
    if (auto salt = telemetry::rotate_salt(config_snapshot().state_dir / "telemetry.salt")) {
      std::lock_guard<std::mutex> lock(telemetry_mutex_);
      device_hash_ = telemetry::device_id_hash(*salt, telemetry::read_machine_id());
      std::optional<double> hw_max;
      if (limits_.cpu_khz) hw_max = static_cast<double>(limits_.cpu_khz->max) / 1000.0;
      row_builder_ = std::make_unique<telemetry::RowBuilder>(session_id_, device_hash_, startup_caps_.kernel_release, hw_max);
      note = "device id rotated\n";
    } else {
      note = "warning: could not rotate the device id salt\n";
    }
    log::info("telemetry", "all telemetry deleted by user");
    return ipc::ok_response("all telemetry deleted (detailed rows, summaries, rollup state); database compacted\n" + note);
  }
  return ipc::error_response(Errc::InvalidArgument, "usage: telemetry [on|off|delete --yes]");
}

ipc::Response Daemon::cmd_model(const ipc::Request& req) {
  if (!predictor_) return ipc::error_response(Errc::Unsupported, "the ML advisor is disabled ([ml] enabled = off)");
  if (!req.args.empty()) {
    if (req.args.size() != 1) return ipc::error_response(Errc::InvalidArgument, "usage: model [off|observe|suggest]");
    const std::string& a = req.args[0];
    if (a == "off") ml_mode_ = 0;
    else if (a == "observe") ml_mode_ = 1;
    else if (a == "suggest") ml_mode_ = 2;
    else return ipc::error_response(Errc::InvalidArgument, "mode must be off, observe or suggest");
    log::info("ml", "ML mode changed by user", {{"mode", a}});
    scheduler_.run_soon("policy.evaluate");
    return ipc::ok_response("ML mode set to " + a + " (runtime only; [ml] mode decides after a restart)\n");
  }
  const int mode = ml_mode_.load();
  std::string s = std::string("Mode:          ") + (mode == 0 ? "off" : (mode == 1 ? "observe (records predictions and agreement; applies nothing)" : "suggest (validated suggestions may move the AUTO profile by one step)")) + "\n";
  s += format_prediction(predictor_->status());
  s += "ML never writes to the system: suggestions are validated by the policy engine (mode, thermal state, confidence, step size,\n"
       "power source), then pass the thermal safety layer, hardware limits, conflict checks and the change gate like any other change.\n";
  return ipc::ok_response(std::move(s));
}

ipc::Response Daemon::cmd_training(const ipc::Request& req) {
  if (!adapter_) return ipc::error_response(Errc::Unsupported, "local adaptation is not running (needs [ml] enabled, [ml] training = on and [telemetry] enabled)");
  if (!req.args.empty()) {
    if (req.args.size() != 1) return ipc::error_response(Errc::InvalidArgument, "usage: training [on|off|run|reset-local]");
    const std::string& a = req.args[0];
    if (a == "on" || a == "off") {
      adapter_->set_enabled(a == "on");
      return ipc::ok_response("local adaptation " + a + " (runtime only; [ml] training decides after a restart)\n");
    }
    if (a == "run") {
      adapter_->request_run();
      return ipc::ok_response("a training job was requested; it still starts only while the machine is idle (see `training`)\n");
    }
    if (a == "reset-local") {
      adapter_->reset_local();
      return ipc::ok_response("local models and checkpoints are being deleted; the base models are restored\n");
    }
    return ipc::error_response(Errc::InvalidArgument, "usage: training [on|off|run|reset-local]");
  }
  const auto st = adapter_->status();
  char b[200];
  std::string s = "State:        " + std::string(to_string(st.state)) + (st.detail.empty() ? "" : " - " + st.detail) + "\n";
  s += "Idle gate:    " + std::string(to_string(st.gate.klass)) + " - " + st.gate.reason + "\n";
  if (!st.job_kind.empty()) {
    std::snprintf(b, sizeof b, "Job:          %s model, %.0f%% (epoch %d of %d)\n", st.job_kind.c_str(), 100 * st.progress, st.epoch, st.epochs);
    s += b;
  }
  s += "Samples:      " + std::to_string(st.samples_total) + " recorded, " + std::to_string(st.samples_since_last) + " since the last finished job\n";
  s += std::string("Checkpoint:   ") + (st.checkpoint_exists ? "present (training resumes from it)" : "none") + "\n";
  std::snprintf(b, sizeof b, "Counters:     %lld jobs, %lld models accepted, %lld rejected, %lld pauses, %lld resumes\n", static_cast<long long>(st.jobs_finished),
                static_cast<long long>(st.models_accepted), static_cast<long long>(st.models_rejected), static_cast<long long>(st.pauses),
                static_cast<long long>(st.resumes));
  s += b;
  if (!st.last_results.empty()) {
    s += "Last job:\n";
    for (const auto& r : st.last_results) s += "  " + r + "\n";
  }
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  if (auto ins = telemetry_->ml_insights(now - 30 * 86400); ins && !ins->empty()) {
    s += "Learned on this machine (last 30 days; per workload and applied profile):\n";
    s += "  workload         profile        samples  avg reward  failures\n";
    for (const auto& i : *ins) {
      std::snprintf(b, sizeof b, "  %-15s  %-12s %8lld  %10s  %lld%s\n", std::string(to_string(static_cast<workload::WorkloadClass>(i.workload))).c_str(),
                    i.profile == 0 ? "POWERSAVE" : (i.profile == 1 ? "BALANCED" : "PERFORMANCE"), static_cast<long long>(i.samples),
                    i.avg_reward ? (std::to_string(*i.avg_reward).substr(0, 5)).c_str() : "-", static_cast<long long>(i.failures),
                    i.applied_samples == 0 ? "  (controllers were not applying: outcome unrelated to the profile)" : "");
      s += b;
    }
  }
  s += "Training runs only on a sustained idle machine (AC preferred), at the lowest CPU priority, pauses within one batch when a workload appears,\n"
       "and an adapted model is adopted only if it beats the current one on this machine's hold-out without forgetting the base prior.\n";
  return ipc::ok_response(std::move(s));
}

ipc::Response Daemon::cmd_set_log_level(const ipc::Request& req) {
  if (req.args.size() != 1) return ipc::error_response(Errc::InvalidArgument, "usage: set-log-level <level>");
  auto lvl = log::parse_level(req.args[0]);
  if (!lvl) return ipc::error_response(Errc::InvalidArgument, "unknown log level '" + req.args[0] + "'");
  log::set_level(*lvl);  // runtime only; config reload restores the configured level
  log::info("daemon", "log level changed", {{"level", std::string(log::to_string(*lvl))}});
  return ipc::ok_response("log level: " + std::string(log::to_string(*lvl)) + "\n");
}

ipc::Response Daemon::cmd_reload_config(const ipc::Request&) {
  if (auto r = reload_config(); !r) return ipc::error_response(r.error().code, r.error().message);
  return ipc::ok_response("configuration reloaded\n");
}

std::string Daemon::render_status() const {
  const Config cfg = config_snapshot();
  const auto uptime =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();
  auto cap = [&](std::string_view id) {
    const Capability* c = startup_caps_.find(id);
    return c ? std::string(to_string(c->status)) + " - " + c->detail : std::string("not probed");
  };
  auto line = [](std::string label, const std::string& value) {
    label += ':';
    label.resize(std::max<std::size_t>(label.size(), 15), ' ');
    return label + value + "\n";
  };
  std::string s;
  s += line("Daemon", "running - ml-optimizerd " + std::string(kVersion) + " (pid " + std::to_string(::getpid()) +
                          ", uptime " + std::to_string(uptime) + "s, euid " + std::to_string(::geteuid()) + ")");
  s += line("Phase", std::to_string(kCurrentPhase) + " - monitoring, policy, safety, controllers (dry-run unless [controllers] mode = apply)");
  s += line("Kernel", startup_caps_.kernel_release);
  s += line("CPU", cap("cpu.topology"));
  s += line("CPUFreq", cap("cpufreq"));
  s += line("EPP", cap("cpufreq.epp"));
  s += line("uclamp", cap("sched.uclamp"));
  s += line("GPU", cap("gpu.drm"));
  s += line("Swap", cap("mem.swap"));
  s += line("zRAM", cap("mem.zram"));
  s += line("PSI", cap("psi"));
  s += line("cgroup v2", cap("cgroup.v2"));
  s += line("Thermal", cap("thermal"));
  s += line("Power source", cap("power"));
  {
    std::shared_ptr<const monitor::SystemSnapshot> snap;
    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      snap = latest_;
    }
    if (snap) {
      for (const auto& [k, v] : monitor::format_summary(*snap)) s += line(k, v);
    } else {
      s += line("Live data", "no sample yet");
    }
  }
  {
    std::shared_ptr<const workload::WorkloadResult> w;
    {
      std::lock_guard<std::mutex> lock(workload_mutex_);
      w = workload_latest_;
    }
    char conf[16];
    if (w) std::snprintf(conf, sizeof conf, "%.2f", w->confidence);
    s += line("Workload", w ? std::string(to_string(w->cls)) + " (confidence " + conf + ")" : std::string("no classification yet"));
    s += line("Features", w ? workload::flags_to_string(w->flags) : std::string("n/a"));
  }
  {
    std::shared_ptr<const policy::PolicyDecision> d;
    policy::UserMode mode;
    {
      std::lock_guard<std::mutex> lock(policy_mutex_);
      d = policy_latest_;
      mode = user_mode_;
    }
    s += line("Profile", d ? std::string(to_string(d->profile)) + " (" + std::string(to_string(d->source)) + "; user mode " +
                                 policy::format_user_mode(mode) + ")"
                           : std::string("no decision yet"));
  }
  {
    std::string v = "thermal guard not running";
    if (guard_) {
      const auto st = guard_->status();
      v = std::string(to_string(st.state)) + " - " + st.reason + (st.monitoring ? "" : " [no usable sensors]");
    }
    s += line("Safety state", v);
    s += line("Rollback", rollback_ ? std::to_string(rollback_->outstanding_count()) + " outstanding change(s)" : std::string("journal unavailable"));
  }
  {
    std::string v = "disabled";
    if (predictor_) {
      const auto st = predictor_->status();
      const int mode = ml_mode_.load();
      v = std::string(mode == 0 ? "off" : (mode == 1 ? "observe" : "suggest"));
      if (st.ready) {
        char b[96];
        std::snprintf(b, sizeof b, ": models ready, agreement with rules workload %.0f%% / profile %.0f%% (n=%lld)", 100 * st.workload_agreement.value,
                      100 * st.profile_agreement.value, static_cast<long long>(st.profile_agreement.n));
        v += b;
      } else {
        v += st.bootstrapping ? ": building base models" : ": models unavailable";
      }
    }
    s += line("ML state", v);
  if (adapter_) {
    const auto st = adapter_->status();
    s += line("Local training", std::string(to_string(st.state)) + " (gate " + std::string(to_string(st.gate.klass)) + "), " + std::to_string(st.samples_total) +
                                 " samples, " + std::to_string(st.models_accepted) + " adapted model(s) accepted");
  }
  }
  {
    std::string v = "disabled";
    if (telemetry_) {
      v = telemetry_on_.load() ? "recording" : "paused";
      if (auto st = telemetry_->stats()) v += ": " + std::to_string(st->detailed_rows) + " samples, " + std::to_string(st->summary_rows) + " summaries, " + std::to_string(st->db_bytes / 1024) + " KiB";
    }
    s += line("Telemetry", v + " (local only)");
  }
  {
    std::string v = "not running";
    if (controls_) {
      const auto r = controls_->last();
      v = std::string(to_string(controls_->mode())) + ": last cycle applied " + std::to_string(r.applied) + ", would apply " +
          std::to_string(r.would_apply) + ", blocked " + std::to_string(r.blocked) + ", failed " + std::to_string(r.failed) +
          " (see `controllers`)";
    }
    s += line("Controllers", v);
  }
  {
    std::lock_guard<std::mutex> lock(workload_mutex_);
    std::string names;
    for (const auto& t : tools_latest_) names += (names.empty() ? "" : ", ") + t.name;
    s += line("Conflicts", names.empty() ? std::string("no known optimizer running") : names + " (observe-only; see `conflicts`)");
  }
  s += line("Config", "log_level=" + std::string(log::to_string(cfg.log_level)) + " aggressiveness=" +
                          std::to_string(cfg.aggressiveness) + " (" + std::string(aggressiveness_name(cfg.aggressiveness)) +
                          ") state_dir=" + cfg.state_dir.string());
  return s;
}

ipc::Response Daemon::cmd_status(const ipc::Request&) { return ipc::ok_response(render_status()); }

}  // namespace opt
