#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

#include "check.hpp"
#include "core/atomic_file.hpp"
#include "core/logging.hpp"
#include "core/sysfs.hpp"
#include "policy/policy_engine.hpp"
#include "safety/hardware_limits.hpp"
#include "safety/rollback.hpp"
#include "safety/safety_gate.hpp"
#include "safety/thermal_guard.hpp"
#include "safety/thermal_safety.hpp"

namespace fs = std::filesystem;
using namespace opt;
using namespace opt::safety;
using std::chrono::seconds;

namespace {

fs::path tmpdir() {
  char t[] = "/tmp/opt-saf-XXXXXX";
  return ::mkdtemp(t);
}
void put(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::trunc) << content << "\n";
}
bool near(double a, double b, double e = 0.01) { return a > b - e && a < b + e; }

ThermalSensor sensor(const std::string& name, SensorKind k, double temp, std::optional<double> limit = std::nullopt) {
  ThermalSensor s;
  s.name = name;
  s.source = "test";
  s.kind = k;
  s.temp_c = temp;
  s.limit_c = limit;
  s.limit_source = limit ? "test limit" : "";
  return s;
}

void test_atomic_file() {
  const auto d = tmpdir();
  const auto f = d / "a/b/state";
  CHECK(write_file_atomic(f, "one\n").has_value());
  CHECK(write_file_atomic(f, "two\n").has_value());
  CHECK(sysfs::read_text(f) == "two");
  CHECK(!fs::exists(f.string() + ".tmp"));
  fs::remove_all(d);
}

void test_sensors() {
  const auto root = tmpdir();
  const fs::path th = root / "sys/class/thermal";
  put(th / "thermal_zone0/type", "acpitz");
  put(th / "thermal_zone0/temp", "27800");
  put(th / "thermal_zone0/trip_point_0_temp", "105000");
  put(th / "thermal_zone0/trip_point_0_type", "critical");
  put(th / "thermal_zone0/trip_point_1_temp", "100000");
  put(th / "thermal_zone0/trip_point_1_type", "hot");
  put(th / "thermal_zone0/trip_point_2_temp", "55000");
  put(th / "thermal_zone0/trip_point_2_type", "active");
  put(th / "thermal_zone5/type", "x86_pkg_temp");
  put(th / "thermal_zone5/temp", "64000");
  put(th / "thermal_zone5/trip_point_0_temp", "-274000");
  put(th / "thermal_zone5/trip_point_0_type", "passive");
  put(th / "thermal_zone4/type", "iwlwifi_1");
  put(th / "thermal_zone4/temp", "46000");
  put(th / "thermal_zone9/type", "broken");  // no temp: skipped
  put(root / "sys/class/hwmon/hwmon2/name", "coretemp");
  put(root / "sys/class/hwmon/hwmon2/temp1_input", "61000");
  put(root / "sys/class/hwmon/hwmon2/temp1_label", "Package id 0");
  put(root / "sys/class/hwmon/hwmon2/temp1_max", "90000");
  put(root / "sys/class/hwmon/hwmon2/temp1_crit", "100000");
  put(root / "sys/class/hwmon/hwmon1/name", "nvme");
  put(root / "sys/class/hwmon/hwmon1/temp1_input", "40000");
  put(root / "sys/class/hwmon/hwmon1/temp1_crit", "85000");
  const auto s = read_thermal_sensors(root);
  auto find = [&](const std::string& n) -> const ThermalSensor* {
    for (const auto& x : s)
      if (x.name == n) return &x;
    return nullptr;
  };
  CHECK(find("acpitz") && find("acpitz")->limit_c && near(*find("acpitz")->limit_c, 100.0) && find("acpitz")->kind == SensorKind::Platform);
  CHECK(find("x86_pkg_temp") && !find("x86_pkg_temp")->limit_c && find("x86_pkg_temp")->kind == SensorKind::Cpu);
  CHECK(find("iwlwifi_1") && find("iwlwifi_1")->kind == SensorKind::Other);
  CHECK(!find("broken"));
  const auto* ct = find("coretemp:Package id 0");
  CHECK(ct && ct->kind == SensorKind::Cpu && near(*ct->limit_c, 100.0) && ct->limit_source == "hardware crit");
  CHECK(find("nvme:temp1") && find("nvme:temp1")->kind == SensorKind::Other);  // storage is informational only
  CHECK(read_thermal_sensors(root / "none").empty());
  fs::remove_all(root);
}

