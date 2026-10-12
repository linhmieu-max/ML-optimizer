#include "ml/fit.hpp"

#include "ml/trainer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace opt::ml {

void init_weights(Mlp& m, Rng& rng) {
  const double s1 = std::sqrt(6.0 / (m.in() + m.hidden())), s2 = std::sqrt(6.0 / (m.hidden() + m.out()));
  for (auto& w : m.w1) w = static_cast<float>(rng.uniform(-s1, s1));
  for (auto& w : m.w2) w = static_cast<float>(rng.uniform(-s2, s2));
  std::fill(m.b1.begin(), m.b1.end(), 0.0f);
  std::fill(m.b2.begin(), m.b2.end(), 0.0f);
}

double accuracy(const Mlp& m, const std::vector<Sample>& data) {
  if (data.empty()) return 0.0;
  std::size_t ok = 0;
  for (const auto& s : data)
    if (m.argmax(s.x.data()) == s.y) ++ok;
  return static_cast<double>(ok) / static_cast<double>(data.size());
}

FitResult fit(Mlp& m, const std::vector<Sample>& train, const std::vector<Sample>& val, const FitConfig& cfg, std::stop_token stop,
              const std::function<bool()>& between_batches) {
  FitResult res;
  Trainer t(m, train, val, cfg);
  const auto status = t.run([&] { return !stop.stop_requested() && (!between_batches || between_batches()); });
  m = t.model();
  res.epochs_run = t.epoch();
  res.train_loss = t.train_loss();
  if (status == Trainer::Status::Paused) {
    res.stopped = true;
    return res;
  }
  res.val_accuracy = accuracy(m, val);
  return res;
}

}  // namespace opt::ml
