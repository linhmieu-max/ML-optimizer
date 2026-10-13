#pragma once
// Turns recorded samples into training sets. Local labels are outcome-aware and conservative:
//   workload: the rule classifier's class when it was confident (>= 0.5)
//   profile : only AUTO decisions whose profile was really applied by the controllers;
//             success + reward >= 0.15 -> the applied profile is reinforced;
//             failure (thermal HOT+ or a failed change) -> one step LOWER; anything else is skipped.
// Every set also contains a deterministic replay of the base data so the model does not forget the prior.

#include <optional>
#include <string>
#include <vector>

#include "database/telemetry.hpp"
#include "ml/fit.hpp"

namespace opt::ml {

std::string quantize(const float* v, int n);                       // one byte per value
std::vector<float> dequantize(const std::string& blob, int n);      // empty if the size is wrong

std::optional<int> workload_label(const db::MlSampleRow&);
std::optional<int> profile_label(const db::MlSampleRow&);
bool is_validation_sample(std::int64_t ts);                          // deterministic ~10 % hold-out

struct TrainingSets {
  std::vector<Sample> train;  // replay + local
  std::vector<Sample> val;    // local hold-out only
  std::int64_t local_train = 0, local_val = 0;
};

TrainingSets build_workload_sets(const std::vector<db::MlSampleRow>&, int replay_per_class, std::uint64_t seed);
TrainingSets build_profile_sets(const std::vector<db::MlSampleRow>&, int replay_samples, int replicate, std::uint64_t seed);

}  // namespace opt::ml