void test_state_machine() {
  ThermalSafety t;
  const auto t0 = Clock::now();
  auto at = [&](double temp, int sec) { return t.update({sensor("cpu", SensorKind::Cpu, temp, 100.0)}, t0 + seconds(sec)); };
  CHECK(at(60, 0) == SafetyState::Normal && t.status().monitoring);
  CHECK(at(86, 1) == SafetyState::Normal);   // 1st reading above WARM entry (85): needs confirmation
  CHECK(at(60, 2) == SafetyState::Normal);   // blip filtered
  CHECK(at(86, 3) == SafetyState::Normal);
  CHECK(at(86, 4) == SafetyState::Warm);     // 2 consecutive
  CHECK(at(93, 5) == SafetyState::Warm);
  CHECK(at(93, 6) == SafetyState::Hot);      // HOT entry is 92
  CHECK(at(97, 7) == SafetyState::Emergency);  // EMERGENCY (97) is immediate, no confirmation
  CHECK(t.status().transitions == 3 && t.status().reason.find("cpu 97.0 C") != std::string::npos);
  CHECK(at(80, 8) == SafetyState::Emergency);   // cool, but the 10 s hold has not elapsed
  CHECK(at(80, 18) == SafetyState::Hot);        // one level per hold
  CHECK(at(80, 20) == SafetyState::Hot);
  CHECK(at(80, 27) == SafetyState::Hot);        // 10 s hold counts from the HOT transition at t=18
  CHECK(at(80, 29) == SafetyState::Warm);
  CHECK(at(80, 45) == SafetyState::Warm);       // 80 C is inside the WARM hysteresis band (entry 85 - 5)
  CHECK(at(79, 60) == SafetyState::Normal);
  CHECK(at(90, 61) == SafetyState::Normal && at(90, 62) == SafetyState::Warm);  // warm to hot needs >= 92
}

