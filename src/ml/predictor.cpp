#include "ml/predictor.hpp"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/logging.hpp"

namespace opt::ml {
namespace fs = std::filesystem;
namespace {
constexpr const char* kWorkloadFile = "base-workload-v1.model";
constexpr const char* kProfileFile = "base-profile-v1.model";
constexpr const char* kLocalWorkloadFile = "local-workload-v1.model";
constexpr const char* kLocalProfileFile = "local-profile-v1.model";

Profile profile_from_index(int i) { return i == 0 ? Profile::Powersave : (i == 1 ? Profile::Balanced : Profile::Performance); }

std::string meta_text(const Mlp& m) {
  char b[160];
  std::snprintf(b, sizeof b, "%s model (%s, %lld samples, validation accuracy %.1f%%, %d->%d->%d, sha256 %.12s)", m.meta.kind.c_str(),
                m.meta.source.c_str(), static_cast<long long>(m.meta.trained_samples), 100.0 * m.meta.val_accuracy, m.in(), m.hidden(), m.out(),
                m.checksum().c_str());
  return b;
}
}  // namespace

void AgreementTracker::add(bool agree) {
  const double x = agree ? 1.0 : 0.0;
  ++n;
  if (n <= kWarmup) value += (x - value) / static_cast<double>(n);  // running mean
  else value += kAlpha * (x - value);                                // slow EMA afterwards
}
double AgreementTracker::factor() const { return value * std::min(1.0, static_cast<double>(n) / kWarmup); }

Predictor::Predictor(fs::path model_dir, Options options) : dir_(std::move(model_dir)), options_(options) {}

bool Predictor::ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workload_ && profile_;
}

bool Predictor::cache_ok(const fs::path& dir) {
  auto w = load_model(dir / kWorkloadFile);
  auto p = load_model(dir / kProfileFile);
  return w && p && w->meta.feature_version == kFeatureVersion && p->meta.feature_version == kFeatureVersion &&
         w->in() == kFeatureCount && p->in() == kProfileFeatureCount;
}

bool Predictor::try_load() {
  auto w = load_model(dir_ / kWorkloadFile);
  auto p = load_model(dir_ / kProfileFile);
  auto ok = [](const Result<Mlp>& m, int in, int out) {
    return m && m->meta.feature_version == kFeatureVersion && m->in() == in && m->out() == out;
  };
  if (!ok(w, kFeatureCount, static_cast<int>(workload::kClassCount)) || !ok(p, kProfileFeatureCount, 3)) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    workload_ = std::make_shared<const Mlp>(std::move(*w));
    profile_ = std::make_shared<const Mlp>(std::move(*p));
  }
  load_local_models();
  return true;
}

void Predictor::load_local_models() {
  // A valid locally adapted model replaces the base one; anything damaged or outdated is ignored.
  auto lw = load_model(dir_ / kLocalWorkloadFile);
  auto lp = load_model(dir_ / kLocalProfileFile);
  std::lock_guard<std::mutex> lock(mutex_);
  if (lw && lw->meta.source == "local" && lw->meta.feature_version == kFeatureVersion && lw->in() == kFeatureCount && lw->out() == static_cast<int>(workload::kClassCount))
    workload_ = std::make_shared<const Mlp>(std::move(*lw));
  if (lp && lp->meta.source == "local" && lp->meta.feature_version == kFeatureVersion && lp->in() == kProfileFeatureCount && lp->out() == 3)
    profile_ = std::make_shared<const Mlp>(std::move(*lp));
}

Result<void> Predictor::install_local(const Mlp& m) {
  const bool is_workload = m.meta.kind == "workload";
  if (!is_workload && m.meta.kind != "profile") return fail(make_error(Errc::InvalidArgument, "unknown model kind"));
  if (m.meta.feature_version != kFeatureVersion || m.in() != (is_workload ? kFeatureCount : kProfileFeatureCount) ||
      m.out() != (is_workload ? static_cast<int>(workload::kClassCount) : 3))
    return fail(make_error(Errc::InvalidArgument, "model does not match the current feature layout"));
  std::lock_guard<std::mutex> lock(mutex_);
  if (is_workload) { workload_ = std::make_shared<const Mlp>(m); agree_workload_ = {}; }
  else { profile_ = std::make_shared<const Mlp>(m); agree_profile_ = {}; }
  return {};
}

