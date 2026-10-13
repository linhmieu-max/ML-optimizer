#pragma once
// Resumable trainer. All state needed to continue EXACTLY where it stopped (weights, Adam moments, step
// counter, RNG, shuffled order, position inside the epoch) can be written to a checkpoint and restored, so
// pausing for a real workload never loses work and never changes the result: an interrupted-and-resumed run
// ends with the same weights as an uninterrupted one.

#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "ml/fit.hpp"

namespace opt::ml {

class Trainer {
 public:
  enum class Status { Finished, Paused };

  Trainer(Mlp model, std::vector<Sample> train, std::vector<Sample> val, FitConfig cfg);

  /// Trains batch by batch while `keep_going()` returns true (checked before every batch), for at most
  /// `max_batches`. Returns Paused when interrupted or out of budget, Finished when all epochs are done.
  Status run(const std::function<bool()>& keep_going, long max_batches = std::numeric_limits<long>::max());

  bool finished() const { return epoch_ >= cfg_.epochs; }
  double progress() const;            // 0..1
  int epoch() const { return epoch_; }
  int epochs() const { return cfg_.epochs; }
  double train_loss() const { return last_loss_; }
  const Mlp& model() const { return m_; }
  Mlp& model() { return m_; }
  double val_accuracy() const { return accuracy(m_, val_); }

  std::string checkpoint(const std::string& kind, const std::string& fingerprint) const;
  /// Fails (and the caller must start over) if the text is damaged or was made for other data/config.
  static Result<Trainer> restore(std::string_view text, std::vector<Sample> train, std::vector<Sample> val, const FitConfig& cfg,
                                 const std::string& kind, const std::string& fingerprint);

 private:
  struct Moments {
    std::vector<float> m, v;
  };
  void shuffle();
  void adam(std::vector<float>& w, const std::vector<float>& g, Moments& mo);

  Mlp m_;
  std::vector<Sample> train_, val_;
  FitConfig cfg_;
  Rng rng_;
  std::vector<std::uint32_t> order_;
  Moments a1_, ab1_, a2_, ab2_;
  int epoch_ = 0;
  std::size_t pos_ = 0;
  bool epoch_started_ = false;
  int t_ = 0;
  double loss_acc_ = 0, last_loss_ = 0;
};

/// Stable identity of a dataset (labels and features), used to refuse a checkpoint made for other data.
std::string dataset_fingerprint(const std::string& kind, const std::vector<Sample>& train, const std::vector<Sample>& val);

}  // namespace opt::ml
