#include <filesystem>
#include <fstream>
#include <map>

#include "check.hpp"
#include "control/cgroup.hpp"
#include "control/control_manager.hpp"
#include "control/cpufreq.hpp"
#include "control/file_state_writer.hpp"
#include "control/gpu.hpp"
#include "control/io.hpp"
#include "control/memory.hpp"
#include "control/runtime_pm.hpp"
#include "control/uclamp.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "core/sysfs.hpp"
#include "policy/profiles.hpp"

namespace fs = std::filesystem;
using namespace opt;
using namespace opt::control;
using std::chrono::seconds;
using std::chrono::minutes;

namespace {

fs::path tmpdir() {
  char t[] = "/tmp/opt-ctl-XXXXXX";
  return ::mkdtemp(t);
}
void put(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::trunc) << content << "\n";
}
std::string get(const fs::path& root, const std::string& target) { return sysfs::read_text(root / target.substr(1)).value_or("<missing>"); }

const char* kEppList = "default performance balance_performance balance_power power";

void make_machine(const fs::path& r) {
  for (const char* pol : {"policy0", "policy1"}) {
    const fs::path d = r / "sys/devices/system/cpu/cpufreq" / pol;
    put(d / "cpuinfo_min_freq", "400000");
    put(d / "cpuinfo_max_freq", "4400000");
    put(d / "scaling_driver", "intel_pstate");
    put(d / "scaling_governor", "powersave");
    put(d / "scaling_available_governors", "performance powersave");
    put(d / "energy_performance_available_preferences", kEppList);
    put(d / "energy_performance_preference", "balance_performance");
    put(d / "scaling_max_freq", "4400000");
  }
  put(r / "sys/devices/system/cpu/intel_pstate/no_turbo", "0");
  const fs::path card = r / "sys/class/drm/card0";
  fs::create_directories(card / "device");
  fs::create_directories(r / "drivers/i915");
  fs::create_symlink(r / "drivers/i915", card / "device/driver");
  put(card / "gt_min_freq_mhz", "300");
  put(card / "gt_max_freq_mhz", "1100");
  put(card / "gt_RP0_freq_mhz", "1100");
  put(card / "gt_RPn_freq_mhz", "300");
  put(r / "proc/sys/vm/swappiness", "60");
  put(r / "sys/block/sda/queue/rotational", "1");
  put(r / "sys/block/sda/queue/scheduler", "[mq-deadline] kyber bfq none");
  put(r / "sys/block/nvme0n1/queue/rotational", "0");
  put(r / "sys/block/nvme0n1/queue/scheduler", "[none] mq-deadline");
  put(r / "sys/fs/cgroup/system.slice/cpu.weight", "100");
  const fs::path pci = r / "sys/bus/pci/devices";
  put(pci / "0000:00:02.0/class", "0x030000");  put(pci / "0000:00:02.0/power/control", "on");
  put(pci / "0000:00:00.0/class", "0x060000");  put(pci / "0000:00:00.0/power/control", "on");
  put(pci / "0000:00:14.0/class", "0x0c0330");  put(pci / "0000:00:14.0/power/control", "on");   // USB: excluded
  put(pci / "0000:00:1f.3/class", "0x040100");  put(pci / "0000:00:1f.3/power/control", "on");   // audio: excluded
  put(pci / "0000:00:1c.0/class", "0x060400");  put(pci / "0000:00:1c.0/power/control", "auto"); // already auto
}

CapabilityReport make_caps(bool swap = true, bool cgroup = true) {
  CapabilityReport c;
  c.add({"cpufreq", CapStatus::Available, ""});
  c.add({"mem.swap", swap ? CapStatus::Available : CapStatus::Unavailable, ""});
  c.add({"cgroup.v2", cgroup ? CapStatus::Available : CapStatus::Unavailable, ""});
  c.add({"gpu.drm", CapStatus::Available, ""});
  c.add({"io.scheduler", CapStatus::Available, ""});
  return c;
}

