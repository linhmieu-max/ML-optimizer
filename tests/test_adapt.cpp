#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

#include "check.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "database/telemetry.hpp"
#include "ml/adaptation.hpp"
#include "ml/base_model.hpp"
#include "ml/dataset.hpp"
#include "ml/feature_vector.hpp"
#include "ml/predictor.hpp"
#include "ml/trainer.hpp"
#include "ml/training_gate.hpp"

namespace fs = std::filesystem;
using namespace opt;
using namespace opt::ml;
using workload::WorkloadClass;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

fs::path tmpdir() {
  char t[] = "/tmp/opt-adp-XXXXXX";
  return ::mkdtemp(t);
}
bool near(double a, double b, double e = 0.01) { return a > b - e && a < b + e; }
bool wait_until(const std::function<bool()>& pred, int timeout_ms = 20000) {
  for (int i = 0; i < timeout_ms / 10; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(milliseconds(10));
  }
  return pred();
}

// ---------------- trainer ----------------
struct Toy {
  std::vector<Sample> train, val;
};
Toy make_toy() {
  Rng r(5);
  Toy t;
  auto gen = [&](std::vector<Sample>& v, int n) {
    for (int i = 0; i < n; ++i) {
      const double a = r.uniform(), b = r.uniform();
      v.push_back({{static_cast<float>(a), static_cast<float>(b), static_cast<float>(a * b)}, ((a > 0.5) != (b > 0.5)) ? 1 : 0});
    }
  };
  gen(t.train, 300);
  gen(t.val, 100);
  return t;
}
Mlp toy_model(std::uint64_t seed) {
  Mlp m(3, 8, 2);
  Rng r(seed);
  init_weights(m, r);
  m.meta = {"toy", kFeatureVersion, 0, 0, "base", 0.0};
  return m;
}

void test_trainer_resume_equivalence() {
  const auto toy = make_toy();
  FitConfig cfg;
  cfg.epochs = 5;
  cfg.batch = 16;
  cfg.seed = 9;

  Trainer straight(toy_model(1), toy.train, toy.val, cfg);
  CHECK(straight.run(nullptr) == Trainer::Status::Finished && straight.finished() && near(straight.progress(), 1.0));

  // Interrupt after every few batches (including in the middle of an epoch), serialise, restore, continue.
  const std::string fp = dataset_fingerprint("toy", toy.train, toy.val);
  Trainer paused(toy_model(1), toy.train, toy.val, cfg);
  int rounds = 0;
  while (!paused.finished()) {
    paused.run(nullptr, 1 + (rounds % 7));  // 1..7 batches per slice
    ++rounds;
    const std::string ck = paused.checkpoint("toy", fp);
    auto restored = Trainer::restore(ck, toy.train, toy.val, cfg, "toy", fp);
    CHECK(restored.has_value());
    if (!restored) return;
    paused = std::move(*restored);
  }
  CHECK(rounds > 10);
  CHECK(paused.model().checksum() == straight.model().checksum());   // pausing/resuming changes nothing
  CHECK(near(paused.train_loss(), straight.train_loss(), 1e-9));

  // keep_going() is honoured before every batch; progress is monotonic
  Trainer t(toy_model(1), toy.train, toy.val, cfg);
  int calls = 0;
  CHECK(t.run([&] { return ++calls <= 3; }) == Trainer::Status::Paused && calls == 4);
  const double p1 = t.progress();
  t.run(nullptr, 5);
  CHECK(t.progress() > p1);
}

void test_checkpoint_validation() {
  const auto toy = make_toy();
  FitConfig cfg;
  cfg.epochs = 3;
  cfg.batch = 16;
  cfg.seed = 3;
  const std::string fp = dataset_fingerprint("toy", toy.train, toy.val);
  Trainer t(toy_model(2), toy.train, toy.val, cfg);
  t.run(nullptr, 7);
  const std::string ck = t.checkpoint("toy", fp);
  CHECK(Trainer::restore(ck, toy.train, toy.val, cfg, "toy", fp).has_value());

  std::string flipped = ck;
  const auto pos = flipped.find("order ") + 8;
  flipped[pos] = flipped[pos] == '1' ? '2' : '1';
  CHECK(!Trainer::restore(flipped, toy.train, toy.val, cfg, "toy", fp));                      // edited: checksum
  CHECK(!Trainer::restore(ck.substr(0, ck.size() / 2), toy.train, toy.val, cfg, "toy", fp));  // truncated
  CHECK(!Trainer::restore("", toy.train, toy.val, cfg, "toy", fp));
  CHECK(!Trainer::restore(ck, toy.train, toy.val, cfg, "other", fp));                         // another model kind
  CHECK(!Trainer::restore(ck, toy.train, toy.val, cfg, "toy", "0000"));                       // other data
  auto fewer = toy.train;
  fewer.pop_back();
  CHECK(!Trainer::restore(ck, fewer, toy.val, cfg, "toy", dataset_fingerprint("toy", fewer, toy.val)));  // order/size differs
  FitConfig other = cfg;
  other.epochs = 4;
  CHECK(!Trainer::restore(ck, toy.train, toy.val, other, "toy", fp));                         // other configuration
  other = cfg;
  other.seed = 4;
  CHECK(!Trainer::restore(ck, toy.train, toy.val, other, "toy", fp));
  CHECK(dataset_fingerprint("toy", toy.train, toy.val) != dataset_fingerprint("toy", fewer, toy.val));
  auto relabelled = toy.train;
  relabelled[0].y ^= 1;
  CHECK(dataset_fingerprint("toy", relabelled, toy.val) != fp);
}

