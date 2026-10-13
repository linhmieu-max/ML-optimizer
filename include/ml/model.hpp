#pragma once
// One-hidden-layer MLP classifier (tanh hidden, softmax output) with a self-checking text format.
// Small on purpose: inference is a few thousand multiply-adds, no dependencies.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"

namespace opt::ml {

struct ModelMeta {
  std::string kind;                 // "workload" | "profile"
  int feature_version = 0;
  std::int64_t trained_samples = 0;
  std::int64_t trained_at = 0;      // unix seconds
  std::string source = "base";      // "base" (distilled prior) | "local" (adapted on this machine, Phase 9)
  double val_accuracy = 0;
};

class Mlp {
 public:
  Mlp() = default;
  Mlp(int in, int hidden, int out);

  int in() const { return in_; }
  int hidden() const { return hidden_; }
  int out() const { return out_; }
  /// Softmax probabilities for one input (x has in() values).
  void forward(const float* x, std::vector<float>& probs) const;
  void forward_hidden(const float* x, std::vector<float>& h, std::vector<float>& probs) const;
  int argmax(const float* x) const;

  std::vector<float> w1, b1, w2, b2;  // w1[hidden][in], w2[out][hidden]
  ModelMeta meta;

  std::string serialize() const;                         // ends with a sha256 line over the body
  static Result<Mlp> parse(std::string_view text);       // strict: size limits, finite values, checksum
  std::string checksum() const;                          // sha256 of the serialized body

 private:
  int in_ = 0, hidden_ = 0, out_ = 0;
};

Result<void> save_model(const std::filesystem::path&, const Mlp&);  // atomic
Result<Mlp> load_model(const std::filesystem::path&);

}  // namespace opt::ml