policy::PolicyDecision dec(Profile p) {
  policy::PolicyDecision d;
  d.profile = p;
  d.targets = policy::base_targets(p);
  return d;
}

monitor::SystemSnapshot battery_sys(bool on_ac) {
  monitor::SystemSnapshot s;
  s.power = monitor::PowerSample{};
  s.power->on_ac = on_ac;
  return s;
}

const Desired* find(const std::vector<Desired>& v, const std::string& suffix) {
  for (const auto& d : v)
    if (d.target.size() >= suffix.size() && d.target.compare(d.target.size() - suffix.size(), suffix.size(), suffix) == 0) return &d;
  return nullptr;
}

// ---------------- writer ----------------
void test_writer() {
  const auto root = tmpdir();
  make_machine(root);
  FileStateWriter w(root);
  const std::string epp = "/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference";
  CHECK(w.read(epp).value_or("") == "balance_performance");
  CHECK(w.write(epp, "power").has_value() && get(root, epp) == "power");
  CHECK(w.read("/sys/block/sda/queue/scheduler").value_or("") == "mq-deadline");  // bracketed active choice
  CHECK(normalize_value("/sys/fs/cgroup/x/io.weight", "default 100\n8:0 50") == "default 100");
  CHECK(normalize_value("/x/y", "plain") == "plain");

  CHECK(w.write("/etc/passwd", "x").error().code == Errc::PermissionDenied);
  CHECK(w.read("/etc/passwd").error().code == Errc::PermissionDenied);
  CHECK(w.write(epp, "a\nb").error().code == Errc::InvalidArgument);
  CHECK(w.write(epp, "").error().code == Errc::InvalidArgument);
  CHECK(w.write(epp, std::string(200, 'x')).error().code == Errc::InvalidArgument);
  const std::string missing = "/sys/devices/system/cpu/cpufreq/policy9/scaling_governor";
  CHECK(w.write(missing, "performance").error().code == Errc::NotFound && !fs::exists(root / missing.substr(1)));  // never creates files
  CHECK(w.write("/sys/devices/system/cpu/cpufreq/policy0/../policy1/scaling_governor", "performance").error().code == Errc::PermissionDenied);

  // A symlink that escapes /sys is refused and the outside file is untouched.
  put(root / "outside/secret", "keep");
  const fs::path gov = root / "sys/devices/system/cpu/cpufreq/policy0/scaling_governor";
  fs::remove(gov);
  fs::create_symlink(root / "outside/secret", gov);
  const auto r = w.write("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "performance");
  CHECK(!r && r.error().code == Errc::PermissionDenied && get(root, "/outside/secret") == "keep");
  // A symlink that stays inside /sys is fine (e.g. /sys/class/... -> /sys/devices/...).
  fs::remove(gov);
  put(root / "sys/devices/real/governor", "powersave");
  fs::create_symlink(root / "sys/devices/real/governor", gov);
  CHECK(w.write("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "performance").has_value() &&
        get(root, "/sys/devices/real/governor") == "performance");
  fs::remove_all(root);
}

// ---------------- planners ----------------
void test_planners() {
  const auto root = tmpdir();
  make_machine(root);
  const auto caps = make_caps();
  const auto limits = safety::HardwareLimits::discover(root);
  CHECK(limits.cpu_khz && limits.gpu_mhz);
  auto ctx = [&](const policy::PolicyDecision& d, const monitor::SystemSnapshot* s = nullptr, bool rpm = false) {
    return ControlContext{root, d, caps, limits, s, rpm};
  };

  {  // CPU
    auto d = dec(Profile::Performance);
    auto p = plan_cpufreq(ctx(d));
    CHECK(p.size() == 2 && p[0].value == "performance" && p[0].raises_performance && !p[0].safety_action);
    CHECK(p[0].target == "/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference");
    auto pw = plan_cpufreq(ctx(dec(Profile::Powersave)));
    CHECK(find(pw, "policy0/energy_performance_preference") && find(pw, "policy0/energy_performance_preference")->value == "power");
    CHECK(find(pw, "no_turbo") && find(pw, "no_turbo")->value == "1" && !find(pw, "no_turbo")->safety_action);  // powersave: no boost
    CHECK(plan_cpufreq(ctx(dec(Profile::Balanced))).empty());                                                   // hands off
    auto em = plan_cpufreq(ctx(dec(Profile::ThermalEmergency)));
    const auto* cap = find(em, "policy1/scaling_max_freq");
    CHECK(cap && cap->value == "2200000" && cap->safety_action && !cap->raises_performance && cap->limit_kind == safety::LimitKind::CpuFreqKhz);
    CHECK(find(em, "no_turbo") && find(em, "no_turbo")->safety_action);
    // No EPP interface: governor fallback only for PERFORMANCE; POWERSAVE never forces the powersave governor.
    fs::remove(root / "sys/devices/system/cpu/cpufreq/policy0/energy_performance_available_preferences");
    fs::remove(root / "sys/devices/system/cpu/cpufreq/policy1/energy_performance_available_preferences");
    auto noepp = plan_cpufreq(ctx(d));
    CHECK(noepp.size() == 2 && noepp[0].target.ends_with("scaling_governor") && noepp[0].value == "performance");
    for (const auto& x : plan_cpufreq(ctx(dec(Profile::Powersave)))) CHECK(!x.target.ends_with("scaling_governor"));
    put(root / "sys/devices/system/cpu/cpufreq/policy0/energy_performance_available_preferences", kEppList);
    put(root / "sys/devices/system/cpu/cpufreq/policy1/energy_performance_available_preferences", kEppList);
    // boost fallback interface
    fs::remove(root / "sys/devices/system/cpu/intel_pstate/no_turbo");
    put(root / "sys/devices/system/cpu/cpufreq/boost", "1");
    auto b = plan_cpufreq(ctx(dec(Profile::Powersave)));
    CHECK(find(b, "cpufreq/boost") && find(b, "cpufreq/boost")->value == "0");
    put(root / "sys/devices/system/cpu/intel_pstate/no_turbo", "0");
    fs::remove(root / "sys/devices/system/cpu/cpufreq/boost");
  }
  {  // GPU
    auto perf = plan_gpu(ctx(dec(Profile::Performance)));
    CHECK(perf.size() == 1 && perf[0].target == "/sys/class/drm/card0/gt_min_freq_mhz" && perf[0].value == "700" && perf[0].raises_performance);
    auto pw = plan_gpu(ctx(dec(Profile::Powersave)));
    CHECK(pw.size() == 1 && pw[0].target.ends_with("gt_max_freq_mhz") && pw[0].value == "700" && !pw[0].raises_performance);
    auto em = plan_gpu(ctx(dec(Profile::ThermalEmergency)));
    CHECK(em.size() == 1 && em[0].value == "500" && em[0].safety_action);
    CHECK(plan_gpu(ctx(dec(Profile::Balanced))).empty());
    safety::HardwareLimits none;
    CHECK(plan_gpu(ControlContext{root, dec(Profile::Performance), caps, none, nullptr, false}).empty());  // unknown limits: nothing
    fs::remove(root / "sys/class/drm/card0/device/driver");
    fs::create_directories(root / "drivers/nouveau");
    fs::create_symlink(root / "drivers/nouveau", root / "sys/class/drm/card0/device/driver");
    CHECK(plan_gpu(ctx(dec(Profile::Performance))).empty());  // unknown driver: never guessed
  }
  {  // memory
    auto d = dec(Profile::Balanced);
    d.targets.memory = policy::MemoryBias::Responsiveness;
    auto m = plan_memory(ctx(d));
    CHECK(m.size() == 1 && m[0].target == "/proc/sys/vm/swappiness" && m[0].value == "30");
    put(root / "proc/sys/vm/swappiness", "20");
    CHECK(plan_memory(ctx(d)).empty());                     // never raises swappiness
    put(root / "proc/sys/vm/swappiness", "60");
    d.pressure.memory = true;
    CHECK(plan_memory(ctx(d)).empty());                     // not while memory pressure is high
    d.pressure.memory = false;
    const auto noswap = make_caps(false);
    CHECK(plan_memory(ControlContext{root, d, noswap, limits, nullptr, false}).empty());
    d.targets.memory = policy::MemoryBias::Throughput;
    CHECK(plan_memory(ctx(d)).empty());                     // dirty ratios are never touched
  }
  {  // I/O
    auto d = dec(Profile::Balanced);
    d.targets.io = policy::IoBias::Latency;
    auto lat = plan_io(ctx(d));
    CHECK(lat.size() == 1 && lat[0].target == "/sys/block/sda/queue/scheduler" && lat[0].value == "bfq");  // nvme untouched
    d.targets.io = policy::IoBias::Throughput;
    CHECK(plan_io(ctx(d)).at(0).value == "mq-deadline");
    d.targets.io = policy::IoBias::Balanced;
    CHECK(plan_io(ctx(d)).empty());
    put(root / "sys/block/sda/queue/scheduler", "[mq-deadline] none");
    d.targets.io = policy::IoBias::Latency;
    CHECK(plan_io(ctx(d)).empty());                         // bfq not available
    put(root / "sys/block/sda/queue/scheduler", "[mq-deadline] kyber bfq none");
  }
  {  // cgroup
    auto d = dec(Profile::Balanced);
    CHECK(plan_cgroup(ctx(d)).empty());
    d.targets.background = policy::Restriction::Light;
    auto l = plan_cgroup(ctx(d));
    CHECK(l.size() == 1 && l[0].value == "50" && l[0].target == "/sys/fs/cgroup/system.slice/cpu.weight");
    d.targets.background = policy::Restriction::Strong;
    CHECK(plan_cgroup(ctx(d)).at(0).value == "20");
    CHECK(plan_cgroup(ControlContext{root, d, make_caps(true, false), limits, nullptr, false}).empty());
    fs::remove(root / "sys/fs/cgroup/system.slice/cpu.weight");
    CHECK(plan_cgroup(ctx(d)).empty());                     // cpu controller not enabled for the slice
    put(root / "sys/fs/cgroup/system.slice/cpu.weight", "100");
  }
  {  // runtime PM: opt-in, battery, selected classes only
    auto d = dec(Profile::Powersave);
    auto bat = battery_sys(false), ac = battery_sys(true);
    CHECK(plan_runtime_pm(ctx(d, &bat, false)).empty());    // not opted in
    CHECK(plan_runtime_pm(ctx(d, &ac, true)).empty());      // on AC
    CHECK(plan_runtime_pm(ctx(d, nullptr, true)).empty());  // unknown power source
    auto p = plan_runtime_pm(ctx(d, &bat, true));
    CHECK(p.size() == 2 && p[0].target == "/sys/bus/pci/devices/0000:00:00.0/power/control" && p[1].target.find("0000:00:02.0") != std::string::npos);
    CHECK(p[0].value == "auto");
    CHECK(plan_runtime_pm(ctx(dec(Profile::Balanced), &bat, true)).empty());
  }
  CHECK(plan_uclamp(ctx(dec(Profile::Performance))).empty());
  fs::remove_all(root);
}

// ---------------- gate + safety actions ----------------
void test_gate_safety_action() {
  using namespace safety;
  safety::HardwareLimits limits;
  limits.cpu_khz = Range{400000, 4400000};
  policy::PolicyDecision observe;
  observe.control[static_cast<std::size_t>(policy::Subsystem::Cpu)] = {policy::ControlMode::Observe, "PPD"};
  ChangeIntent c;
  c.subsystem = policy::Subsystem::Cpu;
  c.target = "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq";
  c.value = "2200000";
  c.limit_kind = LimitKind::CpuFreqKhz;
  c.numeric = 2200000;
  GateContext ctx{&observe, SafetyState::Emergency, &limits};
  CHECK(!check_change(c, ctx).allowed);        // normal change on an observe-only subsystem: blocked
  c.safety_action = true;
  CHECK(check_change(c, ctx).allowed);         // protective throttle may act regardless of ownership
  GateContext no_decision{nullptr, SafetyState::Emergency, &limits};
  CHECK(check_change(c, no_decision).allowed);
  auto raise = c;
  raise.raises_performance = true;
  CHECK(check_change(raise, ctx).stage == GateStage::Thermal);   // never raises
  auto over = c;
  over.numeric = 5000000;
  CHECK(check_change(over, ctx).stage == GateStage::Limits);     // still bound by hardware limits
  auto evil = c;
  evil.target = "/etc/shadow";
  CHECK(check_change(evil, ctx).stage == GateStage::Target);     // and by the allowlist
  auto unlimited = c;
  GateContext nolim{&observe, SafetyState::Emergency, nullptr};
  CHECK(check_change(unlimited, nolim).stage == GateStage::Limits);
}

// ---------------- rollback upgrades ----------------
struct NormalizingWriter final : safety::StateWriter {
  std::map<std::string, std::string> files;
  Result<std::string> read(const std::string& t) override {
    if (!files.count(t)) return fail(make_error(Errc::NotFound, "none"));
    return files[t];
  }
  Result<void> write(const std::string& t, const std::string& v) override {
    std::string n = v;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));  // kernel normalises case
    files[t] = n;
    return {};
  }
};

