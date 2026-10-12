#include "daemon/daemon.hpp"

#include <csignal>
#include <ctime>
#include <thread>
#include <cstdio>
#include <unistd.h>

#include "core/version.hpp"
#include "ml/dataset.hpp"
#include "sd_notify.hpp"
#include "telemetry/ids.hpp"

namespace opt {
namespace {
constexpr int kExitConfig = 78;  // EX_CONFIG
}

Daemon::Daemon(DaemonOptions options) : options_(std::move(options)) {}

Config Daemon::config_snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void Daemon::sample_once() {
  auto snap = std::make_shared<const monitor::SystemSnapshot>(monitor_->sample());
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  latest_ = std::move(snap);
}

void Daemon::classify_once() {
  std::shared_ptr<const monitor::SystemSnapshot> sys;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    sys = latest_;
  }
  auto procs = scanner_.scan();
  if (!procs) {
    if (!scan_failure_reported_) log::warn("workload", "process scan failed", {{"error", describe(procs.error())}});
    scan_failure_reported_ = true;
    return;
  }
  const workload::DetectContext ctx{*procs, sys.get()};
  auto result = std::make_shared<const workload::WorkloadResult>(classifier_.evaluate(ctx));
  const auto process_tools = safety::detect_running_tools(*procs);
  safety::ToolInventoryReport tool_inventory;
  bool refresh_tool_inventory = false;
  bool refresh_tool_units = false;
  const auto inventory_now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(workload_mutex_);
    refresh_tool_inventory = last_tool_inventory_probe_ == std::chrono::steady_clock::time_point{} ||
                             inventory_now - last_tool_inventory_probe_ >= std::chrono::minutes(5);
    refresh_tool_units = refresh_tool_inventory || last_tool_unit_probe_ == std::chrono::steady_clock::time_point{} ||
                         inventory_now - last_tool_unit_probe_ >= std::chrono::seconds(5);
    if (!refresh_tool_inventory) tool_inventory = tool_inventory_latest_;
  }
  if (refresh_tool_inventory) {
    tool_inventory = safety::detect_tool_inventory(process_tools);
  } else if (refresh_tool_units) {
    safety::refresh_tool_unit_states(tool_inventory);
  }
  if (refresh_tool_units) {
    log::debug("tools", "refreshed external tool state",
               {{"entries", std::to_string(tool_inventory.entries.size())},
                {"systemd", tool_inventory.systemd_query_succeeded ? "available" : "unavailable"}});
  }
  auto tools = safety::effective_tool_owners(tool_inventory, process_tools);
  std::shared_ptr<const ml::FeatureVector> features;
  if (predictor_ && ml_mode_.load() != 0)  // aggregates only: process names never leave extract_features()
    features = std::make_shared<const ml::FeatureVector>(ml::extract_features(*procs, sys.get(), ml::FeatureEnv{cpu_hw_max_mhz_}));
  if (adapter_) {  // lets background learning pause the moment a real workload appears
    ml::GateInput g;
    g.cls = result->cls;
    g.flags = result->flags;
    g.confidence = result->confidence;
    g.audio_running = procs->audio_running.value_or(false);
    if (guard_) g.safety = guard_->state();
    if (sys) {
      if (sys->cpu) g.cpu_pct = sys->cpu->utilization_pct;
      if (sys->gpu)
        for (const auto& d : sys->gpu->devices)
          if (d.busy_pct) g.gpu_pct = std::max(g.gpu_pct.value_or(0.0), *d.busy_pct);
      if (sys->psi) {
        auto some10 = [](const std::optional<monitor::PsiResource>& r) -> std::optional<double> { return r && r->some ? std::optional<double>(r->some->avg10) : std::nullopt; };
        g.psi_cpu = some10(sys->psi->cpu); g.psi_mem = some10(sys->psi->memory); g.psi_io = some10(sys->psi->io);
      }
      if (sys->thermal) g.thermal_margin_c = policy::thermal_margin(*sys->thermal);
      if (sys->memory) { g.mem_available_kb = sys->memory->available_kb; g.mem_total_kb = sys->memory->total_kb; }
      if (sys->power) { g.on_ac = sys->power->on_ac; g.battery_pct = sys->power->battery_pct; }
    }
    adapter_->update_gate(g, monitor::Clock::now());
  }
  std::lock_guard<std::mutex> lock(workload_mutex_);
  workload_latest_ = std::move(result);
  tools_latest_ = std::move(tools);
  if (refresh_tool_inventory || refresh_tool_units) {
    tool_inventory_latest_ = std::move(tool_inventory);
    last_tool_unit_probe_ = inventory_now;
  }
  if (refresh_tool_inventory) last_tool_inventory_probe_ = inventory_now;
  features_latest_ = std::move(features);
}