void test_sensor_selection() {
  const auto t0 = Clock::now();
  {  // CPU sensor without any limit: conservative fallback 100 C
    ThermalSafety t;
    t.update({sensor("pkg", SensorKind::Cpu, 98)}, t0);
    CHECK(t.state() == SafetyState::Emergency && t.status().sensors[0].limit_source == "conservative fallback");
  }
  {  // Platform/GPU sensors without a limit, and informational sensors, are ignored
    ThermalSafety t;
    t.update({sensor("board", SensorKind::Platform, 120), sensor("wifi", SensorKind::Other, 120, 100.0)}, t0);
    CHECK(t.state() == SafetyState::Normal && !t.status().monitoring);
  }
  {  // implausible reading and implausible limit are not trusted
    ThermalSafety t;
    t.update({sensor("cpu", SensorKind::Cpu, 200, 100.0)}, t0);
    CHECK(!t.status().monitoring && t.state() == SafetyState::Normal);
    t.update({sensor("cpu", SensorKind::Cpu, 98, 255.0)}, t0 + seconds(1));  // limit 255 ignored -> fallback 100
    CHECK(t.state() == SafetyState::Emergency);
  }
  {  // worst sensor wins
    ThermalSafety t;
    t.update({sensor("a", SensorKind::Cpu, 50, 100), sensor("gpu", SensorKind::Gpu, 98, 100)}, t0);
    CHECK(t.state() == SafetyState::Emergency && t.status().sensors.size() == 2);
  }
  {  // sensors vanish after having worked: fail safe to WARM, never "cool"
    ThermalSafety t;
    t.update({sensor("cpu", SensorKind::Cpu, 50, 100)}, t0);
    t.update({}, t0 + seconds(1));
    CHECK(t.state() == SafetyState::Warm && !t.status().monitoring);
    t.update({}, t0 + seconds(100));
    CHECK(t.state() == SafetyState::Warm);
    t.update({sensor("cpu", SensorKind::Cpu, 50, 100)}, t0 + seconds(101));  // back and cool: recovers
    CHECK(t.update({sensor("cpu", SensorKind::Cpu, 50, 100)}, t0 + seconds(120)) == SafetyState::Normal);
  }
  {  // restored state with no sensors at all decays by itself
    ThermalSafety t;
    t.restore(SafetyState::Hot, t0);
    CHECK(t.state() == SafetyState::Hot && t.status().restored);
    t.update({}, t0 + seconds(5));
    CHECK(t.state() == SafetyState::Hot);
    t.update({}, t0 + seconds(11));
    CHECK(t.state() == SafetyState::Warm);
  }
  {  // restored HOT but sensors hot: stays; sensors cool: leaves after hold
    ThermalSafety t;
    t.restore(SafetyState::Hot, t0);
    t.update({sensor("cpu", SensorKind::Cpu, 95, 100)}, t0 + seconds(30));
    CHECK(t.state() == SafetyState::Hot);
    t.update({sensor("cpu", SensorKind::Cpu, 50, 100)}, t0 + seconds(31));
    CHECK(t.state() == SafetyState::Hot);  // hold counts from the last reading that still warranted HOT (t=30)
    t.update({sensor("cpu", SensorKind::Cpu, 50, 100)}, t0 + seconds(41));
    CHECK(t.state() == SafetyState::Warm);
  }
}

void test_effects() {
  CHECK(!effects_for(SafetyState::Normal).block_performance_increase && effects_for(SafetyState::Normal).allow_boost);
  CHECK(effects_for(SafetyState::Warm).block_performance_increase && !effects_for(SafetyState::Warm).profile_cap);
  CHECK(effects_for(SafetyState::Hot).profile_cap == Profile::Balanced && !effects_for(SafetyState::Hot).allow_boost);
  CHECK(effects_for(SafetyState::Emergency).force_emergency);
}

void test_guard() {
  const auto root = tmpdir();
  const fs::path th = root / "sys/class/thermal/thermal_zone0";
  auto set_temp = [&](int mc) { put(th / "temp", std::to_string(mc)); };
  put(th / "type", "x86_pkg_temp");
  put(th / "trip_point_0_temp", "100000");
  put(th / "trip_point_0_type", "critical");
  set_temp(50000);
  GuardOptions o;
  o.root = root;
  o.state_file = root / "state/safety.state";
  std::vector<std::string> events;
  {
    ThermalGuard g(o, [&](SafetyState from, SafetyState to, const std::string&) {
      events.push_back(std::string(to_string(from)) + ">" + std::string(to_string(to)));
    });
    const auto t0 = Clock::now();
    g.step(t0);
    CHECK(g.state() == SafetyState::Normal && events.empty());
    set_temp(98000);
    g.step(t0 + seconds(1));
    CHECK(g.state() == SafetyState::Emergency && events.size() == 1 && events[0] == "NORMAL>EMERGENCY");
    CHECK(g.status().monitoring && g.status().sensors.size() == 1);
    const auto saved = sysfs::read_text(o.state_file);
    CHECK(saved && saved->find("state=EMERGENCY") != std::string::npos);
  }
  {  // "restart": a new guard restores the saved state instead of forgetting it
    ThermalGuard g2(o, nullptr);
    g2.restore_saved();
    CHECK(g2.state() == SafetyState::Emergency && g2.status().restored);
    set_temp(40000);
    const auto t0 = Clock::now();
    g2.step(t0);
    CHECK(g2.state() == SafetyState::Emergency);       // hold applies after restoring
    g2.step(t0 + seconds(11));
    CHECK(g2.state() == SafetyState::Hot);
  }
  {  // stale saved state (older than restore_max_age) is ignored: sensors decide
    put(o.state_file, "state=HOT\nwall=1000\nreason=old");
    ThermalGuard g3(o, nullptr);
    g3.restore_saved();
    CHECK(g3.state() == SafetyState::Normal);
    put(o.state_file, "garbage");
    ThermalGuard g4(o, nullptr);
    g4.restore_saved();
    CHECK(g4.state() == SafetyState::Normal);
  }
  {  // threaded run: first reading happens immediately, stop is prompt
    GuardOptions o2;
    o2.root = root;
    o2.interval = std::chrono::milliseconds(20);
    set_temp(50000);
    ThermalGuard g5(o2, nullptr);
    g5.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(g5.status().monitoring);
    const auto t0 = std::chrono::steady_clock::now();
    g5.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < seconds(1));
  }
  fs::remove_all(root);
}

