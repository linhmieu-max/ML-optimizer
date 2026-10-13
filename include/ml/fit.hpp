#pragma once
// Mini-batch Adam with softmax cross-entropy. Deterministic given the seed. Used for the base
// model now and for local adaptation later; always cooperative (stop_token) and bounded.

#include <functional>
#include <stop_token>
#include <vector>

#include "ml/model.hpp"
#include "ml/rng.hpp"

namespace opt::ml {

struct Sample {
  std::vector<float> x;
  int y = 0;
};

struct FitConfig {
  int epochs = 40;
  int batch = 32;
  float lr = 0.01f;
  float l2 = 1e-5f;
  std::uint64_t seed = 1;
};

struct FitResult {
  double train_loss = 0;
  double val_accuracy = 0;
  int epochs_run = 0;
  bool stopped = false;
};

void init_weights(Mlp&, Rng&);  // Xavier-style

/// `between_batches` (optional) is called regularly; returning false stops training early
/// (used by later phases to yield to real workloads).
FitResult fit(Mlp&, const std::vector<Sample>& train, const std::vector<Sample>& val, const FitConfig&, std::stop_token = {},
              const std::function<bool()>& between_batches = nullptr);

double accuracy(const Mlp&, const std::vector<Sample>&);

}  // namespace opt::ml
