#pragma once
// ML inference layer (optional). Produces: a workload prediction, a profile suggestion and a short-term CPU
// demand forecast. It NEVER touches the system: output is advice that the policy engine validates.
//
// Confidence is deliberately pessimistic: effective = model probability x measured AGREEMENT with the rule
// engine (running mean over the first 50 observations, then a slow EMA, scaled by min(1, n/50)). A new
// installation therefore starts with near-zero effective confidence and the rules keep priority.

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "ml/advice.hpp"
#include "ml/base_model.hpp"
#include "ml/feature_vector.hpp"

namespace opt::ml {

using Clock = std::chrono::steady_clock;

struct AgreementTracker {
  std::int64_t n = 0;
  double value = 0;  // mean over the first kWarmup observations, then EMA
  void add(bool agree);
  double factor() const;  // value * min(1, n / kWarmup)
  static constexpr int kWarmup = 50;
  static constexpr double kAlpha = 0.02;
};

struct Forecast {
  double cpu_now = 0, cpu_next = 0;  // percent
  std::string trend = "flat";        // rising | falling | flat
  double rmse = 0;                   // running error of the previous forecasts
  std::int64_t n = 0;
};

struct Prediction {
  bool ready = false;
  workload::WorkloadClass workload = workload::WorkloadClass::Idle;
  double workload_prob = 0, workload_confidence = 0;       // raw / effective
  std::array<double, workload::kClassCount> workload_probs{};
  MlAdvice advice;
  Forecast forecast;
};

struct PredictorStatus {
  bool ready = false, bootstrapping = false;
  std::string error;
  std::string workload_model, profile_model;               // human-readable meta
  AgreementTracker workload_agreement, profile_agreement;
  Forecast forecast;
  Prediction last;
};

class Predictor {
 public:
  struct Options {
    BaseBuildOptions build;
  };
  Predictor(std::filesystem::path model_dir, Options options = {});
  ~Predictor() { stop(); }
  Predictor(const Predictor&) = delete;
  Predictor& operator=(const Predictor&) = delete;

  /// Loads cached base models (rejecting wrong feature version/corrupt files); if absent, builds them in a
  /// low-priority background thread and caches them. Returns immediately.
  void start();
  /// True when valid, current-version base models are cached in `dir` (no thread, no build).
  static bool cache_ok(const std::filesystem::path& dir);
  void stop();
  bool ready() const;
  /// Hot-swaps in a locally adapted model (kind from meta). Dimensions/feature version are validated;
  /// the agreement tracker of that model restarts so confidence is earned again.
  Result<void> install_local(const Mlp&);
  /// Back to the shipped base models (local files are deleted by the caller).
  void reset_local();
  std::shared_ptr<const Mlp> model(const std::string& kind) const;
  const std::filesystem::path& dir() const { return dir_; }

  Prediction evaluate(const FeatureVector&, const ProfileFeatureInput&, std::optional<double> cpu_util_pct);
  /// What the rule engine finally decided; updates the agreement trackers against the last prediction.
  void record_outcome(workload::WorkloadClass rule_class, Profile rule_profile);
  PredictorStatus status() const;

 private:
  void bootstrap(std::stop_token);
  bool try_load();
  Forecast update_forecast(std::optional<double> cpu_util);
  void load_local_models();

  std::filesystem::path dir_;
  Options options_;
  mutable std::mutex mutex_;
  std::shared_ptr<const Mlp> workload_, profile_;
  bool bootstrapping_ = false;
  std::string error_;
  AgreementTracker agree_workload_, agree_profile_;
  Prediction last_;
  // Holt linear smoothing of CPU utilisation
  std::optional<double> level_, trend_, last_forecast_;
  double sq_err_ = 0;
  std::int64_t forecast_n_ = 0;
  std::jthread thread_;
};

std::string format_prediction(const PredictorStatus&);

}  // namespace opt::ml