void test_rollback_upgrades() {
  const auto d = tmpdir();
  const std::string t = "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor";
  {  // normalised value is journaled as it really is, so rollback recognises it
    NormalizingWriter w;
    w.files[t] = "powersave";
    safety::RollbackManager m(std::move(*safety::RollbackJournal::open(d / "a")), w);
    CHECK(m.apply({"cpu", t, "PERFORMANCE", "r", "P"}).has_value() && w.files[t] == "performance");
    CHECK(m.outstanding()[0].new_value == "performance");
    {  // survives a restart: the "A id actual" line is parsed back
      safety::RollbackManager again(std::move(*safety::RollbackJournal::open(d / "a")), w);
      CHECK(again.outstanding_count() == 1 && again.outstanding()[0].new_value == "performance" && again.outstanding()[0].applied);
    }
    CHECK(m.rollback_all("x").restored == 1 && w.files[t] == "powersave");
  }
  {  // chain collapse: one record keeps the ORIGINAL value
    NormalizingWriter w;
    w.files[t] = "powersave";
    safety::RollbackManager m(std::move(*safety::RollbackJournal::open(d / "b")), w);
    CHECK(m.apply({"cpu", t, "ondemand", "r", "P"}).has_value() && m.apply({"cpu", t, "performance", "r", "P"}).has_value() &&
          m.apply({"cpu", t, "schedutil", "r", "P"}).has_value());
    CHECK(m.outstanding_count() == 1 && m.outstanding()[0].old_value == "powersave");
    CHECK(m.rollback_all("x").restored == 1 && w.files[t] == "powersave");
    // an external change breaks the chain instead of being swallowed
    CHECK(m.apply({"cpu", t, "performance", "r", "P"}).has_value());
    w.files[t] = "conservative";
    CHECK(m.apply({"cpu", t, "schedutil", "r", "P"}).has_value() && m.outstanding_count() == 2);
  }
  {  // rollback_target touches only that target
    NormalizingWriter w;
    w.files[t] = "a";
    w.files["/proc/sys/vm/swappiness"] = "60";
    safety::RollbackManager m(std::move(*safety::RollbackJournal::open(d / "c")), w);
    (void)m.apply({"cpu", t, "b", "r", "P"});
    (void)m.apply({"memory", "/proc/sys/vm/swappiness", "10", "r", "P"});
    CHECK(m.rollback_target("/proc/sys/vm/swappiness", "x").restored == 1 && w.files[t] == "b" && w.files["/proc/sys/vm/swappiness"] == "60");
    CHECK(m.outstanding_count() == 1);
  }
  fs::remove_all(d);
}

