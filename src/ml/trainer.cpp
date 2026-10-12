#include "ml/trainer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <numeric>
#include <sstream>

#include "core/sha256.hpp"
#include "core/sysfs.hpp"

namespace opt::ml {
namespace {

std::string join_floats(const std::vector<float>& v) {
  std::string s;
  char b[32];
  for (std::size_t i = 0; i < v.size(); ++i) {
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v[i]));
    if (i) s += ' ';
    s += b;
  }
  return s;
}

bool parse_floats(std::string_view line, std::size_t n, std::vector<float>& out) {
  out.clear();
  out.reserve(n);
  for (auto tok : sysfs::split_ws(line)) {
    const std::string t(tok);
    char* end = nullptr;
    const float v = std::strtof(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out.push_back(v);
  }
  return out.size() == n;
}

}  // namespace

Trainer::Trainer(Mlp model, std::vector<Sample> train, std::vector<Sample> val, FitConfig cfg)
    : m_(std::move(model)), train_(std::move(train)), val_(std::move(val)), cfg_(cfg), rng_(cfg.seed), order_(train_.size()) {
  std::iota(order_.begin(), order_.end(), 0u);
  a1_ = {std::vector<float>(m_.w1.size(), 0.0f), std::vector<float>(m_.w1.size(), 0.0f)};
  ab1_ = {std::vector<float>(m_.b1.size(), 0.0f), std::vector<float>(m_.b1.size(), 0.0f)};
  a2_ = {std::vector<float>(m_.w2.size(), 0.0f), std::vector<float>(m_.w2.size(), 0.0f)};
  ab2_ = {std::vector<float>(m_.b2.size(), 0.0f), std::vector<float>(m_.b2.size(), 0.0f)};
}

double Trainer::progress() const {
  if (cfg_.epochs <= 0 || train_.empty()) return 1.0;
  return std::min(1.0, (epoch_ + static_cast<double>(pos_) / static_cast<double>(train_.size())) / cfg_.epochs);
}

void Trainer::shuffle() {
  for (std::size_t i = order_.size(); i > 1; --i) std::swap(order_[i - 1], order_[static_cast<std::size_t>(rng_.below(static_cast<int>(i)))]);
}

void Trainer::adam(std::vector<float>& w, const std::vector<float>& g, Moments& mo) {
  constexpr float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
  const float c1 = 1.0f - std::pow(b1, static_cast<float>(t_)), c2 = 1.0f - std::pow(b2, static_cast<float>(t_));
  for (std::size_t i = 0; i < w.size(); ++i) {
    mo.m[i] = b1 * mo.m[i] + (1 - b1) * g[i];
    mo.v[i] = b2 * mo.v[i] + (1 - b2) * g[i] * g[i];
    w[i] -= cfg_.lr * (mo.m[i] / c1) / (std::sqrt(mo.v[i] / c2) + eps);
  }
}

