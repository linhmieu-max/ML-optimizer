#pragma once
// Small deterministic PRNG (xorshift64*): identical sequences on every platform/compiler,
// so the base model is reproducible bit for bit.
#include <cmath>
#include <cstdint>

namespace opt::ml {

class Rng {
 public:
  explicit Rng(std::uint64_t seed = 1) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return s_ * 2685821657736338717ull;
  }
  std::uint64_t state() const { return s_; }
  void set_state(std::uint64_t s) { s_ = s ? s : 0x9E3779B97F4A7C15ull; }
  double uniform() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }          // [0,1)
  double uniform(double a, double b) { return a + (b - a) * uniform(); }
  int below(int n) { return n <= 1 ? 0 : static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
  double normal() {  // Box-Muller
    const double u1 = std::max(uniform(), 1e-12), u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }

 private:
  std::uint64_t s_;
};

}  // namespace opt::ml