// ---------------- manager ----------------
struct Rig {
  fs::path root = tmpdir();
  fs::path state = tmpdir();
  FileStateWriter writer{root};
  CapabilityReport caps = make_caps();
  safety::HardwareLimits limits;
  std::unique_ptr<safety::RollbackManager> rollback;
  std::unique_ptr<ControlManager> mgr;
  Rig(ControlsMode mode = ControlsMode::Apply, bool journal = true) {
    make_machine(root);
    limits = safety::HardwareLimits::discover(root);
    if (journal) rollback = std::make_unique<safety::RollbackManager>(std::move(*safety::RollbackJournal::open(state / "j")), writer);
    mgr = std::make_unique<ControlManager>(root, rollback.get(), writer, caps, limits, mode, false);
  }
  ~Rig() {
    mgr.reset();
    fs::remove_all(root);
    fs::remove_all(state);
  }
  std::string v(const std::string& t) { return get(root, t); }
};
const std::string kEpp0 = "/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference";
const std::string kEpp1 = "/sys/devices/system/cpu/cpufreq/policy1/energy_performance_preference";
const std::string kGpuMin = "/sys/class/drm/card0/gt_min_freq_mhz";
const std::string kMax0 = "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq";
const std::string kTurbo = "/sys/devices/system/cpu/intel_pstate/no_turbo";

