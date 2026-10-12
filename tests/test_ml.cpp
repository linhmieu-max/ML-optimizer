#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "check.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "core/sha256.hpp"
#include "ml/base_model.hpp"
#include "ml/feature_vector.hpp"
#include "ml/fit.hpp"
#include "ml/model.hpp"
#include "ml/predictor.hpp"
#include "policy/policy_engine.hpp"
#include "policy/rules.hpp"

namespace fs = std::filesystem;
using namespace opt;
using namespace opt::ml;
using workload::WorkloadClass;
using std::chrono::seconds;

namespace {

fs::path tmpdir() {
  char t[] = "/tmp/opt-ml-XXXXXX";
  return ::mkdtemp(t);
}
bool near(double a, double b, double e = 0.01) { return a > b - e && a < b + e; }

BaseBuildOptions small_options() {
  BaseBuildOptions o;
  o.per_class = 150;
  o.profile_samples = 3500;
  o.epochs_workload = 25;
  o.epochs_profile = 30;
  return o;
}

const BaseModels& shared_models() {
  static const BaseModels m = [] {
    auto r = build_base_models(small_options());
    if (!r) { std::fprintf(stderr, "base model build failed in tests\n"); std::exit(2); }
    return std::move(*r);
  }();
  return m;
}

// ---------- synthetic machine (same style as the workload tests) ----------
struct World {
  workload::ProcessSnapshot ps;
  monitor::SystemSnapshot sys;
  int next = 100;
  explicit World(double cpu = 10) {
    sys.cpu = monitor::CpuSample{};
    sys.cpu->utilization_pct = cpu;
    sys.cpu->freq_avg_mhz = 2000;
    sys.cpu->online_cpus = 8;
    sys.cpu->load1 = cpu / 100.0 * 8;
    sys.memory = monitor::MemorySample{};
    sys.memory->total_kb = 8000000;
    sys.memory->available_kb = 5000000;
  }
  void add(const std::string& comm, double cpu, const std::string& cg = "0::/user.slice/app.scope", unsigned flags = 0, int ppid = 1) {
    workload::ProcessInfo p;
    p.pid = next++;
    p.ppid = ppid;
    p.comm = comm;
    p.exe = comm;
    p.cgroup = cg;
    p.cpu_pct = cpu;
    p.cmd_flags = flags;
    ps.procs.push_back(p);
  }
  void gpu(double busy) {
    sys.gpu = monitor::GpuSample{};
    monitor::GpuDevice g;
    g.busy_pct = busy;
    sys.gpu->devices.push_back(g);
  }
  void io(double r, double w) {
    sys.io.emplace();
    sys.io->total_read_bps = r * 1e6;
    sys.io->total_write_bps = w * 1e6;
  }
  FeatureVector features() {
    ps.finalize();
    return extract_features(ps, &sys, FeatureEnv{4400.0});
  }
};

void test_rng() {
  Rng a(42), b(42), c(43);
  bool same = true, diff = false;
  for (int i = 0; i < 100; ++i) {
    const auto x = a.next(), y = b.next(), z = c.next();
    same &= x == y;
    diff |= x != z;
  }
  CHECK(same && diff);
  Rng r(7);
  double sum = 0, sq = 0;
  bool in_range = true;
  for (int i = 0; i < 20000; ++i) {
    const double u = r.uniform();
    in_range &= u >= 0.0 && u < 1.0;
    const double n = r.normal();
    sum += n;
    sq += n * n;
  }
  CHECK(in_range && near(sum / 20000, 0.0, 0.03) && near(sq / 20000, 1.0, 0.05));
  int hits[5] = {0};
  for (int i = 0; i < 5000; ++i) ++hits[r.below(5)];
  CHECK(*std::min_element(hits, hits + 5) > 800);
}

void test_model_format() {
  Mlp m(6, 5, 3);
  Rng rng(3);
  init_weights(m, rng);
  m.meta = {"profile", kFeatureVersion, 123, 1700000000, "base", 0.97};
  const float x[6] = {0.1f, 0.9f, 0.3f, 0.0f, 1.0f, 0.5f};
  std::vector<float> p1, p2;
  m.forward(x, p1);
  CHECK(p1.size() == 3 && near(p1[0] + p1[1] + p1[2], 1.0, 1e-5));

  const std::string text = m.serialize();
  auto back = Mlp::parse(text);
  CHECK(back.has_value());
  back->forward(x, p2);
  CHECK(p1 == p2 && back->meta.kind == "profile" && back->meta.trained_samples == 123 && back->meta.source == "base");   // exact round trip
  CHECK(back->checksum() == m.checksum() && m.checksum().size() == 64);

  std::string tampered = text;
  const auto pos = tampered.find("w1 ") + 5;
  tampered[pos] = tampered[pos] == '1' ? '2' : '1';
  CHECK(!Mlp::parse(tampered));                                            // edited weights: checksum mismatch
  CHECK(!Mlp::parse(text.substr(0, text.size() / 2)));                     // truncated
  CHECK(!Mlp::parse("OPTMODEL 1\nkind x\n"));                              // no checksum
  CHECK(!Mlp::parse(""));
  auto with_sum = [](const std::string& body) { return body + "sha256 " + sha256_hex(body) + "\n"; };
  CHECK(!Mlp::parse(with_sum("OPTMODEL 1\nkind x\nfeature_version 1\ninput 9999\nhidden 4\noutput 3\n")));      // oversize dims
  CHECK(!Mlp::parse(with_sum("OPTMODEL 2\nkind x\nfeature_version 1\ninput 2\nhidden 1\noutput 2\nw1 1 1\nb1 0\nw2 1 1\nb2 0 0\n")));  // unknown version
  CHECK(!Mlp::parse(with_sum("OPTMODEL 1\nkind x\nfeature_version 1\ninput 2\nhidden 1\noutput 2\nw1 1\nb1 0\nw2 1 1\nb2 0 0\n")));    // wrong count
  Mlp bad(2, 1, 2);
  bad.w1[0] = std::nanf("");
  CHECK(!Mlp::parse(bad.serialize()));                                     // NaN weights refused

  const auto d = tmpdir();
  CHECK(save_model(d / "m/a.model", m).has_value());
  auto loaded = load_model(d / "m/a.model");
  CHECK(loaded && loaded->checksum() == m.checksum());
  CHECK(!load_model(d / "missing.model"));
  { std::ofstream(d / "m/junk.model") << "junk\n"; }
  CHECK(!load_model(d / "m/junk.model"));
  fs::remove_all(d);
}

void test_fit() {
  // 3 well separated clusters + an XOR-like class so the hidden layer matters.
  Rng r(11);
  auto make = [&](int n) {
    std::vector<Sample> v;
    for (int i = 0; i < n; ++i) {
      const double a = r.uniform(), b = r.uniform();
      Sample s;
      s.x = {static_cast<float>(a), static_cast<float>(b)};
      s.y = ((a > 0.5) != (b > 0.5)) ? 1 : 0;
      v.push_back(std::move(s));
    }
    return v;
  };
  const auto train = make(1500), val = make(400);
  auto run = [&](std::uint64_t seed) {
    Mlp m(2, 12, 2);
    Rng init(seed);
    init_weights(m, init);
    FitConfig cfg;
    cfg.epochs = 60;
    cfg.seed = seed;
    const auto res = fit(m, train, val, cfg);
    return std::make_pair(std::move(m), res);
  };
  auto [m1, r1] = run(5);
  auto [m2, r2] = run(5);
  CHECK(r1.val_accuracy > 0.93 && r1.epochs_run == 60 && !r1.stopped);     // learns a non-linear boundary
  CHECK(m1.checksum() == m2.checksum());                                    // deterministic

  Mlp m(2, 12, 2);
  Rng init(1);
  init_weights(m, init);
  std::stop_source src;
  src.request_stop();
  CHECK(fit(m, train, val, FitConfig{}, src.get_token()).stopped);          // honours the stop token
  int calls = 0;
  const auto cooperative = fit(m, train, val, FitConfig{}, {}, [&] { return ++calls < 3; });
  CHECK(cooperative.stopped && calls == 3);                                  // yields when asked to
}

void test_features() {
  const workload::ProcessSnapshot empty;
  const auto f0 = extract_features(empty, nullptr, FeatureEnv{});
  bool ok = true;
  for (float v : f0) ok &= std::isfinite(v) && v >= 0.0f && v <= 1.0f;
  CHECK(ok);
  std::set<std::string> names;
  for (int i = 0; i < kFeatureCount; ++i) names.insert(feature_name(i));
  CHECK(names.size() == static_cast<std::size_t>(kFeatureCount) && !names.count("?"));

  World w(85);
  w.add("gnome-shell", 4);
  for (int i = 0; i < 4; ++i) w.add("cc1plus", 90);
  w.add("ninja", 1);
  w.io(1000, 5000);                 // far above the scaling caps
  w.sys.psi = monitor::PsiSample{};
  w.sys.psi->cpu = monitor::PsiResource{};
  w.sys.psi->cpu->some = monitor::PsiLine{250, 0, 0, 0};   // absurd value must still be clipped
  const auto f = w.features();
  ok = true;
  for (float v : f) ok &= std::isfinite(v) && v >= 0.0f && v <= 1.0f;
  CHECK(ok && near(f[F_CPU_UTIL], 0.85) && f[F_COMPILE_WORKERS] == 0.5f && f[F_COMPILE_ORCH] == 1.0f && near(f[F_COMPOSITOR], 0.04));
  CHECK(f[F_PSI_CPU] == 1.0f && f[F_IO_READ] > 0.9f && near(f[F_CPU_FREQ_RATIO], 2000.0 / 4400.0));

  ProfileFeatureInput pin;
  pin.cls = WorkloadClass::Game;
  pin.confidence = 0.8;
  pin.flags = workload::kHighLoad | workload::kLatencySensitive;
  pin.on_ac = false;
  pin.battery_pct = 40;
  pin.thermal_margin_c = 30;
  pin.aggressiveness = 4;
  auto pf = profile_features(pin);
  CHECK(pf[static_cast<std::size_t>(WorkloadClass::Game)] == 1.0f && pf[14] == 1.0f && pf[15] == 1.0f && pf[16] == 0.0f);
  CHECK(pf[17] == 0.0f && near(pf[18], 0.4) && near(pf[19], 0.5) && pf[22] == 1.0f);
  pin.confidence = 0.1;  // below the rule table's threshold: no class information
  pf = profile_features(pin);
  CHECK(std::all_of(pf.begin(), pf.begin() + 14, [](float v) { return v == 0.0f; }));
  pin.on_ac.reset();
  CHECK(profile_features(pin)[17] == 0.5f);                                 // unknown power source is not guessed
}

void test_base_models() {
  const auto& m = shared_models();
  CHECK(m.workload.out() == static_cast<int>(workload::kClassCount) && m.profile.out() == 3);
  CHECK(m.workload.meta.kind == "workload" && m.workload.meta.source == "base" && m.workload.meta.feature_version == kFeatureVersion);
  Rng fresh(4242);
  const auto wd = make_workload_dataset(120, fresh);
  const auto pd = make_profile_dataset(4000, fresh);
  CHECK(accuracy(m.workload, wd) > 0.95);
  CHECK(accuracy(m.profile, pd) > 0.93);                                    // the profile model reproduces the rule table

  BaseBuildOptions tiny;
  tiny.per_class = 30;
  tiny.profile_samples = 500;
  tiny.epochs_workload = 3;
  tiny.epochs_profile = 3;
  auto a = build_base_models(tiny), b = build_base_models(tiny);
  CHECK(a && b && a->workload.w1 == b->workload.w1 && a->workload.w2 == b->workload.w2 && a->profile.w1 == b->profile.w1 &&
        a->profile.b2 == b->profile.b2);  // reproducible (trained_at is metadata and intentionally differs)
  std::stop_source src;
  src.request_stop();
  auto stopped = build_base_models(small_options(), src.get_token());
  CHECK(!stopped && stopped.error().code == Errc::Timeout);
}

void test_scenarios() {
  const auto& m = shared_models();
  struct Case { const char* name; WorkloadClass expect; World w; };
  std::vector<Case> cases;
  auto add = [&](const char* n, WorkloadClass c, World w) { cases.push_back({n, c, std::move(w)}); };
  { World w(2); w.add("gnome-shell", 1.0); add("idle", WorkloadClass::Idle, std::move(w)); }
  { World w(25); w.add("gnome-shell", 9.0); w.add("nautilus", 6.0); w.add("gnome-terminal", 8.0); w.add("code", 22.0); add("desktop", WorkloadClass::Desktop, std::move(w)); }
  { World w(85); w.add("gnome-shell", 2); w.add("ninja", 1); w.add("cc1plus", 95); w.add("cc1plus", 90); w.add("cc1plus", 80); add("compile", WorkloadClass::Compile, std::move(w)); }
  { World w(60); w.add("ffmpeg", 280); add("encode", WorkloadClass::Encode, std::move(w)); }
  { World w(70); w.add("blender", 300); w.gpu(70); add("render", WorkloadClass::Render, std::move(w)); }
  { World w(40); w.add("zstd", 100); w.io(80, 60); add("archive", WorkloadClass::Archive, std::move(w)); }
  { World w(15); w.add("rsync", 5); w.io(90, 85); add("file copy", WorkloadClass::FileCopy, std::move(w)); }
  { World w(50); w.add("qemu-system-x86", 120, "0::/machine.slice/x.scope"); add("vm", WorkloadClass::Vm, std::move(w)); }
  { World w(30); w.add("postgres", 30, "0::/docker/abc123"); w.add("dockerd", 2, "0::/system.slice/docker.service"); add("container", WorkloadClass::Container, std::move(w)); }
  { World w(12); w.add("nginx", 8, "0::/system.slice/nginx.service"); w.ps.listen_ports_nonlocal = 2; add("server", WorkloadClass::Server, std::move(w)); }
  { World w(25); w.add("firefox", 10); w.add("web content", 12); w.add("web content", 8); w.add("gnome-shell", 6); add("browser", WorkloadClass::Browser, std::move(w)); }
  { World w(20); w.add("mpv", 12); w.add("gnome-shell", 3); w.ps.audio_running = true; w.gpu(15); add("video", WorkloadClass::VideoPlayback, std::move(w)); }
  { World w(55); w.add("steam", 2); w.add("gnome-shell", 8); w.add("game", 160, "0::/user.slice/app.scope", workload::kCmdSteamApp); w.gpu(80); add("game", WorkloadClass::Game, std::move(w)); }
  int correct = 0;
  for (auto& c : cases) {
    const auto f = c.w.features();
    const auto got = static_cast<WorkloadClass>(m.workload.argmax(f.data()));
    if (got == c.expect) ++correct;
    else std::fprintf(stderr, "scenario %s: expected %s got %s\n", c.name, std::string(to_string(c.expect)).c_str(), std::string(to_string(got)).c_str());
  }
  CHECK(correct >= 12);  // of 14: the learned model must agree with the rule classifier on typical situations
}

void test_agreement_and_forecast() {
  AgreementTracker a;
  CHECK(a.factor() == 0.0);
  for (int i = 0; i < 25; ++i) a.add(true);
  CHECK(near(a.value, 1.0) && near(a.factor(), 0.5));                       // half-way through the warm-up
  for (int i = 0; i < 25; ++i) a.add(true);
  CHECK(near(a.factor(), 1.0));
  AgreementTracker m;
  for (int i = 0; i < 50; ++i) m.add(i % 5 != 0);                            // 80 % agreement
  CHECK(near(m.value, 0.8) && near(m.factor(), 0.8));
  for (int i = 0; i < 10; ++i) m.add(false);
  CHECK(m.value < 0.8 && m.value > 0.6);                                     // slow EMA: a few misses do not collapse it

  const auto d = tmpdir();
  Predictor p(d / "ml", Predictor::Options{});
  const FeatureVector fv{};
  const ProfileFeatureInput pin;
  auto rising = [&](double v) { return p.evaluate(fv, pin, v).forecast; };
  Forecast f;
  for (double v : {10.0, 20.0, 30.0, 40.0, 50.0}) f = rising(v);
  CHECK(f.trend == "rising" && f.cpu_next > f.cpu_now && f.cpu_next <= 100.0);
  Predictor flat(d / "ml2", Predictor::Options{});
  for (int i = 0; i < 12; ++i) f = flat.evaluate(fv, pin, 20.0).forecast;
  CHECK(f.trend == "flat" && near(f.cpu_next, 20.0, 1.0) && f.rmse < 1.0 && f.n >= 10);
  Predictor falling(d / "ml3", Predictor::Options{});
  for (double v : {90.0, 70.0, 50.0, 30.0, 10.0}) f = falling.evaluate(fv, pin, v).forecast;
  CHECK(f.trend == "falling" && f.cpu_next >= 0.0);
  const auto none = flat.evaluate(fv, pin, std::nullopt).forecast;
  CHECK(none.cpu_next == 0.0 && none.n == 0);                                // no measurement: no invented forecast
  fs::remove_all(d);
}

void test_predictor_lifecycle() {
  const auto d = tmpdir();
  Predictor::Options o;
  o.build.per_class = 40;
  o.build.profile_samples = 800;
  o.build.epochs_workload = 6;
  o.build.epochs_profile = 6;
  {
    Predictor p(d / "ml", o);
    CHECK(!Predictor::cache_ok(d / "ml"));
    p.start();
    CHECK(!p.evaluate(FeatureVector{}, ProfileFeatureInput{}, 10.0).ready || p.ready());   // never crashes before the models exist
    for (int i = 0; i < 1200 && !p.ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(p.ready() && !p.status().bootstrapping && p.status().error.empty());
    CHECK(Predictor::cache_ok(d / "ml") && fs::exists(d / "ml/base-workload-v1.model"));
    const auto pred = p.evaluate(FeatureVector{}, ProfileFeatureInput{}, 10.0);
    CHECK(pred.ready && pred.advice.available && pred.workload_prob > 0.0 && pred.advice.confidence == 0.0);   // no agreement yet => rules lead
    for (int i = 0; i < 60; ++i) {
      const auto q = p.evaluate(FeatureVector{}, ProfileFeatureInput{}, 10.0);
      p.record_outcome(q.workload, q.advice.suggested);                                                        // perfect agreement
    }
    const auto after = p.evaluate(FeatureVector{}, ProfileFeatureInput{}, 10.0);
    CHECK(after.advice.confidence > 0.3 && after.advice.confidence <= 1.0 && near(p.status().profile_agreement.value, 1.0));
    CHECK(format_prediction(p.status()).find("Agreement with the rules") != std::string::npos);
  }
  {  // second start: cached models load immediately, no build thread
    Predictor p(d / "ml", o);
    p.start();
    CHECK(p.ready() && !p.status().bootstrapping);
  }
  {  // corrupt cache and wrong feature version are rejected (and rebuilt on start)
    { std::ofstream(d / "ml/base-profile-v1.model") << "garbage\n"; }
    CHECK(!Predictor::cache_ok(d / "ml"));
    Mlp wrong(kFeatureCount, 4, static_cast<int>(workload::kClassCount));
    wrong.meta = {"workload", 99, 0, 0, "base", 0.0};
    CHECK(save_model(d / "ml/base-workload-v1.model", wrong).has_value() && !Predictor::cache_ok(d / "ml"));
    Predictor p(d / "ml", o);
    p.start();
    for (int i = 0; i < 1200 && !p.ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(p.ready() && Predictor::cache_ok(d / "ml"));
  }
  {  // stopping during the one-time build is prompt and leaves nothing half-written
    const auto d2 = tmpdir();
    Predictor p(d2 / "ml", Predictor::Options{});
    p.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto t0 = std::chrono::steady_clock::now();
    p.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < seconds(5) && !p.ready() && !fs::exists(d2 / "ml/base-workload-v1.model"));
    fs::remove_all(d2);
  }
  fs::remove_all(d);
}

// ---------- policy integration: the ML layer can only suggest ----------
monitor::SystemSnapshot sys_with(std::optional<bool> ac, int battery, std::optional<double> margin) {
  monitor::SystemSnapshot s;
  s.power = monitor::PowerSample{};
  s.power->on_ac = ac;
  s.power->battery_pct = battery;
  if (margin) {
    s.thermal = monitor::ThermalSample{};
    monitor::ThermalZone z;
    z.temp_c = 100.0 - *margin;
    z.trips.push_back({100.0, "hot"});
    s.thermal->zones.push_back(z);
  }
  return s;
}

MlAdvice advice(Profile p, double conf) {
  MlAdvice a;
  a.available = true;
  a.suggested = p;
  a.confidence = conf;
  a.note = "test";
  return a;
}

struct Eval {
  policy::PolicyDecision d;
};

policy::PolicyDecision run_engine(const MlAdvice* ml, bool suggest, WorkloadClass cls, unsigned flags, const monitor::SystemSnapshot& sys,
                                  policy::UserMode user = {}, SafetyState safety = SafetyState::Normal, int aggr = 3) {
  policy::PolicyEngine e;
  workload::WorkloadResult wl;
  wl.cls = cls;
  wl.confidence = 0.9;
  wl.flags = flags;
  policy::PolicyInput in;
  in.user = user;
  in.aggressiveness = aggr;
  in.safety = safety;
  in.workload = &wl;
  in.sys = &sys;
  in.ml = ml;
  in.ml_suggest = suggest;
  in.ml_min_confidence = 0.75;
  in.now = monitor::Clock::now();
  return e.evaluate(in);
}

void test_policy_integration() {
  const auto ac_cool = sys_with(true, 100, 40.0);   // AC, 40 C thermal margin
  // Desktop on AC: the rules say BALANCED.
  {
    auto base = run_engine(nullptr, false, WorkloadClass::Desktop, 0, ac_cool);
    CHECK(base.profile == Profile::Balanced && base.rule_valid && base.rule_profile == Profile::Balanced && !base.ml);
  }
  {  // observe mode: recorded, never applied
    const auto a = advice(Profile::Performance, 0.95);
    auto d = run_engine(&a, false, WorkloadClass::Desktop, 0, ac_cool);
    CHECK(d.profile == Profile::Balanced && d.ml && !d.ml->used && d.ml->why.find("observe") != std::string::npos);
    CHECK(format_decision(d).find("ML advice") != std::string::npos);
  }
  {  // suggest mode, valid one-step increase on AC with thermal headroom: accepted and explained
    const auto a = advice(Profile::Performance, 0.9);
    auto d = run_engine(&a, true, WorkloadClass::Desktop, 0, ac_cool);
    CHECK(d.profile == Profile::Performance && d.ml && d.ml->used && d.rule_profile == Profile::Balanced);
    bool reason = false;
    for (const auto& r : d.reasons) reason |= r.find("[ML]") != std::string::npos && r.find("accepted") != std::string::npos;
    CHECK(reason);
  }
  auto ignored = [&](const char* name, const MlAdvice& a, WorkloadClass cls, unsigned flags, const monitor::SystemSnapshot& s,
                     policy::UserMode user, SafetyState safety, int aggr, Profile expect, const char* why) {
    auto d = run_engine(&a, true, cls, flags, s, user, safety, aggr);
    const bool ok = d.profile == expect && d.ml && !d.ml->used && d.ml->why.find(why) != std::string::npos;
    if (!ok) std::fprintf(stderr, "ML case '%s': profile=%s why='%s'\n", name, std::string(to_string(d.profile)).c_str(), d.ml ? d.ml->why.c_str() : "(none)");
    CHECK(ok);
  };
  const auto perf = advice(Profile::Performance, 0.9);
  ignored("low confidence", advice(Profile::Performance, 0.5), WorkloadClass::Desktop, 0, ac_cool, {}, SafetyState::Normal, 3, Profile::Balanced, "effective confidence");
  ignored("two steps", perf, WorkloadClass::Idle, 0, ac_cool, {}, SafetyState::Normal, 3, Profile::Powersave, "more than one step");  // rules: idle -> POWERSAVE
  ignored("battery", perf, WorkloadClass::Desktop, 0, sys_with(false, 80, 40.0), {}, SafetyState::Normal, 3, Profile::Balanced, "only on AC");
  ignored("unknown power source", perf, WorkloadClass::Desktop, 0, sys_with(std::nullopt, 80, 40.0), {}, SafetyState::Normal, 3, Profile::Balanced, "only on AC");
  ignored("no thermal data", perf, WorkloadClass::Desktop, 0, sys_with(true, 100, std::nullopt), {}, SafetyState::Normal, 3, Profile::Balanced, "thermal headroom");
  ignored("small margin", perf, WorkloadClass::Desktop, 0, sys_with(true, 100, 18.0), {}, SafetyState::Normal, 3, Profile::Balanced, "thermal headroom");
  {  // thermal derating (margin < 15 C) beats everything, including an ML suggestion
    auto d = run_engine(&perf, true, WorkloadClass::Desktop, 0, sys_with(true, 100, 12.0));
    CHECK(d.profile == Profile::Powersave && d.ml && !d.ml->used && d.ml->why.find("derating") != std::string::npos);
  }
  ignored("conservative level", perf, WorkloadClass::Desktop, 0, ac_cool, {}, SafetyState::Normal, 1, Profile::Balanced, "conservative");
  ignored("agrees", advice(Profile::Balanced, 0.99), WorkloadClass::Desktop, 0, ac_cool, {}, SafetyState::Normal, 3, Profile::Balanced, "agrees");
  ignored("latency lowering", advice(Profile::Powersave, 0.95), WorkloadClass::Desktop, workload::kLatencySensitive, ac_cool, {}, SafetyState::Normal, 3,
          Profile::Balanced, "latency-sensitive");
  ignored("critical battery", perf, WorkloadClass::Desktop, 0, sys_with(false, 10, 40.0), {}, SafetyState::Normal, 3, Profile::Powersave, "urgent");
  // Thermal restrictions always win over the ML layer.
  ignored("thermal WARM", perf, WorkloadClass::Desktop, 0, ac_cool, {}, SafetyState::Warm, 3, Profile::Balanced, "not NORMAL");
  ignored("thermal HOT", perf, WorkloadClass::Desktop, 0, ac_cool, {}, SafetyState::Hot, 3, Profile::Balanced, "not NORMAL");
  {
    auto d = run_engine(&perf, true, WorkloadClass::Compile, 0, ac_cool, {}, SafetyState::Hot);
    CHECK(d.profile == Profile::Balanced && d.safety_limited);                 // rules wanted PERFORMANCE; HOT caps it; ML cannot lift the cap
    auto e = run_engine(&perf, true, WorkloadClass::Compile, 0, ac_cool, {}, SafetyState::Emergency);
    CHECK(e.profile == Profile::ThermalEmergency);
  }
  // Manual and locked profiles are never touched by ML.
  ignored("manual", perf, WorkloadClass::Desktop, 0, ac_cool, {true, Profile::Powersave, false}, SafetyState::Normal, 3, Profile::Powersave, "manual/locked");
  ignored("locked", perf, WorkloadClass::Desktop, 0, ac_cool, {true, Profile::Powersave, true}, SafetyState::Normal, 3, Profile::Powersave, "manual/locked");
  {
    auto d = run_engine(&perf, true, WorkloadClass::Desktop, 0, ac_cool, {true, Profile::Powersave, true});
    CHECK(!d.rule_valid);                                                       // not counted in the agreement statistics
  }
  {  // advice unavailable (models not ready): no ML outcome at all
    MlAdvice none;
    auto d = run_engine(&none, true, WorkloadClass::Desktop, 0, ac_cool);
    CHECK(!d.ml && d.profile == Profile::Balanced);
  }
}

void test_config_ml() {
  auto d = parse_config("");
  CHECK(d && d->ml_enabled && d->ml_mode == "observe" && near(d->ml_min_confidence, 0.75));   // observe by default
  auto c = parse_config("[ml]\nenabled = off\nmode = suggest\nmin_confidence = 0.9\n");
  CHECK(c && !c->ml_enabled && c->ml_mode == "suggest" && near(c->ml_min_confidence, 0.9));
  CHECK(!parse_config("[ml]\nmode = auto\n"));
  CHECK(!parse_config("[ml]\nmin_confidence = 0.3\n"));
  CHECK(!parse_config("[ml]\nmin_confidence = 1.0\n"));
  CHECK(!parse_config("[ml]\nmin_confidence = high\n"));
  CHECK(!parse_config("[ml]\nenabled = yes\n"));
  CHECK(!parse_config("[ml]\nmodel_url = http://x\n"));                          // models are never downloaded
}

}  // namespace

void run_ml_tests() {
  log::set_level(log::Level::Safety);
  test_rng();
  test_model_format();
  test_fit();
  test_features();
  test_base_models();
  test_scenarios();
  test_agreement_and_forecast();
  test_predictor_lifecycle();
  test_policy_integration();
  test_config_ml();
}
