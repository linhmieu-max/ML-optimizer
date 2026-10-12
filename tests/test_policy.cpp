#include <atomic>
#include <filesystem>
#include <thread>
#include <fstream>

#include "check.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "daemon/scheduler.hpp"
#include "policy/policy_engine.hpp"
#include "policy/policy_store.hpp"
#include "policy/profiles.hpp"

namespace fs = std::filesystem;
using namespace opt;
using namespace opt::policy;
using workload::WorkloadClass;
using std::chrono::seconds;

namespace {

monitor::SystemSnapshot make_sys(std::optional<bool> ac = true, int battery = 100) {
  monitor::SystemSnapshot s;
  s.power = monitor::PowerSample{};
  s.power->on_ac = ac;
  s.power->battery_pct = battery;
  return s;
}
void add_thermal(monitor::SystemSnapshot& s, double temp, double hot_limit) {
  s.thermal = monitor::ThermalSample{};
  monitor::ThermalZone z;
  z.name = "thermal_zone0";
  z.type = "acpitz";
  z.temp_c = temp;
  z.trips.push_back({hot_limit, "hot"});
  s.thermal->zones.push_back(z);
}
void add_psi(monitor::SystemSnapshot& s, double cpu, double mem = 0, double io = 0) {
  auto res = [](double v) {
    monitor::PsiResource r;
    r.some = monitor::PsiLine{v, v, v, 0};
    return r;
  };
  s.psi = monitor::PsiSample{res(cpu), res(mem), res(io)};
}
workload::WorkloadResult wl(WorkloadClass c, unsigned flags = 0, double conf = 0.8) {
  workload::WorkloadResult r;
  r.cls = c;
  r.flags = flags;
  r.confidence = conf;
  return r;
}
PolicyInput input(const workload::WorkloadResult* w, const monitor::SystemSnapshot* s, Clock::time_point now,
                  UserMode user = {}, int aggr = 3) {
  PolicyInput in;
  in.user = user;
  in.aggressiveness = aggr;
  in.workload = w;
  in.sys = s;
  in.now = now;
  return in;
}

void test_profiles() {
  CHECK(base_targets(Profile::Powersave).cpu == Bias::Power && !base_targets(Profile::Powersave).allow_boost);
  CHECK(base_targets(Profile::Performance).cpu == Bias::Performance);
  CHECK(!base_targets(Profile::ThermalEmergency).allow_boost && base_targets(Profile::ThermalEmergency).background == Restriction::Strong);
  CHECK(step_down(Profile::Performance, 1) == Profile::Balanced && step_down(Profile::Performance, 2) == Profile::Powersave);
  CHECK(step_down(Profile::Powersave, 3) == Profile::Powersave && step_down(Profile::ThermalEmergency, 1) == Profile::ThermalEmergency);
  CHECK(parse_user_profile("balanced") == Profile::Balanced && !parse_user_profile("auto") && !parse_user_profile("thermal-emergency"));

  Targets g = base_targets(Profile::Performance);
  tune_for_workload(g, WorkloadClass::Game, workload::kLatencySensitive, 3);
  CHECK(g.io == IoBias::Latency && g.background == Restriction::Strong && g.gpu == Bias::Performance);
  Targets g1 = base_targets(Profile::Performance);
  tune_for_workload(g1, WorkloadClass::Game, 0, 1);
  CHECK(g1.background == Restriction::Light);  // conservative level never uses Strong
  Targets c = base_targets(Profile::Balanced);
  tune_for_workload(c, WorkloadClass::Compile, 0, 3);
  CHECK(c.io == IoBias::Throughput && c.memory == MemoryBias::Throughput && c.background == Restriction::None);
  Targets c2 = base_targets(Profile::Balanced);
  tune_for_workload(c2, WorkloadClass::Compile, workload::kLatencySensitive, 3);
  CHECK(c2.io == IoBias::Balanced);  // latency-sensitive work is never starved
  Targets f = base_targets(Profile::Performance);
  tune_for_workload(f, WorkloadClass::FileCopy, 0, 3);
  CHECK(f.cpu == Bias::Balanced);
}

ProfileChoice rule(WorkloadClass c, std::optional<bool> ac = true, int battery = 100, int aggr = 3, unsigned flags = 0,
                   double conf = 0.8, std::optional<double> margin = std::nullopt) {
  RuleInput r;
  r.cls = c; r.on_ac = ac; r.battery_pct = battery; r.aggressiveness = aggr; r.flags = flags; r.confidence = conf; r.thermal_margin_c = margin;
  return choose_profile(r);
}

void test_rules() {
  CHECK(rule(WorkloadClass::Compile).profile == Profile::Performance);
  CHECK(rule(WorkloadClass::Game, true, 100, 2).profile == Profile::Performance);
  CHECK(rule(WorkloadClass::Compile, true, 100, 1).profile == Profile::Balanced);           // conservative: no proven headroom
  CHECK(rule(WorkloadClass::Compile, true, 100, 1, 0, 0.8, 25.0).profile == Profile::Performance);
  CHECK(rule(WorkloadClass::Compile, false, 80).profile == Profile::Balanced);              // battery: never Performance
  CHECK(rule(WorkloadClass::Game, false, 60, 3).profile == Profile::Balanced);
  CHECK(rule(WorkloadClass::Game, false, 60, 4).profile == Profile::Performance);
  CHECK(rule(WorkloadClass::Game, false, 35, 4).profile == Profile::Balanced);
  CHECK(rule(WorkloadClass::Game, false, 10).profile == Profile::Powersave && rule(WorkloadClass::Game, false, 10).urgent);
  CHECK(rule(WorkloadClass::Idle).profile == Profile::Powersave);                           // aggressive: idle -> powersave
  CHECK(rule(WorkloadClass::Idle, true, 100, 2).profile == Profile::Balanced);
  CHECK(rule(WorkloadClass::Idle, false, 80, 2).profile == Profile::Powersave);
  CHECK(rule(WorkloadClass::Idle, true, 100, 3, workload::kLatencySensitive).profile == Profile::Balanced);
  CHECK(rule(WorkloadClass::Desktop).profile == Profile::Balanced);
  CHECK(rule(WorkloadClass::Desktop, true, 100, 4, workload::kHighLoad).profile == Profile::Performance);
  CHECK(rule(WorkloadClass::Desktop, true, 100, 3, workload::kHighLoad).profile == Profile::Balanced);
  CHECK(rule(WorkloadClass::Vm, true, 100, 3, workload::kHighLoad).profile == Profile::Performance);
  CHECK(rule(WorkloadClass::Background, true, 100, 3).profile == Profile::Powersave);
  CHECK(rule(WorkloadClass::Background, true, 100, 3, workload::kHighLoad).profile == Profile::Balanced);
  CHECK(rule(WorkloadClass::Compile, true, 100, 3, 0, 0.1).profile == Profile::Balanced);   // low confidence: default only
  CHECK(rule(WorkloadClass::Compile, std::nullopt).profile == Profile::Performance);        // unknown source: not battery
  CHECK(!rule(WorkloadClass::Compile).reasons.empty());
}

void test_feedback() {
  Feedback f;
  const auto t0 = Clock::now();
  auto s = make_sys();
  add_thermal(s, 70, 100);  // margin 30
  CHECK(f.thermal(&s, t0).derate == 0);
  add_thermal(s, 90, 100);  // margin 10 -> derate 1 immediately
  auto a = f.thermal(&s, t0 + seconds(1));
  CHECK(a.derate == 1 && a.margin_c && *a.margin_c > 9.9 && *a.margin_c < 10.1);
  add_thermal(s, 95, 100);  // margin 5 -> derate 2
  CHECK(f.thermal(&s, t0 + seconds(2)).derate == 2);
  add_thermal(s, 80, 100);  // margin 20: cool enough, but the 15 s hold has not elapsed
  CHECK(f.thermal(&s, t0 + seconds(5)).derate == 2);
  CHECK(f.thermal(&s, t0 + seconds(20)).derate == 1);   // one step down only
  add_thermal(s, 83, 100);  // margin 17 < 15 + 5 hysteresis: stays derated
  CHECK(f.thermal(&s, t0 + seconds(40)).derate == 1);
  add_thermal(s, 70, 100);
  CHECK(f.thermal(&s, t0 + seconds(60)).derate == 0);
  CHECK(f.thermal(nullptr, t0 + seconds(61)).derate == 0);
  monitor::ThermalSample none;
  monitor::ThermalZone z;  // zone without hot/critical trip is ignored
  z.temp_c = 99;
  z.trips.push_back({95, "passive"});
  none.zones.push_back(z);
  CHECK(!thermal_margin(none));

  Feedback p;
  auto cpu_hi = make_sys();
  add_psi(cpu_hi, 40);
  CHECK(!p.pressure(&cpu_hi).cpu && !p.pressure(&cpu_hi).cpu);  // 2nd call
  CHECK(p.pressure(&cpu_hi).cpu);                               // 3rd consecutive call
  auto mid = make_sys();
  add_psi(mid, 20);
  CHECK(p.pressure(&mid).cpu);                                  // between thresholds: sticky
  auto lo = make_sys();
  add_psi(lo, 5, 20, 30);
  auto adv = p.pressure(&lo);
  CHECK(!adv.cpu && adv.memory && adv.io);
  add_psi(lo, 5, 2, 2);
  adv = p.pressure(&lo);
  CHECK(!adv.memory && !adv.io);
}

void test_engine_auto() {
  PolicyEngine e;
  const auto t0 = Clock::now();
  auto sys = make_sys();
  auto compile = wl(WorkloadClass::Compile, workload::kHighLoad);
  auto d = e.evaluate(input(&compile, &sys, t0));
  CHECK(d.profile == Profile::Performance && d.source == ProfileSource::Auto && !d.held);
  CHECK(d.targets.cpu == Bias::Performance && d.targets.io == IoBias::Throughput && !d.reasons.empty());

  // Workload ends: change is held for the dwell time, then applied.
  auto desk = wl(WorkloadClass::Desktop);
  d = e.evaluate(input(&desk, &sys, t0 + seconds(5)));
  CHECK(d.profile == Profile::Performance && d.held);
  d = e.evaluate(input(&desk, &sys, t0 + seconds(25)));
  CHECK(d.profile == Profile::Balanced && !d.held && d.targets.io == IoBias::Latency);

  // No workload information at all -> power-source default, no crash.
  PolicyEngine e2;
  auto d2 = e2.evaluate(input(nullptr, nullptr, t0));
  CHECK(d2.profile == Profile::Balanced);
}

void test_engine_stability() {
  PolicyEngine e;
  const auto t0 = Clock::now();
  auto sys = make_sys();
  auto game = wl(WorkloadClass::Game, workload::kLatencySensitive);
  auto desk = wl(WorkloadClass::Desktop);
  e.evaluate(input(&game, &sys, t0));
  int changes = 0;
  Profile last = e.current().profile;
  Clock::time_point t = t0;
  // Flapping workload every 25 s: dwell grows, so fewer changes than a naive engine would make.
  for (int i = 1; i <= 12; ++i) {
    t = t0 + seconds(25 * i);
    const auto& d = e.evaluate(input(i % 2 ? &desk : &game, &sys, t));
    if (d.profile != last) { ++changes; last = d.profile; }
  }
  CHECK(changes < 12 && changes >= 3);

  // Critical battery is urgent: bypasses dwell.
  PolicyEngine u;
  auto ac = make_sys(true);
  u.evaluate(input(&desk, &ac, t0));
  auto low = make_sys(false, 10);
  auto d = u.evaluate(input(&desk, &low, t0 + seconds(1)));
  CHECK(d.profile == Profile::Powersave && !d.held);
}

void test_engine_user_modes() {
  const auto t0 = Clock::now();
  auto sys = make_sys();
  auto idle = wl(WorkloadClass::Idle);
  auto game = wl(WorkloadClass::Game, workload::kLatencySensitive);

  {  // locked: profile and targets are frozen, whatever the workload
    PolicyEngine e;
    UserMode m{true, Profile::Powersave, true};
    auto d = e.evaluate(input(&game, &sys, t0, m));
    CHECK(d.profile == Profile::Powersave && d.source == ProfileSource::UserLocked);
    CHECK(d.targets == base_targets(Profile::Powersave));
    auto hot = make_sys();
    add_thermal(hot, 95, 100);  // adaptive derating must not apply to a locked profile
    UserMode perf{true, Profile::Performance, true};
    d = e.evaluate(input(&game, &hot, t0 + seconds(1), perf));
    CHECK(d.profile == Profile::Performance && d.targets == base_targets(Profile::Performance) && d.thermal_derate == 2);
  }
  {  // manual (unlocked): profile fixed, targets adapt, thermal derating applies, change is immediate
    PolicyEngine e;
    UserMode m{true, Profile::Performance, false};
    auto d = e.evaluate(input(&game, &sys, t0, m));
    CHECK(d.profile == Profile::Performance && d.source == ProfileSource::User && d.targets.io == IoBias::Latency);
    UserMode m2{true, Profile::Balanced, false};
    d = e.evaluate(input(&game, &sys, t0 + seconds(1), m2));   // user-initiated change: no dwell
    CHECK(d.profile == Profile::Balanced && !d.held);
    auto hot = make_sys();
    add_thermal(hot, 95, 100);  // margin 5 -> derate 2
    d = e.evaluate(input(&idle, &hot, t0 + seconds(2), UserMode{true, Profile::Performance, false}));
    CHECK(d.profile == Profile::Powersave && d.thermal_derate == 2);
  }
  {  // thermal safety EMERGENCY beats a lock
    PolicyEngine e;
    UserMode m{true, Profile::Performance, true};
    auto in = input(&game, &sys, t0, m);
    in.safety = SafetyState::Emergency;
    auto d = e.evaluate(in);
    CHECK(d.profile == Profile::ThermalEmergency && d.source == ProfileSource::Safety && !d.targets.allow_boost);
    in.safety = SafetyState::Normal;
    in.now = t0 + seconds(1);
    d = e.evaluate(in);
    CHECK(d.profile == Profile::Performance && d.source == ProfileSource::UserLocked);
  }
  {  // CPU pressure on AC lifts BALANCED to PERFORMANCE (aggressive level), not on battery
    PolicyEngine e;
    auto busy = make_sys();
    add_psi(busy, 50);
    auto desk = wl(WorkloadClass::Desktop);
    PolicyDecision d;
    for (int i = 0; i < 3; ++i) d = e.evaluate(input(&desk, &busy, t0 + seconds(i)));
    CHECK(d.profile == Profile::Balanced && d.held && d.pressure.cpu);   // proposal held by the dwell time
    d = e.evaluate(input(&desk, &busy, t0 + seconds(30)));
    CHECK(d.profile == Profile::Performance);
    PolicyEngine b;
    auto batt = make_sys(false, 80);
    add_psi(batt, 50);
    for (int i = 0; i < 3; ++i) d = b.evaluate(input(&desk, &batt, t0 + seconds(i)));
    d = b.evaluate(input(&desk, &batt, t0 + seconds(30)));
    CHECK(d.profile == Profile::Balanced && !d.held);
  }
  {  // memory / I/O pressure adjust targets, not the profile
    PolicyEngine e;
    auto s = make_sys();
    add_psi(s, 0, 25, 40);
    auto desk = wl(WorkloadClass::Desktop);
    auto d = e.evaluate(input(&desk, &s, t0));
    CHECK(d.targets.background == Restriction::Strong && d.targets.io == IoBias::Latency && d.pressure.memory);
    auto d1 = PolicyEngine().evaluate(input(&desk, &s, t0, {}, 1));
    CHECK(d1.targets.background == Restriction::Light);  // conservative caps Strong
  }
}

void test_engine_control() {
  const auto t0 = Clock::now();
  CapabilityReport caps;
  caps.add({"cpufreq", CapStatus::Available, ""});
  caps.add({"gpu.drm", CapStatus::Available, ""});
  caps.add({"cgroup.v2", CapStatus::Available, ""});
  caps.add({"io.scheduler", CapStatus::Unavailable, ""});
  std::vector<safety::ExternalTool> tools = {
      {"power-profiles-daemon", "x", {Subsystem::Cpu}, false, false, 7},
      {"GameMode", "y", {Subsystem::Cpu, Subsystem::Gpu, Subsystem::Io}, false, true, 8},
      {"thermald", "z", {}, true, false, 9}};
  auto sys = make_sys();
  auto desk = wl(WorkloadClass::Desktop);
  PolicyEngine e;
  auto in = input(&desk, &sys, t0);
  in.caps = &caps;
  in.tools = &tools;
  auto d = e.evaluate(in);
  CHECK(d.of(Subsystem::Cpu).mode == ControlMode::Observe);          // power-profiles-daemon owns CPU
  CHECK(d.of(Subsystem::Gpu).mode == ControlMode::Active);           // GameMode is game-scoped
  CHECK(d.of(Subsystem::Io).mode == ControlMode::Unavailable);
  CHECK(d.of(Subsystem::Background).mode == ControlMode::Active);
  auto game = wl(WorkloadClass::Game, workload::kLatencySensitive);
  in.workload = &game;
  in.now = t0 + seconds(1);
  d = e.evaluate(in);
  CHECK(d.of(Subsystem::Gpu).mode == ControlMode::Observe);          // do not fight GameMode during a game
  CHECK(d.of(Subsystem::Io).mode == ControlMode::Unavailable);       // missing interface stays unavailable
  const std::string txt = format_decision(d);
  CHECK(txt.find("observe-only") != std::string::npos && txt.find("controllers") != std::string::npos);
}

void test_store() {
  char t[] = "/tmp/opt-pol-XXXXXX";
  const fs::path dir = ::mkdtemp(t);
  const fs::path f = dir / "state/policy.state";
  auto def = load_user_mode(f);
  CHECK(def && !def->manual);
  UserMode m{true, Profile::Performance, true};
  CHECK(save_user_mode(f, m).has_value());
  auto back = load_user_mode(f);
  CHECK(back && *back == m);
  CHECK(save_user_mode(f, UserMode{}).has_value());
  CHECK(load_user_mode(f) && !load_user_mode(f)->manual);
  CHECK(!fs::exists(f.string() + ".tmp"));
  { std::ofstream(f, std::ios::trunc) << "mode=manual\nprofile=turbo\nlocked=1\n"; }
  CHECK(!load_user_mode(f));
  { std::ofstream(f, std::ios::trunc) << "mode=manual\nprofile=balanced\nlocked=2\n"; }
  CHECK(!load_user_mode(f));
  { std::ofstream(f, std::ios::trunc) << "garbage\n"; }
  CHECK(!load_user_mode(f));
  fs::remove_all(dir);
}

void test_conflict_detector() {
  workload::ProcessSnapshot ps;
  auto add = [&](int pid, const std::string& comm) {
    workload::ProcessInfo p;
    p.pid = pid;
    p.comm = comm;
    ps.procs.push_back(p);
  };
  add(1, "systemd");
  add(2, "power-profiles-");
  add(3, "gamemoded");
  add(4, "thermald");
  add(5, "tlp");
  add(6, "tlpx");  // must not match tlp
  add(7, "bash");
  ps.finalize();
  auto tools = safety::detect_running_tools(ps);
  CHECK(tools.size() == 4);
  bool ppd = false, gm = false, th = false, tlp = false;
  for (const auto& t : tools) {
    if (t.name == "power-profiles-daemon") ppd = !t.subsystems.empty() && t.subsystems[0] == Subsystem::Cpu;
    if (t.name == "GameMode") gm = t.game_scoped;
    if (t.name == "thermald") th = t.thermal && t.subsystems.empty();
    if (t.name == "TLP") tlp = t.pid == 5;
  }
  CHECK(ppd && gm && th && tlp);
}

void test_scheduler_run_soon() {
  std::atomic<int> n{0};
  Scheduler s;
  s.add("slow", std::chrono::seconds(60), [&] { ++n; });
  s.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(n == 1);
  s.run_soon("slow");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(n == 2);
  s.stop();
}

}  // namespace

void run_policy_tests() {
  log::set_level(log::Level::Safety);
  test_profiles();
  test_rules();
  test_feedback();
  test_engine_auto();
  test_engine_stability();
  test_engine_user_modes();
  test_engine_control();
  test_store();
  test_conflict_detector();
  test_scheduler_run_soon();
}