void test_manager_apply_and_revert() {
  Rig r;
  const auto t0 = Clock::now();
  auto rep = r.mgr->apply(dec(Profile::Performance), SafetyState::Normal, nullptr, t0);
  CHECK(rep.applied == 4 && rep.failed == 0 && rep.blocked == 0);   // 2x EPP, GPU min, system.slice weight (Performance = light background restriction)
  CHECK(r.v(kEpp0) == "performance" && r.v(kEpp1) == "performance" && r.v(kGpuMin) == "700");
  CHECK(r.rollback->outstanding_count() == 4 && r.v("/sys/fs/cgroup/system.slice/cpu.weight") == "50");
  rep = r.mgr->apply(dec(Profile::Performance), SafetyState::Normal, nullptr, t0 + seconds(2));
  CHECK(rep.applied == 0 && r.rollback->outstanding_count() == 4);      // idempotent: "already set"
  rep = r.mgr->apply(dec(Profile::Balanced), SafetyState::Normal, nullptr, t0 + seconds(30));
  CHECK(rep.reverted == 4 && rep.applied == 0 && r.rollback->outstanding_count() == 0 && r.v("/sys/fs/cgroup/system.slice/cpu.weight") == "100");
  CHECK(r.v(kEpp0) == "balance_performance" && r.v(kEpp1) == "balance_performance" && r.v(kGpuMin) == "300");  // hands-off restores originals
  // Powersave then Performance: profile changes move values, never leave stale ones behind.
  rep = r.mgr->apply(dec(Profile::Powersave), SafetyState::Normal, nullptr, t0 + seconds(60));
  CHECK(r.v(kEpp0) == "power" && r.v(kTurbo) == "1" && r.v("/sys/class/drm/card0/gt_max_freq_mhz") == "700");
  rep = r.mgr->apply(dec(Profile::Performance), SafetyState::Normal, nullptr, t0 + seconds(90));
  CHECK(r.v(kEpp0) == "performance" && r.v(kTurbo) == "0" && r.v("/sys/class/drm/card0/gt_max_freq_mhz") == "1100" && r.v(kGpuMin) == "700");
  CHECK(r.mgr->apply(dec(Profile::Balanced), SafetyState::Normal, nullptr, t0 + seconds(120)).reverted >= 3);
  CHECK(r.rollback->outstanding_count() == 0 && r.v(kGpuMin) == "300" && r.v(kEpp0) == "balance_performance");
}