std::filesystem::path Daemon::policy_state_path() const { return config_snapshot().state_dir / "policy.state"; }

void Daemon::policy_once() {
  std::shared_ptr<const monitor::SystemSnapshot> sys;
  std::shared_ptr<const workload::WorkloadResult> wl;
  std::vector<safety::ExternalTool> tools;
  policy::UserMode mode;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    sys = latest_;
  }
  {
    std::lock_guard<std::mutex> lock(workload_mutex_);
    wl = workload_latest_;
    tools = tools_latest_;
  }
  {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    mode = user_mode_;
  }
  std::shared_ptr<const ml::FeatureVector> features;
  {
    std::lock_guard<std::mutex> lock(workload_mutex_);
    features = features_latest_;
  }
  policy::PolicyInput in;
  in.user = mode;
  in.aggressiveness = config_snapshot().aggressiveness;
  in.workload = wl.get();
  in.sys = sys.get();
  in.caps = &startup_caps_;
  in.tools = &tools;
  in.now = monitor::Clock::now();
  const auto cfg_now = config_snapshot();
  // ML advice (optional): evaluated here so it is exactly as fresh as the decision it may influence.
  ml::MlAdvice advice;
  const int ml_mode = ml_mode_.load();
  if (predictor_ && ml_mode != 0 && features && wl) {
    const ml::ProfileFeatureInput pin = make_profile_input(*wl, sys.get(), in.aggressiveness);
    std::optional<double> cpu_util;
    if (sys && sys->cpu) cpu_util = sys->cpu->utilization_pct;
    advice = predictor_->evaluate(*features, pin, cpu_util).advice;
  }
  in.ml = advice.available ? &advice : nullptr;
  in.ml_suggest = ml_mode == 2;
  in.ml_min_confidence = cfg_now.ml_min_confidence;
  if (guard_) in.safety = guard_->state();
  const auto* sys_ptr = sys.get();
  auto decision = std::make_shared<const policy::PolicyDecision>(engine_.evaluate(in));
  if (predictor_ && decision->rule_valid && wl) predictor_->record_outcome(wl->cls, decision->rule_profile);  // agreement vs the rules
  if (controls_) controls_->apply(*decision, in.safety, sys_ptr, in.now);  // gated + journaled (or dry-run)
  std::lock_guard<std::mutex> lock(policy_mutex_);
  policy_latest_ = std::move(decision);
}

ml::ProfileFeatureInput Daemon::make_profile_input(const workload::WorkloadResult& wl, const monitor::SystemSnapshot* sys, int aggr) const {
  ml::ProfileFeatureInput pin;
  pin.cls = wl.cls;
  pin.confidence = wl.confidence;
  pin.flags = wl.flags;
  pin.aggressiveness = aggr;
  if (sys) {
    if (sys->power) { pin.on_ac = sys->power->on_ac; pin.battery_pct = sys->power->battery_pct; }
    if (sys->thermal) pin.thermal_margin_c = policy::thermal_margin(*sys->thermal);
    if (sys->psi && sys->psi->cpu && sys->psi->cpu->some) pin.psi_cpu10 = sys->psi->cpu->some->avg10;
    if (sys->cpu && sys->cpu->utilization_pct) pin.cpu_util = *sys->cpu->utilization_pct;
  }
  return pin;
}