void test_hardware_limits() {
  const auto root = tmpdir();
  put(root / "sys/devices/system/cpu/cpufreq/policy0/cpuinfo_min_freq", "400000");
  put(root / "sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq", "4400000");
  put(root / "sys/devices/system/cpu/cpufreq/policy1/cpuinfo_min_freq", "400000");
  put(root / "sys/devices/system/cpu/cpufreq/policy1/cpuinfo_max_freq", "3300000");
  put(root / "sys/class/drm/card0/gt_RP0_freq_mhz", "1100");
  put(root / "sys/class/drm/card0/gt_RPn_freq_mhz", "300");
  put(root / "sys/class/powercap/intel-rapl:0/constraint_0_max_power_uw", "28000000");
  put(root / "sys/class/powercap/intel-rapl:0/constraint_1_max_power_uw", "55000000");
  auto h = HardwareLimits::discover(root);
  CHECK(h.cpu_khz && h.cpu_khz->min == 400000 && h.cpu_khz->max == 4400000);
  CHECK(h.gpu_mhz && h.gpu_mhz->max == 1100 && h.rapl_max_uw == 55000000);
  CHECK(h.validate(LimitKind::CpuFreqKhz, 2000000).has_value());
  CHECK(!h.validate(LimitKind::CpuFreqKhz, 4500000) && !h.validate(LimitKind::CpuFreqKhz, 100));   // no overclocking
  CHECK(!h.validate(LimitKind::GpuFreqMhz, 1200) && h.validate(LimitKind::GpuFreqMhz, 1100));
  CHECK(!h.validate(LimitKind::RaplPowerUw, 60000000) && h.validate(LimitKind::RaplPowerUw, 28000000));
  CHECK(!h.validate(LimitKind::RaplPowerUw, 0));
  auto empty = HardwareLimits::discover(root / "none");
  CHECK(!empty.validate(LimitKind::CpuFreqKhz, 1000000));  // unknown limit => refuse
  CHECK(empty.validate(LimitKind::CpuFreqKhz, 1).error().code == Errc::Unsupported);
  CHECK(format_limits(h).find("400000..4400000") != std::string::npos);
  fs::remove_all(root);
}

// ---------------- rollback ----------------
struct FakeWriter final : StateWriter {
  std::map<std::string, std::string> files;
  std::set<std::string> fail_write, fail_read;
  int writes = 0;
  Result<std::string> read(const std::string& t) override {
    if (fail_read.count(t) || !files.count(t)) return fail(make_error(Errc::NotFound, "no such target"));
    return files[t];
  }
  Result<void> write(const std::string& t, const std::string& v) override {
    if (fail_write.count(t)) return fail(make_error(Errc::Io, "EIO"));
    files[t] = v;
    ++writes;
    return {};
  }
};