// ---------------- dataset ----------------
db::MlSampleRow sample(std::int64_t ts, int wl, double conf = 0.9) {
  db::MlSampleRow r;
  r.ts = ts;
  r.workload = wl;
  r.wl_confidence = conf;
  FeatureVector f{};
  r.features = quantize(f.data(), kFeatureCount);
  ProfileFeatures p{};
  r.pfeatures = quantize(p.data(), kProfileFeatureCount);
  return r;
}

void test_dataset() {
  const float v[4] = {0.0f, 0.5f, 1.0f, 0.123f};
  const auto q = quantize(v, 4);
  const auto d = dequantize(q, 4);
  CHECK(q.size() == 4 && d.size() == 4 && d[0] == 0.0f && d[2] == 1.0f && std::fabs(d[3] - 0.123f) < 1.0f / 255 && std::fabs(d[1] - 0.5f) < 1.0f / 255);
  CHECK(dequantize(q, 5).empty());                                       // wrong size: rejected, never misread
  const float wild[2] = {-3.0f, 7.0f};
  CHECK(quantize(wild, 2) == std::string("\x00\xff", 2));                 // out-of-range values are clamped

  CHECK(workload_label(sample(1, 4, 0.9)) == 4 && !workload_label(sample(1, 4, 0.4)) && !workload_label(sample(1, 99, 0.9)));

  auto ps = [](bool auto_src, bool apply, int rule, int applied, bool ok, std::optional<double> reward) {
    db::MlSampleRow r = sample(1, 1);
    r.auto_source = auto_src; r.controllers_apply = apply; r.rule_profile = rule; r.applied_profile = applied; r.success = ok; r.reward = reward;
    return r;
  };
  CHECK(profile_label(ps(true, true, 2, 2, true, 0.5)) == 2);             // worked well: reinforced
  CHECK(profile_label(ps(true, true, 2, 2, false, 0.5)) == 1);            // failed (thermal HOT...): one step lower
  CHECK(profile_label(ps(true, true, 1, 1, false, std::nullopt)) == 0);
  CHECK(profile_label(ps(true, true, 0, 0, false, std::nullopt)) == 0);   // cannot go lower
  CHECK(!profile_label(ps(true, true, 2, 2, true, 0.05)));                // mediocre outcome: no label
  CHECK(!profile_label(ps(true, true, 2, 2, true, std::nullopt)));
  CHECK(!profile_label(ps(false, true, 1, 1, true, 0.9)));                // user/safety chose the profile
  CHECK(!profile_label(ps(true, false, 1, 1, true, 0.9)));                // controllers were not applying: outcome unrelated to the profile
  CHECK(!profile_label(ps(true, true, 2, 1, true, 0.9)));                 // applied differs from what the rules chose (held/derated)
  CHECK(!profile_label(ps(true, true, 3, 3, false, 0.9)));                // THERMAL_EMERGENCY is never a label

  int val = 0;
  for (std::int64_t ts = 1'700'000'000; ts < 1'700'000'000 + 20000; ts += 10) val += is_validation_sample(ts) ? 1 : 0;
  CHECK(val > 100 && val < 300 && is_validation_sample(1'700'000'010) == is_validation_sample(1'700'000'010));  // ~10 %, deterministic

  std::vector<db::MlSampleRow> rows;
  for (int i = 0; i < 400; ++i) rows.push_back(sample(1'700'000'000 + i * 10, i % 14, i % 7 == 0 ? 0.2 : 0.9));
  const auto w = build_workload_sets(rows, 10, 1);
  CHECK(w.train.size() == 140 + static_cast<std::size_t>(w.local_train) && w.val.size() == static_cast<std::size_t>(w.local_val) && w.local_val > 10 && w.local_train > 200);
  const auto again = build_workload_sets(rows, 10, 1);
  CHECK(again.train.size() == w.train.size() && again.train.back().y == w.train.back().y && dataset_fingerprint("w", w.train, w.val) == dataset_fingerprint("w", again.train, again.val));
}

// ---------------- gate (spec section 17) ----------------
GateInput idle_input() {
  GateInput g;
  g.cls = WorkloadClass::Idle;
  g.confidence = 0.8;
  g.cpu_pct = 3; g.gpu_pct = 2; g.psi_cpu = 0; g.psi_mem = 0; g.psi_io = 0.1;
  g.thermal_margin_c = 50;
  g.mem_available_kb = 4'000'000; g.mem_total_kb = 8'000'000;
  g.on_ac = true;
  return g;
}

void test_gate() {
  const auto t0 = Clock::now();
  {  // important workloads: never
    for (auto c : {WorkloadClass::Game, WorkloadClass::Compile, WorkloadClass::Render, WorkloadClass::Encode, WorkloadClass::Desktop, WorkloadClass::Browser,
                   WorkloadClass::Server, WorkloadClass::VideoPlayback, WorkloadClass::Vm, WorkloadClass::Container, WorkloadClass::FileCopy, WorkloadClass::Archive}) {
      TrainingGate g(GateConfig{.idle_seconds = 0});
      auto in = idle_input();
      in.cls = c;
      const auto d = g.evaluate(in, t0);
      CHECK(d.klass == TrainingClass::Blocked && d.reason.find("active") != std::string::npos);
    }
  }
  {  // idle: allowed, but only after sustained idleness
    TrainingGate g(GateConfig{.idle_seconds = 60});
    CHECK(g.evaluate(idle_input(), t0).klass == TrainingClass::Blocked);
    const auto early = g.evaluate(idle_input(), t0 + seconds(30));
    CHECK(early.klass == TrainingClass::Blocked && early.reason.find("sustained idle") != std::string::npos);
    CHECK(g.evaluate(idle_input(), t0 + seconds(61)).klass == TrainingClass::Allowed);
    auto busy = idle_input();
    busy.cls = WorkloadClass::Compile;
    CHECK(g.evaluate(busy, t0 + seconds(62)).klass == TrainingClass::Blocked);          // a workload appears: blocked immediately
    CHECK(g.evaluate(idle_input(), t0 + seconds(63)).klass == TrainingClass::Blocked);   // the idle clock restarted
    CHECK(g.evaluate(idle_input(), t0 + seconds(124)).klass == TrainingClass::Allowed);
  }
  {  // background: limited duty cycle, with its own CPU ceiling
    TrainingGate g(GateConfig{.idle_seconds = 0});
    auto in = idle_input();
    in.cls = WorkloadClass::Background;
    in.cpu_pct = 30;
    CHECK(g.evaluate(in, t0).klass == TrainingClass::Limited);
    in.cpu_pct = 60;
    CHECK(g.evaluate(in, t0).klass == TrainingClass::Blocked);
  }
  // every "really idle" condition matters
  auto blocked = [&](const char* why, const std::function<void(GateInput&)>& mutate, const std::function<void(GateConfig&)>& cfg = nullptr) {
    GateConfig c;
    c.idle_seconds = 0;
    if (cfg) cfg(c);
    TrainingGate g(c);
    auto in = idle_input();
    mutate(in);
    const auto d = g.evaluate(in, t0);
    if (d.klass != TrainingClass::Blocked) std::fprintf(stderr, "gate case '%s' was not blocked\n", why);
    CHECK(d.klass == TrainingClass::Blocked);
  };
  blocked("cpu", [](GateInput& g) { g.cpu_pct = 40; });
  blocked("gpu", [](GateInput& g) { g.gpu_pct = 50; });
  blocked("psi cpu", [](GateInput& g) { g.psi_cpu = 12; });
  blocked("psi io", [](GateInput& g) { g.psi_io = 12; });
  blocked("psi mem", [](GateInput& g) { g.psi_mem = 12; });
  blocked("thermal margin", [](GateInput& g) { g.thermal_margin_c = 10; });
  blocked("thermal WARM", [](GateInput& g) { g.safety = SafetyState::Warm; });
  blocked("thermal HOT", [](GateInput& g) { g.safety = SafetyState::Hot; });
  blocked("thermal EMERGENCY", [](GateInput& g) { g.safety = SafetyState::Emergency; });
  blocked("low free memory", [](GateInput& g) { g.mem_available_kb = 600'000; });
  blocked("low free memory fraction", [](GateInput& g) { g.mem_available_kb = 1'200'000; g.mem_total_kb = 16'000'000; });
  blocked("battery", [](GateInput& g) { g.on_ac = false; g.battery_pct = 95; });
  blocked("battery too low even if allowed", [](GateInput& g) { g.on_ac = false; g.battery_pct = 50; }, [](GateConfig& c) { c.allow_on_battery = true; });
  blocked("audio playing", [](GateInput& g) { g.audio_running = true; });
  blocked("latency sensitive", [](GateInput& g) { g.flags = workload::kLatencySensitive; });
  blocked("unclassified", [](GateInput& g) { g.confidence = 0.1; });
  {
    TrainingGate g(GateConfig{.allow_on_battery = true, .battery_min_pct = 80, .idle_seconds = 0});
    auto in = idle_input();
    in.on_ac = false;
    in.battery_pct = 90;
    CHECK(g.evaluate(in, t0).klass == TrainingClass::Allowed);       // opt-in: charged battery is acceptable
  }
  {  // unknown power source (desktop PC) and unknown thermal data do not block by themselves
    TrainingGate g(GateConfig{.idle_seconds = 0});
    auto in = idle_input();
    in.on_ac.reset();
    in.thermal_margin_c.reset();
    CHECK(g.evaluate(in, t0).klass == TrainingClass::Allowed);
  }
}

// ---------------- store: samples, retention, migration ----------------
void test_store_samples() {
  auto s = db::TelemetryStore::open(":memory:", {});
  CHECK(s.has_value());
  for (int i = 0; i < 50; ++i) {
    auto r = sample(1000 + i, i % 2, 0.9);
    r.applied_profile = 2;
    r.rule_profile = 2;
    r.auto_source = true;
    r.controllers_apply = i % 2 == 0;
    r.reward = 0.4;
    r.success = i % 5 != 0;
    CHECK((*s)->insert_ml_sample(r).has_value());
  }
  auto all = (*s)->ml_samples(0, 5000, 1000);
  CHECK(all && all->size() == 50 && all->front().ts == 1000 && all->back().ts == 1049);         // oldest first
  CHECK(all->at(0).features.size() == kFeatureCount && all->at(0).pfeatures.size() == kProfileFeatureCount);
  auto newest = (*s)->ml_samples(0, 5000, 10);
  CHECK(newest && newest->size() == 10 && newest->front().ts == 1040);                            // the NEWEST ten
  auto window = (*s)->ml_samples(1010, 1019, 100);
  CHECK(window && window->size() == 10 && window->front().ts == 1010);
  CHECK((*s)->ml_sample_count(1040).value() == 10 && (*s)->ml_sample_count(0).value() == 50);
  auto ins = (*s)->ml_insights(0);
  CHECK(ins && ins->size() == 2);                                                                  // workload 0 and 1, profile 2
  for (const auto& i : *ins) CHECK(i.profile == 2 && i.samples == 25 && i.failures == 5 && i.avg_reward && near(*i.avg_reward, 0.4));
  CHECK(ins->at(0).applied_samples == 25 && ins->at(1).applied_samples == 0);

  // retention: age and row cap, independent of the telemetry retention
  (*s)->set_retention({7, 365, 64 << 20, 1, 20});
  auto rep = (*s)->maintain(1000 + 3 * 86400);
  CHECK(rep.has_value() && (*s)->ml_sample_count(0).value() == 0);                                 // older than ml_days = 1
  for (int i = 0; i < 60; ++i) CHECK((*s)->insert_ml_sample(sample(1000 + 3 * 86400 + i, 1)).has_value());
  CHECK((*s)->maintain(1000 + 3 * 86400 + 100).has_value() && (*s)->ml_sample_count(0).value() == 20);  // row cap keeps the newest
  CHECK((*s)->ml_samples(0, 1LL << 40, 100)->front().ts == 1000 + 3 * 86400 + 40);
  // deleting telemetry deletes the learning samples too
  CHECK((*s)->delete_all().has_value() && (*s)->ml_sample_count(0).value() == 0);
  CHECK((*s)->insert_ml_sample(sample(5, 1)).has_value());                                         // usable afterwards
}

void test_schema_migration() {
  const auto d = tmpdir();
  const auto f = d / "t.db";
  {  // a Phase 7 (version 1) database with data
    auto old = db::Database::open(f);
    CHECK(old.has_value());
    CHECK((*old)->exec("CREATE TABLE telemetry (id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, session_id TEXT NOT NULL, device_id_hash TEXT NOT NULL, "
                       "kernel_version TEXT, interval_s REAL, workload TEXT NOT NULL, workload_confidence REAL, cpu_utilization REAL, cpu_frequency REAL, "
                       "gpu_utilization REAL, gpu_frequency REAL, ram_used REAL, swap_activity REAL, psi_cpu REAL, psi_memory REAL, psi_io REAL, "
                       "temperature REAL, power REAL, energy_delta REAL, selected_policy TEXT NOT NULL, policy_parameters TEXT, fps REAL, frame_time REAL, "
                       "latency REAL, io_throughput REAL, safety_action TEXT, rollback INTEGER NOT NULL DEFAULT 0, reward REAL, success INTEGER NOT NULL DEFAULT 1)").has_value());
    CHECK((*old)->exec("INSERT INTO telemetry (ts, session_id, device_id_hash, workload, selected_policy) VALUES (100, 's', 'd', 'IDLE', 'BALANCED')").has_value());
    CHECK((*old)->exec("PRAGMA user_version=1").has_value());
  }
  auto s = db::TelemetryStore::open(f, {});
  CHECK(s.has_value());
  CHECK((*s)->stats()->detailed_rows == 1);                                                         // old data preserved
  CHECK((*s)->insert_ml_sample(sample(1, 1)).has_value() && (*s)->ml_sample_count(0).value() == 1); // new table exists
  s->reset();
  auto check = db::Database::open(f);
  CHECK((*check)->scalar_int("PRAGMA user_version").value() == 2);
  fs::remove_all(d);
}

// ---------------- predictor: local models ----------------
const BaseModels& tiny_models() {
  static const BaseModels m = [] {
    BaseBuildOptions o;
    o.per_class = 60;
    o.profile_samples = 2500;
    o.epochs_workload = 15;
    o.epochs_profile = 20;
    auto r = build_base_models(o);
    if (!r) std::exit(2);
    return std::move(*r);
  }();
  return m;
}

void seed_model_dir(const fs::path& dir) {
  CHECK(save_model(dir / "base-workload-v1.model", tiny_models().workload).has_value());
  CHECK(save_model(dir / "base-profile-v1.model", tiny_models().profile).has_value());
}

void test_predictor_local() {
  const auto d = tmpdir();
  seed_model_dir(d / "ml");
  Predictor p(d / "ml");
  p.start();
  CHECK(p.ready() && p.model("profile")->meta.source == "base");
  Mlp local = *p.model("profile");
  local.meta.kind = "profile";
  local.meta.source = "local";
  local.b2[0] += 0.5f;
  const auto before = p.model("profile")->checksum();
  CHECK(p.install_local(local).has_value() && p.model("profile")->meta.source == "local" && p.model("profile")->checksum() != before);
  Mlp wrong(5, 4, 3);
  wrong.meta = {"profile", kFeatureVersion, 0, 0, "local", 0};
  CHECK(!p.install_local(wrong));                                       // wrong layout is refused
  wrong = Mlp(kProfileFeatureCount, 4, 3);
  wrong.meta = {"profile", kFeatureVersion + 1, 0, 0, "local", 0};
  CHECK(!p.install_local(wrong));                                       // another feature version is refused
  wrong.meta.kind = "bogus";
  CHECK(!p.install_local(wrong));
  p.reset_local();
  CHECK(p.model("profile")->checksum() == before && p.model("profile")->meta.source == "base");

  // a valid local file is picked up at start; a damaged one is ignored
  CHECK(save_model(d / "ml/local-profile-v1.model", local).has_value());
  Predictor p2(d / "ml");
  p2.start();
  CHECK(p2.model("profile")->meta.source == "local" && p2.model("workload")->meta.source == "base");
  { std::ofstream(d / "ml/local-profile-v1.model") << "damaged\n"; }
  Predictor p3(d / "ml");
  p3.start();
  CHECK(p3.ready() && p3.model("profile")->meta.source == "base");
  fs::remove_all(d);
}

// ---------------- adapter ----------------
struct AdaptRig {
  fs::path dir = tmpdir();
  std::unique_ptr<db::TelemetryStore> store;
  std::unique_ptr<Predictor> predictor;
  AdaptationOptions opts;
  std::unique_ptr<Adapter> adapter;
  std::int64_t base_ts;

  AdaptRig() {
    seed_model_dir(dir / "ml");
    store = std::move(*db::TelemetryStore::open(":memory:", {}));
    predictor = std::make_unique<Predictor>(dir / "ml");
    predictor->start();
    base_ts = static_cast<std::int64_t>(std::time(nullptr)) - 100000;
    opts.dir = dir / "ml";
    opts.gate.idle_seconds = 0;
    opts.min_new_samples = 10;
    opts.interval = seconds(0);
    opts.replay_per_class = 20;
    opts.replay_profile = 2500;
    opts.epochs = 6;
    opts.lr = 0.01f;
    opts.poll = milliseconds(5);
    opts.checkpoint_every = seconds(1);
  }
  ~AdaptRig() {
    adapter.reset();
    predictor.reset();
    store.reset();
    fs::remove_all(dir);
  }
  void make_adapter() { adapter = std::make_unique<Adapter>(opts, *store, *predictor); }
  void allow() { adapter->update_gate(idle_input(), Clock::now()); }
  void block() {
    auto in = idle_input();
    in.cls = WorkloadClass::Compile;
    adapter->update_gate(in, Clock::now());
  }
  // This machine (8 GB, swapping) shows a desktop as heavy swap + memory pressure, unlike the synthetic archetype.
  void add_workload_samples(int n, bool consistent_with_base = false) {
    Rng r(21);
    for (int i = 0; i < n; ++i) {
      FeatureVector f{};
      if (consistent_with_base) {
        Rng g(static_cast<std::uint64_t>(i) + 1);
        auto archetypes = make_workload_dataset(1, g);
        const auto& a = archetypes[static_cast<std::size_t>(i % 14)];
        for (int k = 0; k < kFeatureCount; ++k) f[static_cast<std::size_t>(k)] = a.x[static_cast<std::size_t>(k)];
      } else {
        f[F_RAM_USED] = static_cast<float>(r.uniform(0.9, 0.98));
        f[F_SWAP] = static_cast<float>(r.uniform(0.6, 0.9));
        f[F_PSI_MEM] = static_cast<float>(r.uniform(0.25, 0.45));
        f[F_COMPOSITOR] = static_cast<float>(r.uniform(0.06, 0.12));
        f[F_SESSION] = static_cast<float>(r.uniform(0.15, 0.3));
        f[F_CPU_UTIL] = static_cast<float>(r.uniform(0.2, 0.4));
      }
      db::MlSampleRow s = sample(base_ts + i * 10, 0);
      s.workload = consistent_with_base ? predictor->model("workload")->argmax(f.data()) : static_cast<int>(WorkloadClass::Desktop);
      s.features = quantize(f.data(), kFeatureCount);
      CHECK(store->insert_ml_sample(s).has_value());
    }
  }
  // On this machine PERFORMANCE for COMPILE reaches thermal HOT when the margin is small.
  void add_profile_failures(int n) {
    Rng r(33);
    for (int i = 0; i < n; ++i) {
      ProfileFeatureInput in;
      in.cls = WorkloadClass::Compile;
      in.confidence = 0.9;
      in.flags = workload::kHighLoad;
      in.on_ac = true;
      in.battery_pct = 100;
      in.thermal_margin_c = r.uniform(5, 14);
      in.psi_cpu10 = 5;
      in.cpu_util = 90;
      in.aggressiveness = 3;
      const auto pf = profile_features(in);
      db::MlSampleRow s = sample(base_ts + 50000 + i * 10, 0);
      s.wl_confidence = 0.0;  // profile evidence only: not a labelled workload example
      s.pfeatures = quantize(pf.data(), kProfileFeatureCount);
      s.rule_profile = 2;
      s.applied_profile = 2;
      s.auto_source = true;
      s.controllers_apply = true;
      s.success = false;
      s.reward = -0.1;
      CHECK(store->insert_ml_sample(s).has_value());
    }
  }
  // ...and runs fine (good reward) when the machine has thermal headroom.
  void add_profile_successes(int n) {
    Rng r(34);
    for (int i = 0; i < n; ++i) {
      ProfileFeatureInput in;
      in.cls = WorkloadClass::Compile;
      in.confidence = 0.9;
      in.flags = workload::kHighLoad;
      in.on_ac = true;
      in.battery_pct = 100;
      in.thermal_margin_c = r.uniform(25, 60);
      in.psi_cpu10 = 5;
      in.cpu_util = 90;
      in.aggressiveness = 3;
      const auto pf = profile_features(in);
      db::MlSampleRow s = sample(base_ts + 70000 + i * 10, 0);
      s.wl_confidence = 0.0;
      s.pfeatures = quantize(pf.data(), kProfileFeatureCount);
      s.rule_profile = 2;
      s.applied_profile = 2;
      s.auto_source = true;
      s.controllers_apply = true;
      s.success = true;
      s.reward = 0.5;
      CHECK(store->insert_ml_sample(s).has_value());
    }
  }
  static ProfileFeatures compile_context(double margin) {
    ProfileFeatureInput in;
    in.cls = WorkloadClass::Compile;
    in.confidence = 0.9;
    in.flags = workload::kHighLoad;
    in.on_ac = true;
    in.battery_pct = 100;
    in.thermal_margin_c = margin;
    in.psi_cpu10 = 5;
    in.cpu_util = 90;
    in.aggressiveness = 3;
    return profile_features(in);
  }
  int suggestion(double margin) {
    const auto f = compile_context(margin);
    return predictor->model("profile")->argmax(f.data());
  }
};

void test_adapter_learns_and_pauses() {
  AdaptRig r;
  r.opts.slice_batches = 1;                    // slow, pausable progress for the test
  r.opts.poll = milliseconds(15);
  r.opts.epochs = 3;
  r.add_profile_failures(300);
  r.add_profile_successes(300);
  r.add_workload_samples(0);
  CHECK(r.suggestion(10) == 2 && r.suggestion(50) == 2);                  // base model = the rules: Performance everywhere
  r.make_adapter();
  r.adapter->start();
  r.block();                                                              // a workload is running
  std::this_thread::sleep_for(milliseconds(300));
  auto st = r.adapter->status();
  CHECK(st.state == AdaptState::Waiting && st.progress == 0.0 && !st.checkpoint_exists);   // nothing happens while blocked

  r.allow();
  CHECK(wait_until([&] { return r.adapter->status().state == AdaptState::Training && r.adapter->status().progress > 0.0; }));
  r.block();                                                              // a game starts
  CHECK(wait_until([&] { return r.adapter->status().state == AdaptState::Paused; }));
  st = r.adapter->status();
  CHECK(st.checkpoint_exists && st.pauses == 1 && st.gate.klass == TrainingClass::Blocked && st.progress < 1.0);
  const double frozen = st.progress;
  std::this_thread::sleep_for(milliseconds(300));
  CHECK(r.adapter->status().progress == frozen);                          // really stopped: no CPU used for training

  r.allow();                                                              // idle again: resumes from the checkpoint
  CHECK(wait_until([&] { return r.adapter->status().jobs_finished == 1; }, 60000));
  st = r.adapter->status();
  CHECK(st.resumes >= 1 && st.models_accepted == 1 && !st.checkpoint_exists);
  bool accepted = false, skipped = false;
  for (const auto& line : st.last_results) {
    accepted |= line.starts_with("profile: ACCEPTED");
    skipped |= line.starts_with("workload: skipped");
  }
  if (!(accepted && skipped)) for (const auto& l : st.last_results) std::fprintf(stderr, "result: %s\n", l.c_str());
  CHECK(accepted && skipped);
  // Machine-specific knowledge: hot context -> BALANCED, cool context still PERFORMANCE.
  CHECK(r.suggestion(10) == 1);
  CHECK(r.suggestion(50) == 2);
  CHECK(r.predictor->model("profile")->meta.source == "local" && fs::exists(r.dir / "ml/local-profile-v1.model"));

  // reset-local restores the base models and removes the files
  r.adapter->reset_local();
  CHECK(wait_until([&] { return !fs::exists(r.dir / "ml/local-profile-v1.model"); }));
  CHECK(wait_until([&] { return r.predictor->model("profile")->meta.source == "base"; }));
  CHECK(r.suggestion(10) == 2 && r.adapter->status().last_finished_unix == 0);
}

void test_adapter_workload_and_rejection() {
  {  // this machine's "desktop" differs from the prior: the workload model learns it
    AdaptRig r;
    r.add_workload_samples(700);
    const auto probe = [&] {
      FeatureVector f{};
      f[F_RAM_USED] = 0.94f; f[F_SWAP] = 0.75f; f[F_PSI_MEM] = 0.35f; f[F_COMPOSITOR] = 0.09f; f[F_SESSION] = 0.22f; f[F_CPU_UTIL] = 0.3f;
      return r.predictor->model("workload")->argmax(f.data());
    };
    const int before = probe();
    r.make_adapter();
    r.adapter->start();
    r.allow();
    CHECK(wait_until([&] { return r.adapter->status().jobs_finished == 1; }, 60000));
    const auto st = r.adapter->status();
    bool accepted = false;
    for (const auto& l : st.last_results) accepted |= l.starts_with("workload: ACCEPTED");
    CHECK(accepted && r.predictor->model("workload")->meta.source == "local" && probe() == static_cast<int>(WorkloadClass::Desktop));
    CHECK(before != static_cast<int>(WorkloadClass::Desktop));   // it really learned something new
    CHECK(fs::exists(r.dir / "ml/local-workload-v1.model") && !fs::exists(r.dir / "ml/adapt-checkpoint.ckpt") && !fs::exists(r.dir / "ml/adapt-job.state"));
    // The machine-specific model still knows the base prior (promotion required >= 80 % on the base hold-out).
    Rng fresh(4242);
    CHECK(accuracy(*r.predictor->model("workload"), make_workload_dataset(60, fresh)) > 0.8);
  }
  {  // nothing new to learn: the candidate is rejected and the current model stays
    AdaptRig r;
    r.add_workload_samples(700, /*consistent_with_base=*/true);
    const auto before = r.predictor->model("workload")->checksum();
    r.make_adapter();
    r.adapter->start();
    r.allow();
    CHECK(wait_until([&] { return r.adapter->status().jobs_finished == 1; }, 60000));
    const auto st = r.adapter->status();
    bool rejected = false;
    for (const auto& l : st.last_results) rejected |= l.starts_with("workload: rejected (no measurable improvement");
    CHECK(rejected && st.models_accepted == 0 && r.predictor->model("workload")->checksum() == before);
    CHECK(!fs::exists(r.dir / "ml/local-workload-v1.model"));
  }
  {  // too little labelled data: skipped, nothing trained
    AdaptRig r;
    r.add_workload_samples(40);
    r.make_adapter();
    r.adapter->start();
    r.allow();
    CHECK(wait_until([&] { return r.adapter->status().jobs_finished == 1; }, 60000));
    const auto st = r.adapter->status();
    CHECK(st.models_accepted == 0 && st.models_rejected == 0 && !st.last_results.empty() && st.last_results[0].find("skipped") != std::string::npos);
  }
}

void test_adapter_scheduling_and_controls() {
  AdaptRig r;
  r.opts.min_new_samples = 100000 > 100000 ? 0 : 500;   // threshold not met
  r.add_workload_samples(100);
  r.make_adapter();
  r.adapter->start();
  r.allow();
  CHECK(wait_until([&] { return r.adapter->status().detail.find("new samples collected") != std::string::npos; }));
  CHECK(r.adapter->status().state == AdaptState::Waiting && r.adapter->status().progress == 0.0);
  r.adapter->request_run();                              // admin override of the thresholds (the idle gate still applies)
  CHECK(wait_until([&] { return r.adapter->status().jobs_finished == 1; }, 60000));
  r.adapter->set_enabled(false);
  CHECK(wait_until([&] { return r.adapter->status().state == AdaptState::Disabled; }));
  r.adapter->request_run();
  std::this_thread::sleep_for(milliseconds(200));
  CHECK(r.adapter->status().jobs_finished == 1);          // disabled: nothing starts
  r.adapter->set_enabled(true);
  r.block();
  r.adapter->request_run();
  std::this_thread::sleep_for(milliseconds(200));
  CHECK(r.adapter->status().state == AdaptState::Waiting && r.adapter->status().jobs_finished == 1);  // blocked gate: no job even when forced
}

void test_adapter_survives_restart() {
  AdaptRig r;
  r.opts.slice_batches = 1;
  r.opts.poll = milliseconds(15);
  r.opts.epochs = 3;
  r.add_profile_failures(300);
  r.add_profile_successes(300);
  r.make_adapter();
  r.adapter->start();
  r.allow();
  CHECK(wait_until([&] { return r.adapter->status().progress > 0.05; }));
  r.adapter->stop();                                      // daemon shutdown mid-training: leaves a checkpoint
  const double at_stop = r.adapter->status().progress;
  CHECK(fs::exists(r.dir / "ml/adapt-checkpoint.ckpt") && fs::exists(r.dir / "ml/adapt-job.state") && at_stop < 1.0);
  r.adapter.reset();

  r.make_adapter();                                       // "restart": a new Adapter over the same files
  r.adapter->start();
  CHECK(wait_until([&] { return r.adapter->status().progress > 0.0 && r.adapter->status().state == AdaptState::Paused; }));
  if (!(r.adapter->status().progress >= at_stop * 0.9 && r.adapter->status().job_kind == "profile"))
    std::fprintf(stderr, "restart: progress=%.3f at_stop=%.3f kind='%s'\n", r.adapter->status().progress, at_stop, r.adapter->status().job_kind.c_str());
  CHECK(r.adapter->status().progress >= at_stop * 0.9 && r.adapter->status().job_kind == "profile");    // continues where it was
  r.allow();
  CHECK(wait_until([&] { return r.adapter->status().jobs_finished == 1; }, 60000));
  CHECK(r.adapter->status().models_accepted == 1 && r.suggestion(10) == 1);

  {  // a checkpoint for data that no longer exists is discarded, never applied
    AdaptRig q;
    q.opts.slice_batches = 1;
    q.opts.poll = milliseconds(15);
    q.opts.epochs = 3;
    q.add_profile_failures(300);
    q.make_adapter();
    q.adapter->start();
    q.allow();
    CHECK(wait_until([&] { return q.adapter->status().progress > 0.05; }));
    q.adapter->stop();
    q.adapter.reset();
    CHECK((q.store->delete_all()).has_value());           // the samples are gone (telemetry deleted)
    q.make_adapter();
    q.adapter->start();
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(q.adapter->status().progress == 0.0 && !fs::exists(q.dir / "ml/adapt-checkpoint.ckpt"));
  }
}

void test_config_training() {
  auto d = parse_config("");
  CHECK(d && d->ml_training && !d->ml_training_on_battery && d->ml_training_min_samples == 500 && d->ml_training_interval_hours == 6 &&
        d->ml_training_idle_seconds == 60);
  auto c = parse_config("[ml]\ntraining = off\ntraining_on_battery = on\ntraining_min_samples = 1000\ntraining_interval_hours = 12\ntraining_idle_seconds = 120\n");
  CHECK(c && !c->ml_training && c->ml_training_on_battery && c->ml_training_min_samples == 1000 && c->ml_training_interval_hours == 12 && c->ml_training_idle_seconds == 120);
  CHECK(!parse_config("[ml]\ntraining_min_samples = 5\n"));
  CHECK(!parse_config("[ml]\ntraining_interval_hours = 0\n"));
  CHECK(!parse_config("[ml]\ntraining_idle_seconds = 5\n"));
  CHECK(!parse_config("[ml]\ntraining = maybe\n"));
  CHECK(!parse_config("[ml]\ntraining_min_samples = many\n"));
}

}  // namespace

void run_adapt_tests() {
  log::set_level(log::Level::Safety);
  test_trainer_resume_equivalence();
  test_checkpoint_validation();
  test_dataset();
  test_gate();
  test_store_samples();
  test_schema_migration();
  test_predictor_local();
  test_adapter_learns_and_pauses();
  test_adapter_workload_and_rejection();
  test_adapter_scheduling_and_controls();
  test_adapter_survives_restart();
  test_config_training();
}
