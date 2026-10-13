#pragma once
// The BASE model is a distilled prior, not a model trained on user data:
//   workload model: fitted on synthetic archetypes of each workload class;
//   profile  model: fitted on the outputs of the rule table (choose_profile) over random situations.
// So at first it mostly reproduces the rules (safe by construction). Local adaptation (Phase 9) is what
// can make it differ, and only when measured rewards justify it.

#include <stop_token>
#include <vector>

#include "ml/fit.hpp"
#include "ml/model.hpp"

namespace opt::ml {

struct BaseBuildOptions {
  int per_class = 500;          // synthetic samples per workload class
  int profile_samples = 12000;  // rule-table samples
  int epochs_workload = 35;
  int epochs_profile = 45;
  int hidden_workload = 24;
  int hidden_profile = 40;
  std::uint64_t seed = 20240601;
};

struct BaseModels {
  Mlp workload;  // 14 outputs, indexed by WorkloadClass
  Mlp profile;   // 3 outputs: powersave, balanced, performance
};

std::vector<Sample> make_workload_dataset(int per_class, Rng&);
std::vector<Sample> make_profile_dataset(int n, Rng&);

/// Deterministic for a given option set. Honors the stop token (returns Errc::Timeout when stopped).
Result<BaseModels> build_base_models(const BaseBuildOptions& = {}, std::stop_token = {});

}  // namespace opt::ml