void Daemon::telemetry_once() {
  if (!telemetry_ || !telemetry_on_.load()) return;
  const auto now = monitor::Clock::now();
  if (now < telemetry_backoff_until_) return;
  std::shared_ptr<const monitor::SystemSnapshot> sys;
  std::shared_ptr<const workload::WorkloadResult> wl;
  std::shared_ptr<const policy::PolicyDecision> decision;
  std::shared_ptr<const ml::FeatureVector> features;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    sys = latest_;
  }
  {
    std::lock_guard<std::mutex> lock(workload_mutex_);
    wl = workload_latest_;
    features = features_latest_;
  }
  {
    std::lock_guard<std::mutex> lock(policy_mutex_);
    decision = policy_latest_;
  }
  if (!sys || !decision) return;  // nothing meaningful to record yet

  telemetry::SampleInputs in;
  in.unix_time = static_cast<std::int64_t>(std::time(nullptr));
  in.interval_s = last_telemetry_.time_since_epoch().count() == 0
                      ? config_snapshot().telemetry_interval_ms / 1000.0
                      : std::chrono::duration<double>(now - last_telemetry_).count();
  in.sys = sys.get();
  in.workload = wl.get();
  in.decision = decision.get();
  if (guard_) in.safety = guard_->state();
  if (controls_) {
    const auto t = controls_->totals();
    in.applied = t.applied - last_totals_.applied;
    in.blocked = t.blocked - last_totals_.blocked;
    in.failed = t.failed - last_totals_.failed;
    in.reverted = t.reverted - last_totals_.reverted;
    last_totals_ = t;
  }
  if (last_profile_ && *last_profile_ != decision->profile) in.profile_changes = 1;
  last_profile_ = decision->profile;
  last_telemetry_ = now;

  db::TelemetryRow row;
  {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    row = row_builder_->build(in);
  }
  if (auto r = telemetry_->insert(row); !r) {
    if (++telemetry_failures_ == 1) log::warn("telemetry", "cannot record a sample", {{"error", describe(r.error())}});
    if (telemetry_failures_ >= 5) {
      telemetry_backoff_until_ = now + std::chrono::minutes(10);
      telemetry_failures_ = 0;
      log::warn("telemetry", "repeated failures: pausing telemetry for 10 minutes");
    }
  } else {
    telemetry_failures_ = 0;
    last_insert_unix_.store(in.unix_time);
    if (adapter_ && features && wl) {  // one training example next to the telemetry row (same database)
      db::MlSampleRow s;
      s.ts = in.unix_time;
      s.workload = static_cast<int>(wl->cls);
      s.wl_confidence = wl->confidence;
      s.features = ml::quantize(features->data(), ml::kFeatureCount);
      const auto pf = ml::profile_features(make_profile_input(*wl, sys.get(), decision->aggressiveness));
      s.pfeatures = ml::quantize(pf.data(), ml::kProfileFeatureCount);
      s.rule_profile = decision->rule_valid ? (decision->rule_profile == Profile::Powersave ? 0 : (decision->rule_profile == Profile::Balanced ? 1 : 2)) : -1;
      s.applied_profile = decision->profile == Profile::Powersave ? 0 : (decision->profile == Profile::Balanced ? 1 : (decision->profile == Profile::Performance ? 2 : 3));
      s.auto_source = decision->source == policy::ProfileSource::Auto && decision->rule_valid;
      s.controllers_apply = controls_ && controls_->mode() == control::ControlsMode::Apply;
      s.reward = row.reward;
      s.success = row.success;
      if (auto ms = telemetry_->insert_ml_sample(s); !ms) log::debug("telemetry", "cannot record a training sample", {{"error", describe(ms.error())}});
    }
  }
}