void test_journal() {
  const auto d = tmpdir();
  const auto f = d / "state/rollback.journal";
  {
    auto j = RollbackJournal::open(f);
    CHECK(j.has_value());
    ChangeRecord r;
    r.subsystem = "cpu";
    r.target = "/sys/x y/z";            // spaces, percent, newline, empty and unicode survive the round trip
    r.old_value = "a b\n100%_";
    r.new_value = "";
    r.reason = "héllo wörld";
    r.policy = "BALANCED";
    auto id = (*j)->begin(r);
    CHECK(id && *id == 1 && (*j)->outstanding().size() == 1 && !(*j)->outstanding()[0].applied);
    CHECK((*j)->mark_applied(*id).has_value() && (*j)->outstanding()[0].applied);
    auto id2 = (*j)->begin(r);
    CHECK(id2 && *id2 == 2);
    CHECK((*j)->mark_aborted(*id2).has_value() && (*j)->outstanding().size() == 1);
  }
  {  // reopen: state is reconstructed exactly; ids keep increasing
    auto j = RollbackJournal::open(f);
    CHECK(j && (*j)->outstanding().size() == 1);
    const auto& r = (*j)->outstanding()[0];
    CHECK(r.id == 1 && r.applied && r.target == "/sys/x y/z" && r.old_value == "a b\n100%_" && r.new_value.empty() && r.reason == "héllo wörld");
    auto id = (*j)->begin(ChangeRecord{});
    CHECK(id && *id == 3);
    CHECK((*j)->mark_rolled_back(1).has_value() && (*j)->outstanding().size() == 1);
  }
  {  // torn last line (crash mid-write) and garbage are skipped, later records still work
    { std::ofstream(f, std::ios::app) << "B 9 123 cpu tar"; }  // no newline
    auto j = RollbackJournal::open(f);
    CHECK(j && (*j)->corrupt_lines() == 1 && (*j)->outstanding().size() == 1);
    auto id = (*j)->begin(ChangeRecord{});
    CHECK(id && *id >= 10);
    auto j2 = RollbackJournal::open(f);
    CHECK(j2 && (*j2)->outstanding().size() == 2);
  }
  {  // compaction keeps only what is outstanding
    const auto f2 = d / "big.journal";
    auto j = RollbackJournal::open(f2);
    ChangeRecord r;
    r.subsystem = "mem";
    r.target = "/proc/sys/vm/swappiness";
    r.old_value = "60";
    r.new_value = "10";
    r.reason = std::string(200, 'x');
    std::uint64_t last = 0;
    for (int i = 0; i < 1500; ++i) {
      last = *(*j)->begin(r);
      (void)(*j)->mark_applied(last);
      if (i != 1499) (void)(*j)->mark_rolled_back(last);
    }
    CHECK(fs::file_size(f2) > 256 * 1024);
    auto again = RollbackJournal::open(f2);
    CHECK(again && (*again)->outstanding().size() == 1 && (*again)->outstanding()[0].id == last);
    CHECK(fs::file_size(f2) < 2000);
    auto id = (*again)->begin(r);
    CHECK(id && *id == last + 1);
  }
  CHECK(!RollbackJournal::open("/proc/nonexistent/dir/j"));
  fs::remove_all(d);
}