void Predictor::reset_local() {
  auto w = load_model(dir_ / kWorkloadFile);
  auto p = load_model(dir_ / kProfileFile);
  std::lock_guard<std::mutex> lock(mutex_);
  if (w) workload_ = std::make_shared<const Mlp>(std::move(*w));
  if (p) profile_ = std::make_shared<const Mlp>(std::move(*p));
  agree_workload_ = {};
  agree_profile_ = {};
}

std::shared_ptr<const Mlp> Predictor::model(const std::string& kind) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return kind == "workload" ? workload_ : (kind == "profile" ? profile_ : nullptr);
}

void Predictor::start() {
  if (try_load()) {
    log::info("ml", "base models loaded from cache", {{"dir", dir_.string()}});
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bootstrapping_ = true;
  }
  thread_ = std::jthread([this](std::stop_token st) { bootstrap(st); });
}

void Predictor::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
}

void Predictor::bootstrap(std::stop_token st) {
  // One-off compute: keep it out of the way of real work.
  ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
  log::info("ml", "building the base models (one time, low priority)");
  auto built = build_base_models(options_.build, st);
  std::lock_guard<std::mutex> lock(mutex_);
  bootstrapping_ = false;
  if (!built) {
    error_ = built.error().message;
    if (built.error().code != Errc::Timeout) log::warn("ml", "base model build failed", {{"error", describe(built.error())}});
    return;
  }
  if (auto r = save_model(dir_ / kWorkloadFile, built->workload); !r) log::warn("ml", "cannot cache the workload model", {{"error", describe(r.error())}});
  if (auto r = save_model(dir_ / kProfileFile, built->profile); !r) log::warn("ml", "cannot cache the profile model", {{"error", describe(r.error())}});
  workload_ = std::make_shared<const Mlp>(std::move(built->workload));
  profile_ = std::make_shared<const Mlp>(std::move(built->profile));
  log::info("ml", "base models ready", {{"workload_acc", std::to_string(workload_->meta.val_accuracy).substr(0, 5)},
                                         {"profile_acc", std::to_string(profile_->meta.val_accuracy).substr(0, 5)}});
}

Forecast Predictor::update_forecast(std::optional<double> cpu) {  // caller holds the lock
  Forecast f;
  if (!cpu) return f;
  const double x = *cpu;
  if (last_forecast_) {  // score the previous forecast against what actually happened
    sq_err_ += (x - *last_forecast_) * (x - *last_forecast_);
    ++forecast_n_;
  }
  constexpr double alpha = 0.5, beta = 0.3;
  if (!level_) {
    level_ = x;
    trend_ = 0.0;
  } else {
    const double prev = *level_;
    level_ = alpha * x + (1 - alpha) * (*level_ + *trend_);
    trend_ = beta * (*level_ - prev) + (1 - beta) * *trend_;
  }
  f.cpu_now = x;
  f.cpu_next = std::clamp(*level_ + *trend_, 0.0, 100.0);
  last_forecast_ = f.cpu_next;
  f.trend = *trend_ > 2.0 ? "rising" : (*trend_ < -2.0 ? "falling" : "flat");
  f.n = forecast_n_;
  f.rmse = forecast_n_ ? std::sqrt(sq_err_ / static_cast<double>(forecast_n_)) : 0.0;
  return f;
}