void Daemon::telemetry_maintain() {
  if (!telemetry_) return;
  auto rep = telemetry_->maintain(static_cast<std::int64_t>(std::time(nullptr)));
  if (!rep) {
    log::warn("telemetry", "maintenance failed", {{"error", describe(rep.error())}});
    return;
  }
  if (rep->any())
    log::info("telemetry", "maintenance", {{"rolled_up_buckets", std::to_string(rep->buckets_rolled)},
                                             {"pruned_detail", std::to_string(rep->pruned_detail)},
                                             {"pruned_summaries", std::to_string(rep->pruned_summaries)},
                                             {"trimmed_for_size", std::to_string(rep->trimmed_for_size)}});
}

Result<void> Daemon::reload_config() {
  bool found = false;
  auto fresh = load_config(options_.config_path, &found);
  if (!fresh) return fail(fresh.error());
  std::lock_guard<std::mutex> lock(mutex_);
  if (fresh->socket_path != config_.socket_path) {
    log::warn("config", "ipc.socket_path changed; restart required to apply",
              {{"active", config_.socket_path.string()}, {"new", fresh->socket_path.string()}});
    fresh->socket_path = config_.socket_path;
  }
  config_ = *fresh;
  log::set_level(config_.log_level);
  scheduler_.set_interval("monitor.sample", std::chrono::milliseconds(config_.monitor_interval_ms));
  scheduler_.set_interval("workload.classify", std::chrono::milliseconds(config_.workload_interval_ms));
  scheduler_.set_interval("policy.evaluate", std::chrono::milliseconds(config_.policy_interval_ms));
  scheduler_.set_interval("telemetry.sample", std::chrono::milliseconds(config_.telemetry_interval_ms));
  if (predictor_) ml_mode_ = config_.ml_mode == "suggest" ? 2 : 1;
  if (telemetry_)
    telemetry_->set_retention({config_.telemetry_detail_days, config_.telemetry_summary_days,
                               static_cast<std::int64_t>(config_.telemetry_max_db_mb) * 1024 * 1024});
  if (controls_) {
    auto mode = control::parse_controls_mode(config_.controllers_mode).value_or(control::ControlsMode::DryRun);
    if (mode == control::ControlsMode::Apply && ::geteuid() != 0) mode = control::ControlsMode::DryRun;
    controls_->set_runtime_pm(config_.runtime_pm);
    controls_->set_mode(mode);
  }
  log::info("config", found ? "configuration reloaded" : "config file absent, using defaults",
            {{"file", options_.config_path.string()}});
  return {};
}