void test_manager() {
  const auto d = tmpdir();
  const auto jf = d / "j";
  FakeWriter w;
  const std::string gov = "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor";
  const std::string epp = "/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference";
  w.files[gov] = "powersave";
  w.files[epp] = "balance_performance";
  {
    RollbackManager m(std::move(*RollbackJournal::open(jf)), w);
    auto a = m.apply({"cpu", gov, "performance", "game", "PERFORMANCE"});
    CHECK(a && *a == 1 && w.files[gov] == "performance" && m.outstanding_count() == 1);
    CHECK(m.outstanding()[0].old_value == "powersave" && m.outstanding()[0].applied);  // old state captured by the manager, not the caller
    CHECK(m.apply({"cpu", gov, "performance", "again", "P"}).value() == 0 && m.outstanding_count() == 1);  // no-op
    CHECK(m.apply({"cpu", epp, "performance", "game", "P"}).has_value());
    CHECK(m.apply({"cpu", gov, "schedutil", "chain", "P"}).has_value() && m.outstanding_count() == 2);  // chain collapsed: one record per target
    CHECK(!m.apply({"cpu", "/sys/missing", "1", "x", "P"}));              // unreadable => refused
    CHECK(m.outstanding_count() == 2);
    w.fail_write.insert(epp);
    CHECK(!m.apply({"cpu", epp, "power", "x", "P"}) && w.files[epp] == "performance");  // failed write leaves no outstanding record
    CHECK(m.outstanding_count() == 2);
    w.fail_write.clear();
  }
  {  // "crash": a new manager over the same journal restores everything, newest first
    RollbackManager m(std::move(*RollbackJournal::open(jf)), w);
    CHECK(m.outstanding_count() == 2);
    auto rep = m.rollback_all("recovery");
    CHECK(rep.ok() && rep.restored == 2 && m.outstanding_count() == 0);
    CHECK(w.files[gov] == "powersave" && w.files[epp] == "balance_performance");
  }
  {  // someone else changed the target after us: not overwritten
    RollbackManager m(std::move(*RollbackJournal::open(jf)), w);
    (void)m.apply({"cpu", gov, "performance", "r", "P"});
    w.files[gov] = "conservative";  // e.g. power-profiles-daemon
    auto rep = m.rollback_all("x");
    CHECK(rep.restored == 0 && rep.skipped == 1 && w.files[gov] == "conservative" && m.outstanding_count() == 0);
  }
  {  // crash between journaling and writing: target still has the old value
    RollbackManager m(std::move(*RollbackJournal::open(jf)), w);
    w.files[gov] = "powersave";
    auto j = RollbackJournal::open(d / "j2");
    ChangeRecord r;
    r.subsystem = "cpu"; r.target = gov; r.old_value = "powersave"; r.new_value = "performance";
    (void)(*j)->begin(r);  // journaled, never written
    RollbackManager m2(std::move(*j), w);
    auto rep = m2.rollback_all("recovery");
    CHECK(rep.skipped == 1 && rep.failed == 0 && w.files[gov] == "powersave");
  }
  {  // restore failure keeps the record for the next attempt
    RollbackManager m(std::move(*RollbackJournal::open(d / "j3")), w);
    w.files[gov] = "powersave";
    (void)m.apply({"cpu", gov, "performance", "r", "P"});
    w.fail_write.insert(gov);
    auto rep = m.rollback_all("x");
    CHECK(!rep.ok() && rep.failed == 1 && m.outstanding_count() == 1);
    w.fail_write.clear();
    CHECK(m.rollback_all("retry").restored == 1 && w.files[gov] == "powersave");
  }
  {  // per-subsystem rollback; unwritable journal refuses changes
    RollbackManager m(std::move(*RollbackJournal::open(d / "j4")), w);
    w.files["/proc/sys/vm/swappiness"] = "60";
    (void)m.apply({"cpu", gov, "performance", "r", "P"});
    (void)m.apply({"memory", "/proc/sys/vm/swappiness", "10", "r", "P"});
    auto rep = m.rollback_subsystem("memory", "x");
    CHECK(rep.restored == 1 && w.files["/proc/sys/vm/swappiness"] == "60" && w.files[gov] == "performance" && m.outstanding_count() == 1);
  }
  {  // NullWriter (current build) cannot change anything
    NullWriter nw;
    RollbackManager m(std::move(*RollbackJournal::open(d / "j5")), nw);
    CHECK(!m.apply({"cpu", gov, "x", "r", "P"}) && m.rollback_all("x").ok());
  }
  fs::remove_all(d);
}

