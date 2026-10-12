#include "ml/dataset.hpp"

#include <algorithm>
#include <cmath>

#include "ml/base_model.hpp"
#include "ml/feature_vector.hpp"

namespace opt::ml {

std::string quantize(const float* v, int n) {
  std::string s(static_cast<std::size_t>(n), '\0');
  for (int i = 0; i < n; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(v[i], 0.0f, 1.0f) * 255.0f)));
  return s;
}

std::vector<float> dequantize(const std::string& blob, int n) {
  if (static_cast<int>(blob.size()) != n) return {};
  std::vector<float> out(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) out[static_cast<std::size_t>(i)] = static_cast<float>(static_cast<unsigned char>(blob[static_cast<std::size_t>(i)])) / 255.0f;
  return out;
}

std::optional<int> workload_label(const db::MlSampleRow& r) {
  if (r.wl_confidence < 0.5 || r.workload < 0 || r.workload >= static_cast<int>(workload::kClassCount)) return std::nullopt;
  return r.workload;
}

std::optional<int> profile_label(const db::MlSampleRow& r) {
  if (!r.auto_source || !r.controllers_apply || r.rule_profile != r.applied_profile || r.applied_profile < 0 || r.applied_profile > 2) return std::nullopt;
  if (!r.success) return std::max(0, r.applied_profile - 1);
  if (r.reward && *r.reward >= 0.15) return r.applied_profile;
  return std::nullopt;
}

bool is_validation_sample(std::int64_t ts) { return (((static_cast<std::uint64_t>(ts) * 2654435761ull) >> 8) % 10) == 0; }

TrainingSets build_workload_sets(const std::vector<db::MlSampleRow>& rows, int replay_per_class, std::uint64_t seed) {
  TrainingSets s;
  Rng rng(seed);
  s.train = make_workload_dataset(replay_per_class, rng);
  for (const auto& r : rows) {
    const auto label = workload_label(r);
    auto x = dequantize(r.features, kFeatureCount);
    if (!label || x.empty()) continue;
    Sample smp{std::move(x), *label};
    if (is_validation_sample(r.ts)) { s.val.push_back(std::move(smp)); ++s.local_val; }
    else { s.train.push_back(std::move(smp)); ++s.local_train; }
  }
  return s;
}

TrainingSets build_profile_sets(const std::vector<db::MlSampleRow>& rows, int replay_samples, int replicate, std::uint64_t seed) {
  TrainingSets s;
  Rng rng(seed);
  s.train = make_profile_dataset(replay_samples, rng);
  for (const auto& r : rows) {
    const auto label = profile_label(r);
    auto x = dequantize(r.pfeatures, kProfileFeatureCount);
    if (!label || x.empty()) continue;
    if (is_validation_sample(r.ts)) {
      s.val.push_back(Sample{std::move(x), *label});
      ++s.local_val;
    } else {
      for (int k = 0; k < std::max(1, replicate); ++k) s.train.push_back(Sample{x, *label});  // local evidence outweighs the replay
      ++s.local_train;
    }
  }
  return s;
}

}  // namespace opt::ml