void test_manager_ownership() {
  Rig r;
  const auto t0 = Clock::now();
  r.mgr->apply(dec(Profile::Performance), SafetyState::Normal, nullptr, t0);
  auto observe = dec(Profile::Performance);   // power-profiles-daemon appears and takes the CPU
  observe.control[static_cast<std::size_t>(policy::Subsystem::Cpu)] = {policy::ControlMode::Observe, "power-profiles-daemon owns this subsystem"};
  auto rep = r.mgr->apply(observe, SafetyState::Normal, nullptr, t0 + seconds(5));
  CHECK(rep.skipped >= 1 && r.v(kEpp0) == "balance_performance" && r.v(kEpp1) == "balance_performance");  // released
  CHECK(r.v(kGpuMin) == "700");                                                                           // GPU still ours
  bool noted = false;
  for (const auto& a : rep.actions) noted |= a.outcome.find("observe-only") != std::string::npos;
  CHECK(noted);
}

void test_manager_modes() {
  {  // dry-run writes nothing but reports
    Rig r(ControlsMode::DryRun);
    auto rep = r.mgr->apply(dec(Profile::Performance), SafetyState::Normal);
    CHECK(rep.would_apply == 4 && rep.applied == 0 && r.v(kEpp0) == "balance_performance" && r.v(kGpuMin) == "300");
    CHECK(r.rollback->outstanding_count() == 0);
    CHECK(format_controls(rep, {}).find("would apply") != std::string::npos && format_controls(rep, {}).find("writes NOTHING") != std::string::npos);
  }
  {  // off does nothing at all
    Rig r(ControlsMode::Off);
    auto rep = r.mgr->apply(dec(Profile::Performance), SafetyState::Normal);
    CHECK(rep.actions.empty() && rep.note == "controllers are off" && r.v(kEpp0) == "balance_performance");
  }
  {  // leaving APPLY restores everything
    Rig r;
    r.mgr->apply(dec(Profile::Performance), SafetyState::Normal);
    CHECK(r.v(kEpp0) == "performance");
    r.mgr->set_mode(ControlsMode::DryRun);
    CHECK(r.v(kEpp0) == "balance_performance" && r.rollback->outstanding_count() == 0);
  }
  {  // no journal => refuse to change anything
    Rig r(ControlsMode::Apply, false);
    auto rep = r.mgr->apply(dec(Profile::Performance), SafetyState::Normal);
    CHECK(rep.applied == 0 && rep.failed == 4 && r.v(kEpp0) == "balance_performance");
  }
  CHECK(parse_controls_mode("apply") == ControlsMode::Apply && parse_controls_mode("dry-run") == ControlsMode::DryRun &&
        parse_controls_mode("off") == ControlsMode::Off && !parse_controls_mode("on"));
}

