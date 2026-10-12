#include "ml/base_model.hpp"

#include <algorithm>
#include <ctime>

#include "ml/feature_vector.hpp"
#include "policy/rules.hpp"

namespace opt::ml {
using workload::WorkloadClass;

namespace {

float clip(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

// Fills a feature vector for one synthetic example of class `c`.
Sample archetype(WorkloadClass c, Rng& r) {
  FeatureVector f{};
  auto set = [&](Feature i, double lo, double hi) { f[i] = clip(r.uniform(lo, hi)); };
  auto flag = [&](Feature i, double p) { f[i] = r.uniform() < p ? 1.0f : 0.0f; };
  // ambient noise present in every state
  set(F_CPU_UTIL, 0.0, 0.2); set(F_CPU_FREQ_RATIO, 0.1, 0.8); set(F_LOAD, 0.0, 0.3); set(F_RAM_USED, 0.2, 0.9);
  set(F_SWAP, 0.0, 0.1); set(F_PSI_CPU, 0.0, 0.1); set(F_PSI_MEM, 0.0, 0.05); set(F_PSI_IO, 0.0, 0.1);
  set(F_IO_READ, 0.0, 0.15); set(F_IO_WRITE, 0.0, 0.15); set(F_GPU_BUSY, 0.0, 0.15);
  set(F_COMPOSITOR, 0.0, 0.1); set(F_SESSION, 0.0, 0.05); set(F_SYSTEM, 0.0, 0.05);
  set(F_TOP_PROC, 0.0, 0.1); set(F_ACTIVE_PROCS, 0.0, 0.2);
  flag(F_LAUNCHER, 0.1); flag(F_AUDIO, 0.1);
  switch (c) {
    case WorkloadClass::Idle:
      set(F_CPU_UTIL, 0.0, 0.1); set(F_COMPOSITOR, 0.0, 0.025); set(F_SESSION, 0.0, 0.02); set(F_SYSTEM, 0.0, 0.02);
      set(F_TOP_PROC, 0.0, 0.03); set(F_ACTIVE_PROCS, 0.0, 0.05); set(F_IO_READ, 0.0, 0.05); set(F_IO_WRITE, 0.0, 0.05);
      f[F_AUDIO] = 0.0f; f[F_LAUNCHER] = 0.0f;
      break;
    case WorkloadClass::Desktop:
      set(F_COMPOSITOR, 0.03, 0.15); set(F_SESSION, 0.03, 0.4); set(F_CPU_UTIL, 0.05, 0.4); set(F_TOP_PROC, 0.05, 0.3);
      set(F_ACTIVE_PROCS, 0.1, 0.4);
      break;
    case WorkloadClass::Browser:
      set(F_BROWSER_CPU, 0.03, 0.6); set(F_BROWSER_PROCS, 0.2, 1.0); set(F_COMPOSITOR, 0.02, 0.1); set(F_CPU_UTIL, 0.08, 0.5);
      set(F_ACTIVE_PROCS, 0.1, 0.5); set(F_TOP_PROC, 0.05, 0.3); flag(F_AUDIO, 0.2);
      break;
    case WorkloadClass::VideoPlayback:
      if (r.uniform() < 0.7) { f[F_PLAYER_PRESENT] = 1.0f; set(F_PLAYER_CPU, 0.04, 0.4); set(F_GPU_BUSY, 0.05, 0.5); }
      else { set(F_BROWSER_CPU, 0.03, 0.3); set(F_BROWSER_PROCS, 0.3, 1.0); set(F_GPU_BUSY, 0.12, 0.5); }
      flag(F_AUDIO, 0.95); set(F_COMPOSITOR, 0.02, 0.1); set(F_CPU_UTIL, 0.05, 0.4);
      break;
    case WorkloadClass::Game:
      set(F_GAME_CPU, 0.1, 0.9); set(F_GPU_BUSY, 0.5, 1.0); set(F_CPU_UTIL, 0.3, 0.95); set(F_TOP_PROC, 0.2, 0.9);
      set(F_COMPOSITOR, 0.02, 0.15); flag(F_LAUNCHER, 0.7); flag(F_AUDIO, 0.7); set(F_ACTIVE_PROCS, 0.15, 0.5);
      break;
    case WorkloadClass::Compile:
      set(F_COMPILE_CPU, 0.2, 1.0); set(F_COMPILE_WORKERS, 0.25, 1.0); flag(F_COMPILE_ORCH, 0.8); set(F_CPU_UTIL, 0.4, 1.0);
      set(F_TOP_PROC, 0.1, 0.3); set(F_ACTIVE_PROCS, 0.2, 0.8); set(F_IO_READ, 0.0, 0.4); set(F_IO_WRITE, 0.0, 0.4);
      break;
    case WorkloadClass::Render:
      set(F_RENDER_CPU, 0.25, 1.0); set(F_CPU_UTIL, 0.5, 1.0); set(F_GPU_BUSY, 0.0, 0.9); set(F_TOP_PROC, 0.3, 1.0);
      break;
    case WorkloadClass::Encode:
      set(F_ENCODER_CPU, 0.15, 1.0); set(F_CPU_UTIL, 0.3, 1.0); set(F_TOP_PROC, 0.2, 1.0); set(F_IO_READ, 0.0, 0.5);
      break;
    case WorkloadClass::FileCopy:
      set(F_IO_READ, 0.45, 0.95); set(F_IO_WRITE, 0.45, 0.95); flag(F_COPY_TOOL, 0.6); set(F_CPU_UTIL, 0.03, 0.4);
      set(F_PSI_IO, 0.0, 0.5); set(F_TOP_PROC, 0.0, 0.2);
      break;
    case WorkloadClass::Archive:
      set(F_ARCHIVER_CPU, 0.1, 1.0); set(F_CPU_UTIL, 0.2, 0.8); set(F_IO_READ, 0.2, 0.7); set(F_IO_WRITE, 0.2, 0.7);
      set(F_TOP_PROC, 0.1, 0.5);
      break;
    case WorkloadClass::Vm:
      set(F_VM_CPU, 0.04, 1.0); set(F_CPU_UTIL, 0.2, 0.9); set(F_RAM_USED, 0.4, 0.95); set(F_TOP_PROC, 0.1, 1.0);
      break;
    case WorkloadClass::Container:
      set(F_CONTAINER_CPU, 0.03, 1.0); set(F_CPU_UTIL, 0.15, 0.9); set(F_SYSTEM, 0.0, 0.4); set(F_TOP_PROC, 0.05, 0.9);
      if (r.uniform() < 0.4) set(F_SERVER_CPU, 0.03, 0.3);  // databases/web servers usually run inside the container
      break;
    case WorkloadClass::Server:
      set(F_SERVER_CPU, 0.03, 0.4); set(F_LISTEN_PORTS, 0.15, 0.7); set(F_SYSTEM, 0.03, 0.4); set(F_COMPOSITOR, 0.0, 0.02);
      set(F_SESSION, 0.0, 0.02); set(F_CPU_UTIL, 0.03, 0.4); f[F_AUDIO] = 0.0f;
      break;
    case WorkloadClass::Background:
      set(F_SYSTEM, 0.03, 0.6); set(F_COMPOSITOR, 0.0, 0.02); set(F_SESSION, 0.0, 0.02); set(F_CPU_UTIL, 0.05, 0.5);
      set(F_TOP_PROC, 0.02, 0.4); set(F_IO_READ, 0.0, 0.3); f[F_AUDIO] = 0.0f;
      break;
  }
  Sample s;
  s.y = static_cast<int>(c);
  s.x.resize(kFeatureCount);
  for (int i = 0; i < kFeatureCount; ++i) s.x[static_cast<std::size_t>(i)] = clip(static_cast<double>(f[static_cast<std::size_t>(i)]) + 0.015 * r.normal());
  return s;
}

}  // namespace

std::vector<Sample> make_workload_dataset(int per_class, Rng& r) {
  std::vector<Sample> out;
  for (std::size_t c = 0; c < workload::kClassCount; ++c)
    for (int i = 0; i < per_class; ++i) out.push_back(archetype(static_cast<WorkloadClass>(c), r));
  return out;
}

std::vector<Sample> make_profile_dataset(int n, Rng& r) {
  std::vector<Sample> out;
  for (int i = 0; i < n; ++i) {
    ProfileFeatureInput in;
    in.cls = static_cast<WorkloadClass>(r.below(static_cast<int>(workload::kClassCount)));
    in.confidence = r.uniform() < 0.1 ? r.uniform(0.0, 0.3) : r.uniform(0.3, 1.0);
    in.flags = 0;
    if (r.uniform() < 0.25) in.flags |= workload::kHighLoad;
    if (r.uniform() < 0.2) in.flags |= workload::kLatencySensitive;
    if (r.uniform() < 0.15) in.flags |= workload::kResourceHeavy;
    const double ac = r.uniform();
    if (ac < 0.5) in.on_ac = true; else if (ac < 0.9) in.on_ac = false;
    in.battery_pct = r.uniform() < 0.3 ? r.uniform(3, 30) : r.uniform(30, 100);
    if (r.uniform() < 0.7) in.thermal_margin_c = r.uniform(0.0, 60.0);
    in.psi_cpu10 = r.uniform(0.0, 60.0);
    in.cpu_util = r.uniform(0.0, 100.0);
    in.aggressiveness = 1 + r.below(4);

    policy::RuleInput ri;  // exactly what the rule table sees
    ri.cls = in.cls;
    ri.flags = in.flags;
    ri.confidence = in.confidence;
    ri.on_ac = in.on_ac;
    ri.battery_pct = in.battery_pct;
    ri.aggressiveness = in.aggressiveness;
    ri.thermal_margin_c = in.thermal_margin_c;
    const Profile p = policy::choose_profile(ri).profile;
    Sample s;
    s.y = p == Profile::Powersave ? 0 : (p == Profile::Balanced ? 1 : 2);
    const auto pf = profile_features(in);
    s.x.assign(pf.begin(), pf.end());
    out.push_back(std::move(s));
  }
  return out;
}

namespace {
Result<void> train(Mlp& m, std::vector<Sample> data, int epochs, std::uint64_t seed, const char* kind, std::stop_token stop) {
  Rng rng(seed);
  for (std::size_t i = data.size(); i > 1; --i) std::swap(data[i - 1], data[static_cast<std::size_t>(rng.below(static_cast<int>(i)))]);
  const std::size_t cut = data.size() * 9 / 10;
  std::vector<Sample> tr(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(cut)), va(data.begin() + static_cast<std::ptrdiff_t>(cut), data.end());
  init_weights(m, rng);
  FitConfig cfg;
  cfg.epochs = epochs;
  cfg.seed = seed + 1;
  const auto r = fit(m, tr, va, cfg, stop);
  if (r.stopped) return fail(make_error(Errc::Timeout, "base model build stopped"));
  m.meta.kind = kind;
  m.meta.feature_version = kFeatureVersion;
  m.meta.trained_samples = static_cast<std::int64_t>(tr.size());
  m.meta.trained_at = static_cast<std::int64_t>(std::time(nullptr));
  m.meta.source = "base";
  m.meta.val_accuracy = r.val_accuracy;
  return {};
}
}  // namespace

Result<BaseModels> build_base_models(const BaseBuildOptions& o, std::stop_token stop) {
  BaseModels b{Mlp(kFeatureCount, o.hidden_workload, static_cast<int>(workload::kClassCount)), Mlp(kProfileFeatureCount, o.hidden_profile, 3)};
  Rng rw(o.seed), rp(o.seed + 7);
  if (auto r = train(b.workload, make_workload_dataset(o.per_class, rw), o.epochs_workload, o.seed + 100, "workload", stop); !r) return fail(r.error());
  if (auto r = train(b.profile, make_profile_dataset(o.profile_samples, rp), o.epochs_profile, o.seed + 200, "profile", stop); !r) return fail(r.error());
  return b;
}

}  // namespace opt::ml