Trainer::Status Trainer::run(const std::function<bool()>& keep_going, long max_batches) {
  if (train_.empty()) {
    epoch_ = cfg_.epochs;
    return Status::Finished;
  }
  std::vector<float> g1(m_.w1.size()), gb1(m_.b1.size()), g2(m_.w2.size()), gb2(m_.b2.size()), h, p, dh(static_cast<std::size_t>(m_.hidden()));
  long budget = max_batches;
  while (!finished()) {
    if (!epoch_started_) {
      shuffle();
      epoch_started_ = true;
      loss_acc_ = 0;
    }
    while (pos_ < order_.size()) {
      if (budget-- <= 0 || (keep_going && !keep_going())) return Status::Paused;
      std::fill(g1.begin(), g1.end(), 0.0f); std::fill(gb1.begin(), gb1.end(), 0.0f);
      std::fill(g2.begin(), g2.end(), 0.0f); std::fill(gb2.begin(), gb2.end(), 0.0f);
      const std::size_t end = std::min(order_.size(), pos_ + static_cast<std::size_t>(cfg_.batch));
      for (std::size_t n = pos_; n < end; ++n) {
        const Sample& s = train_[order_[n]];
        m_.forward_hidden(s.x.data(), h, p);
        loss_acc_ -= std::log(std::max(1e-9f, p[static_cast<std::size_t>(s.y)]));
        p[static_cast<std::size_t>(s.y)] -= 1.0f;
        std::fill(dh.begin(), dh.end(), 0.0f);
        for (int k = 0; k < m_.out(); ++k) {
          const float d = p[static_cast<std::size_t>(k)];
          gb2[static_cast<std::size_t>(k)] += d;
          for (int j = 0; j < m_.hidden(); ++j) {
            g2[static_cast<std::size_t>(k) * m_.hidden() + j] += d * h[static_cast<std::size_t>(j)];
            dh[static_cast<std::size_t>(j)] += d * m_.w2[static_cast<std::size_t>(k) * m_.hidden() + j];
          }
        }
        for (int j = 0; j < m_.hidden(); ++j) {
          const float d = dh[static_cast<std::size_t>(j)] * (1.0f - h[static_cast<std::size_t>(j)] * h[static_cast<std::size_t>(j)]);
          gb1[static_cast<std::size_t>(j)] += d;
          for (int i = 0; i < m_.in(); ++i) g1[static_cast<std::size_t>(j) * m_.in() + i] += d * s.x[static_cast<std::size_t>(i)];
        }
      }
      const float inv = 1.0f / static_cast<float>(end - pos_);
      for (std::size_t i = 0; i < g1.size(); ++i) g1[i] = g1[i] * inv + cfg_.l2 * m_.w1[i];
      for (std::size_t i = 0; i < g2.size(); ++i) g2[i] = g2[i] * inv + cfg_.l2 * m_.w2[i];
      for (auto& g : gb1) g *= inv;
      for (auto& g : gb2) g *= inv;
      ++t_;
      adam(m_.w1, g1, a1_); adam(m_.b1, gb1, ab1_); adam(m_.w2, g2, a2_); adam(m_.b2, gb2, ab2_);
      pos_ = end;
    }
    last_loss_ = loss_acc_ / static_cast<double>(train_.size());
    pos_ = 0;
    epoch_started_ = false;
    ++epoch_;
  }
  return Status::Finished;
}

std::string dataset_fingerprint(const std::string& kind, const std::vector<Sample>& train, const std::vector<Sample>& val) {
  std::string buf = kind + "|" + std::to_string(train.size()) + "|" + std::to_string(val.size()) + "|";
  auto add = [&](const std::vector<Sample>& v) {
    for (const auto& s : v) {
      buf += std::to_string(s.y);
      buf.append(reinterpret_cast<const char*>(s.x.data()), s.x.size() * sizeof(float));
    }
  };
  add(train);
  add(val);
  return sha256_hex(buf);
}

std::string Trainer::checkpoint(const std::string& kind, const std::string& fingerprint) const {
  std::ostringstream o;
  char b[48];
  o << "OPTCKPT 1\nkind " << kind << "\nfingerprint " << fingerprint << "\nconfig " << cfg_.epochs << " " << cfg_.batch << " " << cfg_.seed << "\n";
  std::snprintf(b, sizeof b, "%.9g %.9g", static_cast<double>(cfg_.lr), static_cast<double>(cfg_.l2));
  o << "rates " << b << "\nprogress " << epoch_ << " " << pos_ << " " << t_ << " " << (epoch_started_ ? 1 : 0) << "\nrng " << rng_.state() << "\n";
  std::snprintf(b, sizeof b, "%.17g %.17g", loss_acc_, last_loss_);
  o << "loss " << b << "\norder";
  for (auto v : order_) o << ' ' << v;
  o << "\nadam_w1_m " << join_floats(a1_.m) << "\nadam_w1_v " << join_floats(a1_.v) << "\nadam_b1_m " << join_floats(ab1_.m) << "\nadam_b1_v " << join_floats(ab1_.v)
    << "\nadam_w2_m " << join_floats(a2_.m) << "\nadam_w2_v " << join_floats(a2_.v) << "\nadam_b2_m " << join_floats(ab2_.m) << "\nadam_b2_v " << join_floats(ab2_.v)
    << "\nmodel_begin\n" << m_.serialize() << "model_end\n";
  const std::string body = o.str();
  return body + "sha256 " + sha256_hex(body) + "\n";
}