policy::PolicyDecision decision(policy::ControlMode cpu, policy::ControlMode io = policy::ControlMode::Active) {
  policy::PolicyDecision d;
  d.control[static_cast<std::size_t>(policy::Subsystem::Cpu)] = {cpu, cpu == policy::ControlMode::Active ? "" : "note"};
  d.control[static_cast<std::size_t>(policy::Subsystem::Io)] = {io, io == policy::ControlMode::Active ? "" : "note"};
  return d;
}

void test_gate() {
  CHECK(is_allowed_target("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"));
  CHECK(is_allowed_target("/sys/devices/system/cpu/cpufreq/policy12/energy_performance_preference"));
  CHECK(is_allowed_target("/sys/devices/system/cpu/intel_pstate/no_turbo"));
  CHECK(is_allowed_target("/proc/sys/vm/swappiness"));
  CHECK(is_allowed_target("/sys/block/nvme0n1/queue/scheduler"));
  CHECK(is_allowed_target("/sys/fs/cgroup/user.slice/app.scope/cpu.weight"));
  CHECK(is_allowed_target("/sys/class/drm/card0/gt_max_freq_mhz"));
  CHECK(!is_allowed_target("/sys/devices/system/cpu/cpufreq/policy0/../../../../../etc/passwd"));
  CHECK(!is_allowed_target("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor/../scaling_max_freq"));
  CHECK(!is_allowed_target("/sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed"));
  CHECK(!is_allowed_target("/etc/passwd") && !is_allowed_target("") && !is_allowed_target("relative/path"));
  CHECK(!is_allowed_target("/sys//devices/system/cpu/intel_pstate/no_turbo"));
  CHECK(!is_allowed_target("/sys/devices/system/cpu/intel_pstate/no_turbo "));
  CHECK(!is_allowed_target("/sys/fs/cgroup/user.slice/cgroup.procs"));  // moving processes is not allowlisted
  CHECK(!is_allowed_target("/proc/sys/kernel/core_pattern") && !is_allowed_target("/sys/power/state"));
  CHECK(!is_allowed_target("/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw"));  // power limits: never allowlisted
  CHECK(!is_allowed_target("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor\n"));

  auto active = decision(policy::ControlMode::Active);
  HardwareLimits limits;
  limits.cpu_khz = Range{400000, 4400000};
  ChangeIntent gov;
  gov.subsystem = policy::Subsystem::Cpu;
  gov.target = "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq";
  gov.value = "3000000";
  gov.raises_performance = true;
  gov.limit_kind = LimitKind::CpuFreqKhz;
  gov.numeric = 3000000;
  GateContext ctx{&active, SafetyState::Normal, &limits};
  CHECK(check_change(gov, ctx).allowed);

  auto bad_target = gov;
  bad_target.target = "/etc/shadow";
  CHECK(check_change(bad_target, ctx).stage == GateStage::Target);
  CHECK(check_change(gov, GateContext{nullptr, SafetyState::Normal, &limits}).stage == GateStage::Policy);
  auto unavail = decision(policy::ControlMode::Unavailable);
  CHECK(check_change(gov, GateContext{&unavail, SafetyState::Normal, &limits}).stage == GateStage::Capability);
  auto observe = decision(policy::ControlMode::Observe);
  const auto v = check_change(gov, GateContext{&observe, SafetyState::Normal, &limits});
  CHECK(!v.allowed && v.stage == GateStage::Conflict && v.reason.find("observe-only") != std::string::npos);

  for (auto s : {SafetyState::Warm, SafetyState::Hot, SafetyState::Emergency})
    CHECK(check_change(gov, GateContext{&active, s, &limits}).stage == GateStage::Thermal);   // no performance increase when warm+
  auto lower = gov;
  lower.raises_performance = false;
  lower.numeric = 800000;
  CHECK(check_change(lower, GateContext{&active, SafetyState::Emergency, &limits}).allowed);  // lowering is always allowed

  auto over = gov;
  over.numeric = 4600000;  // above the hardware maximum: no overclocking
  CHECK(check_change(over, ctx).stage == GateStage::Limits);
  CHECK(check_change(gov, GateContext{&active, SafetyState::Normal, nullptr}).stage == GateStage::Limits);
  auto no_num = gov;
  no_num.numeric.reset();
  CHECK(check_change(no_num, ctx).stage == GateStage::Limits);
  HardwareLimits unknown;
  CHECK(check_change(gov, GateContext{&active, SafetyState::Normal, &unknown}).stage == GateStage::Limits);
}