void test_manager_thermal() {
  {  // WARM: raising performance is blocked at the gate; nothing is written
    Rig r;
    auto rep = r.mgr->apply(dec(Profile::Performance), SafetyState::Warm);
    CHECK(rep.blocked == 3 && rep.applied == 1 && r.v(kEpp0) == "balance_performance" && r.v(kGpuMin) == "300");  // only the non-raising background weight goes through
    bool stage = false;
    for (const auto& a : rep.actions) stage |= a.outcome.find("blocked (thermal)") != std::string::npos;
    CHECK(stage);
  }
  {  // EMERGENCY: protective throttle works even though another tool owns the CPU; lowering is allowed
    Rig r;
    auto d = ControlManager::emergency_decision();
    d.control[static_cast<std::size_t>(policy::Subsystem::Cpu)] = {policy::ControlMode::Observe, "PPD"};
    auto rep = r.mgr->apply(d, SafetyState::Emergency);
    CHECK(r.v(kMax0) == "2200000" && r.v("/sys/devices/system/cpu/cpufreq/policy1/scaling_max_freq") == "2200000" && r.v(kTurbo) == "1");
    CHECK(r.v("/sys/class/drm/card0/gt_max_freq_mhz") == "500");
    CHECK(r.v(kEpp0) == "balance_performance");  // the non-safety EPP change respects ownership
    CHECK(rep.applied >= 5);
    // Recovery: once the state is normal and the profile is BALANCED everything is restored.
    auto back = r.mgr->apply(dec(Profile::Balanced), SafetyState::Normal);
    CHECK(back.reverted >= 5 && r.v(kMax0) == "4400000" && r.v(kTurbo) == "0" && r.v("/sys/class/drm/card0/gt_max_freq_mhz") == "1100");
    CHECK(r.rollback->outstanding_count() == 0);
  }
  {  // hardware limits still bind a safety action
    Rig r;
    r.limits.cpu_khz = safety::Range{400000, 2000000};  // pretend firmware allows less than 50% of cpuinfo max
    auto rep = r.mgr->apply(ControlManager::emergency_decision(), SafetyState::Emergency);
    CHECK(r.v(kMax0) == "4400000");
    bool limits = false;
    for (const auto& a : rep.actions) limits |= a.outcome.find("blocked (limits)") != std::string::npos;
    CHECK(limits);
  }
}