Prediction Predictor::evaluate(const FeatureVector& fv, const ProfileFeatureInput& pin, std::optional<double> cpu) {
  std::shared_ptr<const Mlp> w, p;
  Prediction out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    w = workload_;
    p = profile_;
    out.forecast = update_forecast(cpu);
  }
  if (!w || !p) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_ = out;
    return out;
  }
  std::vector<float> probs;
  w->forward(fv.data(), probs);
  const auto top = std::max_element(probs.begin(), probs.end()) - probs.begin();
  out.workload = static_cast<workload::WorkloadClass>(top);
  out.workload_prob = probs[static_cast<std::size_t>(top)];
  for (std::size_t i = 0; i < probs.size() && i < out.workload_probs.size(); ++i) out.workload_probs[i] = probs[i];

  const ProfileFeatures pf = profile_features(pin);
  std::vector<float> pp;
  p->forward(pf.data(), pp);
  const auto best = std::max_element(pp.begin(), pp.end()) - pp.begin();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    out.workload_confidence = out.workload_prob * agree_workload_.factor();
    out.advice.available = true;
    out.advice.suggested = profile_from_index(static_cast<int>(best));
    for (int i = 0; i < 3; ++i) out.advice.probs[static_cast<std::size_t>(i)] = pp[static_cast<std::size_t>(i)];
    out.advice.confidence = static_cast<double>(pp[static_cast<std::size_t>(best)]) * agree_profile_.factor();
    char note[96];
    std::snprintf(note, sizeof note, "model p=%.2f x agreement %.2f (n=%lld)", static_cast<double>(pp[static_cast<std::size_t>(best)]),
                  agree_profile_.factor(), static_cast<long long>(agree_profile_.n));
    out.advice.note = note;
    out.ready = true;
    last_ = out;
  }
  return out;
}

void Predictor::record_outcome(workload::WorkloadClass rule_class, Profile rule_profile) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!last_.ready) return;
  agree_workload_.add(last_.workload == rule_class);
  if (rule_profile != Profile::ThermalEmergency) agree_profile_.add(last_.advice.suggested == rule_profile);
}

PredictorStatus Predictor::status() const {
  PredictorStatus s;
  std::lock_guard<std::mutex> lock(mutex_);
  s.ready = workload_ && profile_;
  s.bootstrapping = bootstrapping_;
  s.error = error_;
  if (workload_) s.workload_model = meta_text(*workload_);
  if (profile_) s.profile_model = meta_text(*profile_);
  s.workload_agreement = agree_workload_;
  s.profile_agreement = agree_profile_;
  s.last = last_;
  s.forecast = last_.forecast;
  return s;
}

std::string format_prediction(const PredictorStatus& s) {
  char b[200];
  std::string out;
  if (!s.ready) {
    out += s.bootstrapping ? "Models:       building the base models (one time, low priority)...\n"
                           : "Models:       not available" + (s.error.empty() ? std::string() : " (" + s.error + ")") + "\n";
    return out;
  }
  out += "Workload model: " + s.workload_model + "\n";
  out += "Profile model:  " + s.profile_model + "\n";
  std::snprintf(b, sizeof b, "Agreement with the rules: workload %.0f%% (n=%lld), profile %.0f%% (n=%lld)  -> effective confidence = probability x agreement\n",
                100 * s.workload_agreement.value, static_cast<long long>(s.workload_agreement.n), 100 * s.profile_agreement.value,
                static_cast<long long>(s.profile_agreement.n));
  out += b;
  if (s.last.ready) {
    std::vector<std::pair<double, std::size_t>> r;
    for (std::size_t i = 0; i < s.last.workload_probs.size(); ++i) r.emplace_back(s.last.workload_probs[i], i);
    std::sort(r.rbegin(), r.rend());
    out += "Workload now:   " + std::string(to_string(s.last.workload)) + " (effective confidence ";
    std::snprintf(b, sizeof b, "%.2f)  top:", s.last.workload_confidence);
    out += b;
    for (std::size_t i = 0; i < 3; ++i) {
      std::snprintf(b, sizeof b, " %s=%.2f", std::string(to_string(static_cast<workload::WorkloadClass>(r[i].second))).c_str(), r[i].first);
      out += b;
    }
    out += "\n";
    std::snprintf(b, sizeof b, "Profile advice:  %s  (effective confidence %.2f; powersave %.2f, balanced %.2f, performance %.2f)\n",
                  std::string(to_string(s.last.advice.suggested)).c_str(), s.last.advice.confidence, s.last.advice.probs[0], s.last.advice.probs[1],
                  s.last.advice.probs[2]);
    out += b;
  }
  std::snprintf(b, sizeof b, "CPU demand forecast: now %.0f%% -> next %.0f%% (%s); forecast error RMSE %.1f points over %lld forecasts\n",
                s.forecast.cpu_now, s.forecast.cpu_next, s.forecast.trend.c_str(), s.forecast.rmse, static_cast<long long>(s.forecast.n));
  out += b;
  return out;
}

}  // namespace opt::ml