void test_engine_safety() {
  using namespace policy;
  const auto t0 = Clock::now();
  monitor::SystemSnapshot sys;
  sys.power = monitor::PowerSample{};
  sys.power->on_ac = true;
  workload::WorkloadResult compile;
  compile.cls = workload::WorkloadClass::Compile;
  compile.confidence = 0.8;
  workload::WorkloadResult desk;
  desk.cls = workload::WorkloadClass::Desktop;
  desk.confidence = 0.8;
  auto in = [&](const workload::WorkloadResult* w, SafetyState s, Clock::time_point now, UserMode u = {}) {
    PolicyInput i;
    i.user = u;
    i.workload = w;
    i.sys = &sys;
    i.safety = s;
    i.now = now;
    return i;
  };
  {  // WARM: an increase is blocked, but staying/lowering is allowed
    PolicyEngine e;
    auto d = e.evaluate(in(&desk, SafetyState::Warm, t0));
    CHECK(d.profile == Profile::Balanced);
    d = e.evaluate(in(&compile, SafetyState::Warm, t0 + seconds(30)));
    CHECK(d.profile == Profile::Balanced && d.safety_limited);
    bool tagged = false;
    for (const auto& r : d.reasons) tagged |= r.find("[SAFETY]") != std::string::npos;
    CHECK(tagged);
    d = e.evaluate(in(&compile, SafetyState::Normal, t0 + seconds(60)));
    CHECK(d.profile == Profile::Performance && !d.safety_limited);
    d = e.evaluate(in(&compile, SafetyState::Warm, t0 + seconds(61)));
    CHECK(d.profile == Profile::Performance);  // not forced down at WARM
  }
  {  // first evaluation while WARM never starts above BALANCED
    PolicyEngine e;
    CHECK(e.evaluate(in(&compile, SafetyState::Warm, t0)).profile == Profile::Balanced);
  }
  {  // HOT caps at BALANCED immediately, even for a locked PERFORMANCE profile, and turns boost off
    PolicyEngine e;
    UserMode locked{true, Profile::Performance, true};
    auto d = e.evaluate(in(&compile, SafetyState::Normal, t0, locked));
    CHECK(d.profile == Profile::Performance && d.targets.allow_boost);
    d = e.evaluate(in(&compile, SafetyState::Hot, t0 + seconds(1), locked));
    CHECK(d.profile == Profile::Balanced && d.safety_limited && !d.targets.allow_boost && d.safety == SafetyState::Hot);
    CHECK(format_decision(d).find("Safety:") != std::string::npos);
    d = e.evaluate(in(&compile, SafetyState::Emergency, t0 + seconds(2), locked));
    CHECK(d.profile == Profile::ThermalEmergency);
    d = e.evaluate(in(&compile, SafetyState::Normal, t0 + seconds(3), locked));
    CHECK(d.profile == Profile::Performance);
  }
}

}  // namespace

void run_safety_tests() {
  // SAFETY-level log lines are intentionally emitted by several tests; keep the output quiet.
  log::set_level(log::Level::Safety);
  test_atomic_file();
  test_sensors();
  test_state_machine();
  test_sensor_selection();
  test_effects();
  test_guard();
  test_hardware_limits();
  test_journal();
  test_manager();
  test_gate();
  test_engine_safety();
}