void test_manager_backoff_and_recovery() {
  Rig r;
  const auto t0 = Clock::now();
  r.mgr->apply(dec(Profile::Performance), SafetyState::Normal, nullptr, t0);
  put(r.root / kEpp0.substr(1), "balance_power");  // someone else (e.g. a power daemon) changes it
  auto rep = r.mgr->apply(dec(Profile::Performance), SafetyState::Normal, nullptr, t0 + seconds(3));
  bool external = false;
  for (const auto& a : rep.actions) external |= a.outcome.find("changed externally") != std::string::npos;
  CHECK(external && r.v(kEpp0) == "balance_power");                       // we did not fight back
  rep = r.mgr->apply(dec(Profile::Performance), SafetyState::Normal, nullptr, t0 + seconds(10));
  bool backing = false;
  for (const auto& a : rep.actions) backing |= a.outcome.find("backing off") != std::string::npos;
  CHECK(backing && r.v(kEpp0) == "balance_power");
  // Reverting later must not overwrite the other tool's value either.
  r.mgr->apply(dec(Profile::Balanced), SafetyState::Normal, nullptr, t0 + seconds(40));
  CHECK(r.v(kEpp0) == "balance_power" && r.v(kEpp1) == "balance_performance" && r.rollback->outstanding_count() == 0);

  // Crash recovery with the real writer: a fresh manager over the same journal restores the system.
  Rig c;
  c.mgr->apply(dec(Profile::Performance), SafetyState::Normal);
  CHECK(c.v(kGpuMin) == "700");
  c.mgr.reset();
  c.rollback.reset();  // "crash"
  safety::RollbackManager recovered(std::move(*safety::RollbackJournal::open(c.state / "j")), c.writer);
  CHECK(recovered.outstanding_count() == 4);
  auto rec = recovered.rollback_all("recovery after restart");
  CHECK(rec.ok() && rec.restored == 4 && c.v(kGpuMin) == "300" && c.v(kEpp0) == "balance_performance");
}

void test_config_controllers() {
  auto c = parse_config("[controllers]\nmode = apply\nruntime_pm = on\n");
  CHECK(c && c->controllers_mode == "apply" && c->runtime_pm);
  auto d = parse_config("");
  CHECK(d && d->controllers_mode == "dry-run" && !d->runtime_pm);  // safe defaults
  CHECK(!parse_config("[controllers]\nmode = yes\n"));
  CHECK(!parse_config("[controllers]\nruntime_pm = maybe\n"));
  CHECK(!parse_config("[controllers]\nbogus = 1\n"));
}

}  // namespace

void run_control_tests() {
  log::set_level(log::Level::Safety);
  test_writer();
  test_planners();
  test_gate_safety_action();
  test_rollback_upgrades();
  test_manager_apply_and_revert();
  test_manager_ownership();
  test_manager_modes();
  test_manager_thermal();
  test_manager_backoff_and_recovery();
  test_config_controllers();
}