int Daemon::run() {
  // 1. Configuration (strict: a typo must not silently change behaviour).
  bool found = false;
  auto cfg = load_config(options_.config_path, &found);
  if (!cfg) {
    log::error("config", "invalid configuration", {{"error", describe(cfg.error())}});
    return kExitConfig;
  }
  config_ = *cfg;
  log::set_level(config_.log_level);
  log::info("daemon", "starting",
            {{"version", kVersion},
             {"phase", std::to_string(kCurrentPhase)},
             {"config", found ? options_.config_path.string() : std::string("(defaults)")}});

  // 2. Capability detection (read-only).
  startup_caps_ = detect_capabilities();
  for (const auto& c : startup_caps_.all())
    log::debug("caps", c.id, {{"status", std::string(to_string(c.status))}, {"detail", c.detail}});
  if (!startup_caps_.available("sched.uclamp")) log::info("caps", "uclamp backend disabled");
  if (!startup_caps_.available("gpu.devfreq")) log::info("caps", "GPU DevFreq backend unavailable");

  monitor_ = std::make_unique<monitor::SystemMonitor>();
  limits_ = safety::HardwareLimits::discover();

  if (options_.check_only) {
    // Two samples so rates (CPU %, I/O, swap, RAPL, GPU RC6) can be computed.
    monitor_->sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto snap = monitor_->sample();
    safety::ThermalGuard check_guard({"/", {}, std::chrono::milliseconds(1000), std::chrono::seconds(0), {}}, nullptr);
    check_guard.step();
    workload::ProcessScanner scanner;
    workload::Classifier classifier({0.35, 0.15, 1});  // single shot: no confirmation delay
    (void)scanner.scan();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::string wl = "process scan failed\n";
    if (auto procs = scanner.scan()) {
      const workload::DetectContext ctx{*procs, &snap};
      const auto& result = classifier.evaluate(ctx);
      wl = workload::format_workload(result);
      const auto process_tools = safety::detect_running_tools(*procs);
      const auto tool_inventory = safety::detect_tool_inventory(process_tools);
      const auto tools = safety::effective_tool_owners(tool_inventory, process_tools);
      policy::PolicyEngine engine;
      policy::PolicyInput pin;
      pin.safety = check_guard.state();
      pin.aggressiveness = config_.aggressiveness;
      pin.workload = &result;
      pin.sys = &snap;
      pin.caps = &startup_caps_;
      pin.tools = &tools;
      if (auto saved = policy::load_user_mode(config_.state_dir / "policy.state")) pin.user = *saved;
      wl += "\nML advisor: " + std::string(!config_.ml_enabled ? "disabled" : (ml::Predictor::cache_ok(config_.state_dir / "ml") ? "base models cached" : "base models are built at the first daemon start (about 2 s, low priority)")) +
            " (mode " + config_.ml_mode + "; suggestions never bypass the rules, safety or the gate)\n";
      wl += "\n" + safety::format_tool_inventory(tool_inventory);
      wl += "\n" + safety::format_safety(check_guard.status(), check_guard.config());
      const auto& decision = engine.evaluate(pin);
      wl += "\nPolicy (user mode: " + policy::format_user_mode(pin.user) + "):\n" + policy::format_decision(decision);
      control::FileStateWriter check_writer;
      control::ControlManager preview("/", nullptr, check_writer, startup_caps_, limits_, control::ControlsMode::DryRun, config_.runtime_pm);
      wl += "\nControllers (preview, nothing is written):\n" + control::format_controls(preview.apply(decision, pin.safety, &snap), {});
    }
    std::printf("configuration OK (%s)\n\ncapabilities:\n%s\nlive sample (500 ms window):\n%s\n%s\n%s",
                found ? options_.config_path.c_str() : "defaults", format_report(startup_caps_).c_str(),
                monitor::format_snapshot(snap).c_str(), wl.c_str(), safety::format_limits(limits_).c_str());
    return 0;
  }

  if (::geteuid() != 0)
    log::warn("daemon", "not running as root; fine while read-only but later phases need root");

  // 2b. Persisted profile choice/lock (own state dir only, never /sys).
  if (auto saved = policy::load_user_mode(policy_state_path())) {
    user_mode_ = *saved;
    if (user_mode_.manual) log::info("policy", "restored user profile", {{"mode", policy::format_user_mode(user_mode_)}});
  } else {
    log::warn("policy", "ignoring unreadable policy state; using AUTO", {{"error", describe(saved.error())}});
  }

  // 2c. Rollback journal: restore anything an earlier run left changed (crash/restart safety).
  if (auto journal = safety::RollbackJournal::open(config_.state_dir / "rollback.journal")) {
    rollback_ = std::make_unique<safety::RollbackManager>(std::move(*journal), writer_);
    if (rollback_->corrupt_lines() > 0)
      log::warn("rollback", "journal had damaged lines (skipped)", {{"lines", std::to_string(rollback_->corrupt_lines())}});
    if (const auto n = rollback_->outstanding_count(); n > 0) {
      log::safety("rollback", "restoring changes left by a previous run", {{"changes", std::to_string(n)}});
      const auto rep = rollback_->rollback_all("recovery after restart");
      if (!rep.ok()) log::safety("rollback", "recovery incomplete; journal kept for the next start", {{"failed", std::to_string(rep.failed)}});
    }
  } else {
    log::safety("rollback", "journal unavailable: system changes will be refused", {{"error", describe(journal.error())}});
  }

  // 2c'. Controllers (dry-run by default; APPLY needs root).
  {
    auto mode = control::parse_controls_mode(config_.controllers_mode).value_or(control::ControlsMode::DryRun);
    if (mode == control::ControlsMode::Apply && ::geteuid() != 0) {
      log::warn("controllers", "mode=apply needs root; staying in dry-run");
      mode = control::ControlsMode::DryRun;
    }
    controls_ = std::make_unique<control::ControlManager>("/", rollback_.get(), writer_, startup_caps_, limits_, mode, config_.runtime_pm);
    log::info("controllers", "controllers ready", {{"mode", std::string(to_string(mode))}});
  }

  // 2d. Thermal guard: own thread, own sensor reads, state survives restarts.
  safety::GuardOptions gopt;
  gopt.state_file = config_.state_dir / "safety.state";
  gopt.interval = std::chrono::milliseconds(config_.safety_interval_ms);
  guard_ = std::make_unique<safety::ThermalGuard>(gopt, [this](SafetyState from, SafetyState to, const std::string& why) {
    log::safety("daemon", "thermal safety state " + std::string(to_string(from)) + " -> " + std::string(to_string(to)), {{"reason", why}});
    // Throttle right now from the guard thread (never waits for the scheduler): protective actions only
    // LOWER performance; the next policy cycle then reverts anything that is no longer wanted.
    if (to == SafetyState::Emergency && controls_)
      controls_->apply(control::ControlManager::emergency_decision(), SafetyState::Emergency, nullptr);
    scheduler_.run_soon("policy.evaluate");
  });
  guard_->start();

  // 2d'. ML advisor: loads cached base models or builds them once in a low-priority thread.
  if (limits_.cpu_khz) cpu_hw_max_mhz_ = static_cast<double>(limits_.cpu_khz->max) / 1000.0;
  if (config_.ml_enabled) {
    predictor_ = std::make_unique<ml::Predictor>(config_.state_dir / "ml");
    ml_mode_ = config_.ml_mode == "suggest" ? 2 : 1;
    predictor_->start();
    log::info("ml", "ML advisor enabled", {{"mode", config_.ml_mode}});
  }

  // 2e. Telemetry: local SQLite, hashed device id, bounded size. Failure never stops the daemon.
  if (config_.telemetry_enabled) {
    const auto state = config_.state_dir;
    session_id_ = telemetry::make_session_id();
    if (auto salt = telemetry::load_or_create_salt(state / "telemetry.salt")) {
      device_hash_ = telemetry::device_id_hash(*salt, telemetry::read_machine_id());
      db::RetentionPolicy ret{config_.telemetry_detail_days, config_.telemetry_summary_days,
                              static_cast<std::int64_t>(config_.telemetry_max_db_mb) * 1024 * 1024};
      if (auto store = db::TelemetryStore::open(state / "telemetry.db", ret)) {
        telemetry_ = std::move(*store);
        std::optional<double> hw_max;
        if (limits_.cpu_khz) hw_max = static_cast<double>(limits_.cpu_khz->max) / 1000.0;
        row_builder_ = std::make_unique<telemetry::RowBuilder>(session_id_, device_hash_, startup_caps_.kernel_release, hw_max);
        telemetry_on_ = true;
        telemetry_maintain();
        log::info("telemetry", "local telemetry enabled", {{"db", (state / "telemetry.db").string()}});
      } else {
        log::warn("telemetry", "telemetry disabled: database unavailable", {{"error", describe(store.error())}});
      }
    } else {
      log::warn("telemetry", "telemetry disabled: cannot create the installation salt", {{"error", describe(salt.error())}});
    }
  }

  // 2f. Local adaptation (needs the models and the telemetry database that holds the samples).
  if (predictor_ && telemetry_ && config_.ml_training) {
    ml::AdaptationOptions ao;
    ao.dir = config_.state_dir / "ml";
    ao.gate.allow_on_battery = config_.ml_training_on_battery;
    ao.gate.idle_seconds = config_.ml_training_idle_seconds;
    ao.min_new_samples = config_.ml_training_min_samples;
    ao.interval = std::chrono::hours(config_.ml_training_interval_hours);
    adapter_ = std::make_unique<ml::Adapter>(ao, *telemetry_, *predictor_);
    adapter_->start();
    log::info("adapt", "local adaptation enabled (runs only on a sustained idle machine)");
  }

  // 3. IPC.
  server_ = std::make_unique<ipc::IpcServer>(
      config_.socket_path,
      [this](const ipc::Request& r, const ipc::PeerCred& p) { return handle(r, p); });
  if (auto s = server_->start(); !s) {
    log::error("daemon", "cannot start IPC server", {{"error", describe(s.error())}});
    return 1;
  }

  scheduler_.add("monitor.sample", std::chrono::milliseconds(config_.monitor_interval_ms), [this] { sample_once(); });
  scheduler_.add("workload.classify", std::chrono::milliseconds(config_.workload_interval_ms), [this] { classify_once(); });
  if (telemetry_) {
    scheduler_.add("telemetry.sample", std::chrono::milliseconds(config_.telemetry_interval_ms), [this] { telemetry_once(); });
    scheduler_.add("telemetry.maintain", std::chrono::hours(1), [this] { telemetry_maintain(); });
  }
  scheduler_.add("policy.evaluate", std::chrono::milliseconds(config_.policy_interval_ms), [this] { policy_once(); });
  scheduler_.start();

  sd::notify("READY=1\nSTATUS=Phase 10: monitoring, adaptive policy, safety, controllers, telemetry, ML adaptation, tool inventory and IPC ready");
  const bool watchdog = sd::watchdog_usec() > 0;
  log::info("daemon", "ready", {{"pid", std::to_string(::getpid())}});

  // 4. Main loop: signals + 1 s tick (watchdog; scheduler hook for later phases).
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  bool running = true;
  while (running) {
    const timespec ts{1, 0};
    siginfo_t info{};
    const int sig = ::sigtimedwait(&set, &info, &ts);
    if (sig < 0) {
      if (errno == EAGAIN || errno == EINTR) {
        if (watchdog) sd::notify("WATCHDOG=1");
        continue;
      }
      log::error("daemon", "sigtimedwait failed", {{"errno", std::to_string(errno)}});
      break;
    }
    if (sig == SIGHUP) {
      sd::notify("RELOADING=1");
      if (auto r = reload_config(); !r)
        log::error("config", "reload failed, keeping previous configuration", {{"error", describe(r.error())}});
      sd::notify("READY=1");
    } else {
      log::info("daemon", "shutdown requested", {{"signal", std::to_string(sig)}});
      running = false;
    }
  }

  // 5. Graceful shutdown. Nothing to roll back yet (no system changes).
  sd::notify("STOPPING=1");
  scheduler_.stop();
  if (guard_) guard_->stop();                                  // no thermal actions may race the final rollback
  if (rollback_) rollback_->rollback_all("daemon shutdown");  // leave the system as we found it
  if (adapter_) adapter_->stop();                              // pauses training with a checkpoint
  adapter_.reset();
  predictor_.reset();                                          // stops/joins the model build thread
  telemetry_.reset();                                          // closes the database (WAL checkpoint)
  server_->stop();
  log::info("daemon", "stopped");
  return 0;
}

}  // namespace opt