Result<Trainer> Trainer::restore(std::string_view text, std::vector<Sample> train, std::vector<Sample> val, const FitConfig& cfg,
                                 const std::string& kind, const std::string& fingerprint) {
  auto bad = [](const char* why) { return fail(make_error(Errc::Parse, std::string("checkpoint: ") + why)); };
  const auto pos = text.rfind("sha256 ");
  if (pos == std::string_view::npos) return bad("missing checksum");
  const std::string_view body = text.substr(0, pos);
  std::string sum(text.substr(pos + 7));
  while (!sum.empty() && (sum.back() == '\n' || sum.back() == ' ')) sum.pop_back();
  if (sum != sha256_hex(body)) return bad("checksum mismatch");

  const auto mb = body.find("model_begin\n"), me = body.rfind("model_end\n");
  if (mb == std::string_view::npos || me == std::string_view::npos || me < mb) return bad("model block missing");
  auto model = Mlp::parse(body.substr(mb + 12, me - (mb + 12)));
  if (!model) return bad("embedded model invalid");

  std::map<std::string, std::string> kv;
  for (auto line : sysfs::split_lines(body.substr(0, mb))) {
    const auto sp = line.find(' ');
    kv[std::string(line.substr(0, sp))] = sp == std::string_view::npos ? "" : std::string(line.substr(sp + 1));
  }
  if (kv["OPTCKPT"] != "1") return bad("unknown version");
  if (kv["kind"] != kind || kv["fingerprint"] != fingerprint) return bad("made for other data (fingerprint/kind differs)");

  Trainer t(std::move(*model), std::move(train), std::move(val), cfg);
  int epochs = 0, batch = 0;
  unsigned long long seed = 0;
  if (std::sscanf(kv["config"].c_str(), "%d %d %llu", &epochs, &batch, &seed) != 3 || epochs != cfg.epochs || batch != cfg.batch || seed != cfg.seed)
    return bad("made for another training configuration");
  int ep = 0, started = 0, step = 0;
  unsigned long long position = 0;
  if (std::sscanf(kv["progress"].c_str(), "%d %llu %d %d", &ep, &position, &step, &started) != 4 || ep < 0 || ep > cfg.epochs || step < 0 ||
      position > t.train_.size())
    return bad("invalid progress");
  t.epoch_ = ep;
  t.pos_ = static_cast<std::size_t>(position);
  t.t_ = step;
  t.epoch_started_ = started != 0;
  t.rng_.set_state(std::strtoull(kv["rng"].c_str(), nullptr, 10));
  if (std::sscanf(kv["loss"].c_str(), "%lf %lf", &t.loss_acc_, &t.last_loss_) != 2) return bad("invalid loss");

  std::vector<std::uint32_t> order;
  for (auto tok : sysfs::split_ws(kv["order"])) {
    auto v = sysfs::parse_u64(tok);
    if (!v || *v >= t.train_.size()) return bad("invalid order");
    order.push_back(static_cast<std::uint32_t>(*v));
  }
  if (order.size() != t.train_.size()) return bad("order size differs from the dataset");
  std::vector<std::uint32_t> check = order;
  std::sort(check.begin(), check.end());
  for (std::size_t i = 0; i < check.size(); ++i)
    if (check[i] != i) return bad("order is not a permutation");
  t.order_ = std::move(order);

  struct Part { const char* key; std::vector<float>* dst; std::size_t n; };
  const Part parts[] = {{"adam_w1_m", &t.a1_.m, t.m_.w1.size()}, {"adam_w1_v", &t.a1_.v, t.m_.w1.size()}, {"adam_b1_m", &t.ab1_.m, t.m_.b1.size()},
                        {"adam_b1_v", &t.ab1_.v, t.m_.b1.size()}, {"adam_w2_m", &t.a2_.m, t.m_.w2.size()}, {"adam_w2_v", &t.a2_.v, t.m_.w2.size()},
                        {"adam_b2_m", &t.ab2_.m, t.m_.b2.size()}, {"adam_b2_v", &t.ab2_.v, t.m_.b2.size()}};
  for (const auto& p : parts)
    if (!parse_floats(kv[p.key], p.n, *p.dst)) return bad("invalid optimizer state");
  return t;
}

}  // namespace opt::ml
