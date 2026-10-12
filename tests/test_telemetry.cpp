#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "check.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "core/sha256.hpp"
#include "database/telemetry.hpp"
#include "policy/profiles.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/ids.hpp"
#include "telemetry/reward.hpp"

namespace fs = std::filesystem;
using namespace opt;
using namespace opt::db;
using namespace opt::telemetry;

namespace {

fs::path tmpdir() {
  char t[] = "/tmp/opt-tel-XXXXXX";
  return ::mkdtemp(t);
}
bool near(double a, double b, double e = 0.001) { return a > b - e && a < b + e; }

constexpr std::int64_t kBase = 1'700'002'800;  // exactly on an hour boundary (1700002800 % 3600 == 0)
static_assert(kBase % 3600 == 0);

TelemetryRow row(std::int64_t ts, const std::string& workload = "DESKTOP", const std::string& policy = "BALANCED") {
  TelemetryRow r;
  r.ts = ts;
  r.session_id = "s";
  r.device_id_hash = "d";
  r.kernel_version = "6.12";
  r.interval_s = 10;
  r.workload = workload;
  r.selected_policy = policy;
  r.policy_parameters = "cpu=balanced";
  return r;
}

void test_sha256_and_ids() {
  CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(sha256_hex(std::string(1000, 'a')).size() == 64);

  const auto s1 = make_session_id(), s2 = make_session_id();
  CHECK(s1.size() == 16 && s1 != s2);
  const auto d = tmpdir();
  const auto f = d / "state/telemetry.salt";
  auto salt = load_or_create_salt(f);
  CHECK(salt && salt->size() == 32);
  CHECK(load_or_create_salt(f).value() == *salt);                    // stable across restarts
  CHECK((fs::status(f).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);  // 0600
  const auto h = device_id_hash(*salt, "0123456789abcdef0123456789abcdef");
  CHECK(h.size() == 16 && h == device_id_hash(*salt, "0123456789abcdef0123456789abcdef"));
  CHECK(h.find("0123456789abcdef") == std::string::npos);            // not the machine id
  auto rotated = rotate_salt(f);
  CHECK(rotated && *rotated != *salt && device_id_hash(*rotated, "0123456789abcdef0123456789abcdef") != h);  // unlinkable after delete
  { std::ofstream(f, std::ios::trunc) << "not-a-salt\n"; }
  CHECK(load_or_create_salt(f).value().size() == 32);                 // damaged salt is replaced
  { std::ofstream(d / "etc_dummy"); }
  fs::create_directories(d / "root/etc");
  { std::ofstream(d / "root/etc/machine-id") << "abc123\n"; }
  CHECK(read_machine_id(d / "root") == "abc123" && read_machine_id(d / "none").empty());
  fs::remove_all(d);
}

void test_reward() {
  RewardInput idle;
  idle.cpu_util_pct = 2; idle.cpu_freq_mhz = 800; idle.cpu_max_mhz = 4400;
  idle.psi_cpu10 = 0; idle.psi_io10 = 0; idle.psi_mem10 = 0; idle.power_w = 3; idle.thermal_margin_c = 60;
  auto a = compute_reward(idle);
  CHECK(a.performance && near(*a.performance, 1.0 - 2.0 / 60 + (2.0 / 60) * (800.0 / 4400), 0.01));  // idle: nothing to deliver
  CHECK(near(*a.responsiveness, 1.0) && near(*a.thermal_cost, 0.0) && a.total > 0.5 && a.total <= 0.65);

  RewardInput stalled = idle;
  stalled.psi_cpu10 = 60;
  CHECK(compute_reward(stalled).total < a.total);                    // more stall pressure -> lower reward
  RewardInput hot = idle;
  hot.thermal_margin_c = 5;
  CHECK(compute_reward(hot).total < a.total && *compute_reward(hot).thermal_cost > 0.8);
  RewardInput power = idle;
  power.power_w = 40;
  CHECK(near(*compute_reward(power).power_cost, 1.0) && compute_reward(power).total < a.total);
  RewardInput busy_slow = idle;
  busy_slow.cpu_util_pct = 95;
  busy_slow.cpu_freq_mhz = 800;
  RewardInput busy_fast = busy_slow;
  busy_fast.cpu_freq_mhz = 4300;
  CHECK(compute_reward(busy_fast).total > compute_reward(busy_slow).total);  // busy machine: frequency headroom counts
  RewardInput unstable = idle;
  unstable.profile_changes = 2;
  unstable.safety_event = true;
  CHECK(near(compute_reward(unstable).instability, 1.0) && compute_reward(unstable).total < a.total);

  RewardInput worst;
  worst.cpu_util_pct = 100; worst.cpu_freq_mhz = 0; worst.cpu_max_mhz = 4400;
  worst.psi_cpu10 = 100; worst.power_w = 100; worst.thermal_margin_c = -10; worst.profile_changes = 9; worst.safety_event = true;
  const auto w = compute_reward(worst);
  CHECK(near(w.total, -0.35, 0.001));                                  // bounded below
  const RewardInput nothing;
  CHECK(near(compute_reward(nothing).total, 0.0));                     // unknown inputs are not guessed
  RewardInput only_event;
  only_event.safety_event = true;
  CHECK(near(compute_reward(only_event).total, -0.05));
  RewardInput partial;
  partial.psi_cpu10 = 0;
  CHECK(near(compute_reward(partial).total, 1.0 * 0.30 / 0.35, 0.001));  // weights renormalised over known parts
}

void test_database_basics() {
  auto d = Database::open(":memory:");
  CHECK(d.has_value());
  CHECK((*d)->exec("CREATE TABLE t(a INTEGER, b REAL, c TEXT)").has_value());
  {
    auto ins = (*d)->prepare("INSERT INTO t VALUES (?1, ?2, ?3)");
    CHECK(ins.has_value());
    ins->bind(1, std::int64_t{7}).bind(2, std::nan("")).bind(3, std::string_view("x'; DROP TABLE t;--"));  // injection-proof binding
    CHECK(ins->step().has_value());
  }
  CHECK((*d)->scalar_int("SELECT COUNT(*) FROM t").value() == 1);
  auto sel = (*d)->prepare("SELECT a, b, c FROM t");
  CHECK(sel->step().value() && sel->col_int(0) == 7 && sel->col_null(1) && sel->col_text(2) == "x'; DROP TABLE t;--");
  CHECK(!(*d)->exec("THIS IS NOT SQL"));
  CHECK(!(*d)->prepare("SELECT FROM"));
  CHECK(!Database::open("/proc/nonexistent/x.db"));
}

void test_store_insert_and_history() {
  auto s = TelemetryStore::open(":memory:", {});
  CHECK(s.has_value());
  TelemetryRow r = row(kBase + 10, "COMPILE", "PERFORMANCE");
  r.cpu_utilization = 85.5;
  r.temperature = 71.0;
  r.reward = 0.4;
  CHECK((*s)->insert(r).has_value());
  TelemetryRow nulls = row(kBase + 20);  // everything optional is NULL
  CHECK((*s)->insert(nulls).has_value());
  auto st = (*s)->stats();
  CHECK(st && st->detailed_rows == 2 && st->summary_rows == 0 && st->oldest_ts == kBase + 10 && st->newest_ts == kBase + 20);
  auto h = (*s)->recent_detail(kBase);
  CHECK(h && h->size() == 2);
  const SummaryRow* c = nullptr;
  for (const auto& x : *h) if (x.workload == "COMPILE") c = &x;
  CHECK(c && c->policy == "PERFORMANCE" && c->avg_cpu && near(*c->avg_cpu, 85.5) && c->avg_power == std::nullopt && c->successes == 1);
  const std::string txt = format_history(*h, "title");
  CHECK(txt.find("COMPILE") != std::string::npos && txt.find("title") != std::string::npos);
  CHECK(format_history({}, "x").find("(no data yet)") != std::string::npos);
}

void test_rollup_and_retention() {
  auto s = TelemetryStore::open(":memory:", {1, 365, 64 << 20});
  // hour 0: 3 DESKTOP rows + 1 COMPILE; hour 1: 2 rows; current hour: 1 row
  auto mk = [&](std::int64_t ts, const char* wl, double cpu, double temp, double energy, bool ok) {
    TelemetryRow r = row(ts, wl, "BALANCED");
    r.cpu_utilization = cpu; r.temperature = temp; r.energy_delta = energy; r.reward = cpu / 100; r.success = ok;
    CHECK((*s)->insert(r).has_value());
  };
  mk(kBase + 10, "DESKTOP", 10, 50, 5, true);
  mk(kBase + 20, "DESKTOP", 20, 60, 5, true);
  mk(kBase + 30, "DESKTOP", 30, 55, 5, false);
  mk(kBase + 40, "COMPILE", 90, 80, 50, true);
  mk(kBase + 3600 + 5, "DESKTOP", 40, 52, 4, true);
  mk(kBase + 3600 + 15, "DESKTOP", 60, 58, 4, true);
  const std::int64_t now = kBase + 2 * 3600 + 100;
  mk(now - 50, "DESKTOP", 99, 99, 1, true);  // current hour: must NOT be rolled up yet

  auto rep = (*s)->maintain(now);
  CHECK(rep && rep->buckets_rolled == 3 && rep->pruned_detail == 0);
  auto sums = (*s)->recent_summaries(10);
  CHECK(sums && sums->size() == 3);
  const SummaryRow* d0 = nullptr; const SummaryRow* c0 = nullptr; const SummaryRow* d1 = nullptr;
  for (const auto& x : *sums) {
    if (x.hour_ts == kBase && x.workload == "DESKTOP") d0 = &x;
    if (x.hour_ts == kBase && x.workload == "COMPILE") c0 = &x;
    if (x.hour_ts == kBase + 3600) d1 = &x;
  }
  CHECK(d0 && d0->samples == 3 && near(*d0->avg_cpu, 20.0) && near(*d0->avg_temp, 55.0) && near(*d0->peak_temp, 60.0) &&
        near(*d0->energy, 15.0) && d0->successes == 2 && d0->failures == 1 && near(d0->duration_s, 30.0));
  CHECK(c0 && c0->samples == 1 && near(*c0->peak_temp, 80.0));
  CHECK(d1 && d1->samples == 2 && near(*d1->avg_cpu, 50.0));
  CHECK((*s)->stats()->detailed_rows == 7);                                  // nothing deleted yet (younger than detail_days)
  auto again = (*s)->maintain(now);
  CHECK(again && again->buckets_rolled == 0);                                // idempotent: watermark prevents double counting
  CHECK((*s)->recent_summaries(10)->size() == 3);

  // Two days later detailed rows are pruned (they were rolled up); the summaries stay.
  const std::int64_t later = now + 2 * 86400;
  auto pr = (*s)->maintain(later);
  CHECK(pr && pr->pruned_detail == 7 && pr->buckets_rolled == 1);            // the former "current hour" bucket is rolled first
  CHECK((*s)->stats()->detailed_rows == 0 && (*s)->stats()->summary_rows == 4);
  // Summaries expire after summary_days.
  (*s)->set_retention({1, 30, 64 << 20});
  auto ex = (*s)->maintain(later + 40LL * 86400);
  CHECK(ex && ex->pruned_summaries == 4 && (*s)->stats()->summary_rows == 0);
}

void test_unrolled_rows_are_never_dropped() {
  auto s = TelemetryStore::open(":memory:", {1, 365, 64 << 20});
  CHECK((*s)->insert(row(kBase + 10, "GAME", "PERFORMANCE")).has_value());
  // maintenance did not run for 30 days: the row is rolled up BEFORE it is pruned
  auto rep = (*s)->maintain(kBase + 30LL * 86400);
  CHECK(rep && rep->buckets_rolled == 1 && rep->pruned_detail == 1);
  auto sums = (*s)->recent_summaries(5);
  CHECK(sums && sums->size() == 1 && sums->at(0).workload == "GAME" && sums->at(0).samples == 1);
}

void test_size_cap_and_delete() {
  const auto d = tmpdir();
  const auto f = d / "t.db";
  RetentionPolicy ret{7, 365, 300 * 1024};  // tiny cap
  auto s = TelemetryStore::open(f, ret);
  CHECK(s.has_value());
  for (int i = 0; i < 4000; ++i) {
    TelemetryRow r = row(kBase + i, "DESKTOP", "BALANCED");
    r.policy_parameters = std::string(120, 'p');
    r.cpu_utilization = i % 100;
    CHECK((*s)->insert(r).has_value());
  }
  const auto before = (*s)->stats();
  CHECK(before && before->db_bytes > ret.max_db_bytes);
  auto rep = (*s)->maintain(kBase + 4000);
  CHECK(rep && rep->trimmed_for_size > 0);
  const auto after = (*s)->stats();
  CHECK(after && after->detailed_rows < 4000 && after->db_bytes < before->db_bytes && after->db_bytes <= ret.max_db_bytes * 12 / 10);
  CHECK(fs::exists(f));
  // file permissions: never world-readable
  CHECK((fs::status(f).permissions() & fs::perms::others_all) == fs::perms::none);

  CHECK((*s)->delete_all().has_value());
  const auto empty = (*s)->stats();
  CHECK(empty && empty->detailed_rows == 0 && empty->summary_rows == 0 && empty->db_bytes < 64 * 1024);
  CHECK((*s)->insert(row(kBase + 9000)).has_value() && (*s)->stats()->detailed_rows == 1);   // usable afterwards
  CHECK((*s)->maintain(kBase + 9000 + 7200)->buckets_rolled >= 1);                            // rollup state was reset too
  s->reset();
  fs::remove_all(d);
}

void test_corruption_recovery() {
  const auto d = tmpdir();
  const auto f = d / "t.db";
  { std::ofstream(f) << std::string(5000, 'x') << "this is not a sqlite file"; }
  log::set_level(log::Level::Safety);
  auto s = TelemetryStore::open(f, {});
  CHECK(s.has_value() && (*s)->insert(row(kBase)).has_value());      // fresh database, daemon keeps working
  int quarantined = 0;
  for (const auto& e : fs::directory_iterator(d))
    if (e.path().filename().string().find(".corrupt-") != std::string::npos) ++quarantined;
  CHECK(quarantined >= 1);
  s->reset();
  fs::remove_all(d);
}

void test_concurrency() {
  auto s = TelemetryStore::open(":memory:", {});
  std::vector<std::thread> th;
  std::atomic<int> failures{0};
  for (int t = 0; t < 4; ++t)
    th.emplace_back([&, t] {
      for (int i = 0; i < 200; ++i) {
        if (!(*s)->insert(row(kBase + t * 1000 + i)).has_value()) ++failures;
        if (i % 50 == 0) (void)(*s)->stats();
      }
    });
  th.emplace_back([&] {
    for (int i = 0; i < 20; ++i) (void)(*s)->recent_detail(kBase);
  });
  for (auto& x : th) x.join();
  CHECK(failures == 0 && (*s)->stats()->detailed_rows == 800);
}

void test_collector_and_privacy() {
  monitor::SystemSnapshot sys;
  sys.cpu = monitor::CpuSample{};
  sys.cpu->utilization_pct = 80;
  sys.cpu->freq_avg_mhz = 3000;
  sys.memory = monitor::MemorySample{};
  sys.memory->total_kb = 8000000;
  sys.memory->available_kb = 2000000;
  sys.memory->swap_in_pages_s = 5;
  sys.memory->swap_out_pages_s = 7;
  sys.psi = monitor::PsiSample{};
  sys.psi->cpu = monitor::PsiResource{};
  sys.psi->cpu->some = monitor::PsiLine{12.5, 0, 0, 0};
  sys.thermal = monitor::ThermalSample{};
  monitor::ThermalZone z;
  z.type = "acpitz";
  z.temp_c = 70;
  z.trips.push_back({100, "hot"});
  sys.thermal->zones.push_back(z);
  sys.power = monitor::PowerSample{};
  sys.power->on_ac = true;
  sys.power->rapl_pkg_w = 15;
  sys.io.emplace();
  sys.io->total_read_bps = 1e6;
  sys.io->total_write_bps = 2e6;
  sys.gpu = monitor::GpuSample{};
  monitor::GpuDevice g;
  g.busy_pct = 30;
  g.cur_mhz = 700;
  sys.gpu->devices.push_back(g);

  workload::WorkloadResult wl;
  wl.cls = workload::WorkloadClass::Compile;
  wl.confidence = 0.8;
  wl.reasons = {"compiler/linker active: secret-project-cc1plus (4 workers)"};
  wl.top.push_back({123, "my-private-app", 90.0});
  policy::PolicyDecision dec;
  dec.profile = Profile::Performance;
  dec.targets = policy::base_targets(Profile::Performance);
  dec.aggressiveness = 3;

  RowBuilder b("sess", "dev", "6.12.1", 4400.0);
  SampleInputs in;
  in.unix_time = kBase;
  in.interval_s = 10;
  in.sys = &sys;
  in.workload = &wl;
  in.decision = &dec;
  in.safety = SafetyState::Hot;
  in.blocked = 2;
  in.reverted = 1;
  const auto r = b.build(in);
  CHECK(r.workload == "COMPILE" && r.selected_policy == "PERFORMANCE" && r.session_id == "sess" && r.device_id_hash == "dev");
  CHECK(r.cpu_utilization == 80 && r.cpu_frequency == 3000 && near(*r.ram_used, 75.0) && near(*r.swap_activity, 12.0));
  CHECK(r.psi_cpu == 12.5 && r.psi_io == std::nullopt && near(*r.temperature, 70.0));
  CHECK(near(*r.power, 15.0) && near(*r.energy_delta, 150.0) && near(*r.io_throughput, 3e6));
  CHECK(near(*r.gpu_utilization, 30.0) && near(*r.gpu_frequency, 700.0));
  CHECK(r.fps == std::nullopt && r.frame_time == std::nullopt && r.latency == std::nullopt);   // no source: NULL, never invented
  CHECK(r.safety_action == "thermal:HOT,gate-blocked:2" && r.rollback == 1 && !r.success && r.reward);
  CHECK(r.policy_parameters.find("cpu=performance") != std::string::npos && r.policy_parameters.find("aggressiveness=3") != std::string::npos);
  // Privacy: no process names or reasons leak into any stored text field.
  const std::string all = r.workload + r.selected_policy + r.policy_parameters + r.safety_action + r.session_id + r.device_id_hash + r.kernel_version;
  CHECK(all.find("secret-project") == std::string::npos && all.find("my-private-app") == std::string::npos && all.find("cc1plus") == std::string::npos);

  // On battery without RAPL the battery's own power draw is used; missing inputs stay NULL.
  monitor::SystemSnapshot bat;
  bat.power = monitor::PowerSample{};
  bat.power->on_ac = false;
  monitor::PowerSupply ps;
  ps.type = "Battery";
  ps.power_w = 9.5;
  bat.power->supplies.push_back(ps);
  SampleInputs in2;
  in2.unix_time = kBase;
  in2.interval_s = 4;
  in2.sys = &bat;
  const auto r2 = b.build(in2);
  CHECK(near(*r2.power, 9.5) && near(*r2.energy_delta, 38.0) && r2.workload == "UNKNOWN" && r2.cpu_utilization == std::nullopt && r2.success);
  CHECK(b.build(SampleInputs{}).power == std::nullopt);
}

void test_config_telemetry() {
  auto d = parse_config("");
  CHECK(d && d->telemetry_enabled && d->telemetry_interval_ms == 10000 && d->telemetry_detail_days == 7 &&
        d->telemetry_summary_days == 365 && d->telemetry_max_db_mb == 64);
  auto c = parse_config("[telemetry]\nenabled = off\ninterval_ms = 20000\ndetail_retention_days = 14\nsummary_retention_days = 90\nmax_db_mb = 16\n");
  CHECK(c && !c->telemetry_enabled && c->telemetry_interval_ms == 20000 && c->telemetry_detail_days == 14 &&
        c->telemetry_summary_days == 90 && c->telemetry_max_db_mb == 16);
  CHECK(!parse_config("[telemetry]\ninterval_ms = 100\n"));
  CHECK(!parse_config("[telemetry]\ndetail_retention_days = 0\n"));
  CHECK(!parse_config("[telemetry]\nsummary_retention_days = 5\n"));
  CHECK(!parse_config("[telemetry]\nmax_db_mb = 99999\n"));
  CHECK(!parse_config("[telemetry]\nenabled = maybe\n"));
  CHECK(!parse_config("[telemetry]\nupload_url = http://x\n"));  // there is no upload option at all
}

}  // namespace

void run_telemetry_tests() {
  log::set_level(log::Level::Safety);
  test_sha256_and_ids();
  test_reward();
  test_database_basics();
  test_store_insert_and_history();
  test_rollup_and_retention();
  test_unrolled_rows_are_never_dropped();
  test_size_cap_and_delete();
  test_corruption_recovery();
  test_concurrency();
  test_collector_and_privacy();
  test_config_telemetry();
}
