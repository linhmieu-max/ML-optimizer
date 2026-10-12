#include "ml/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

#include "core/atomic_file.hpp"
#include "core/sha256.hpp"
#include "core/sysfs.hpp"

namespace opt::ml {
namespace {
constexpr int kMaxIn = 256, kMaxHidden = 256, kMaxOut = 64;
constexpr std::size_t kMaxFile = 4 * 1024 * 1024;

std::string floats(const std::vector<float>& v) {
  std::string s;
  char b[32];
  for (std::size_t i = 0; i < v.size(); ++i) {
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v[i]));
    if (i) s += ' ';
    s += b;
  }
  return s;
}

Result<std::vector<float>> parse_floats(std::string_view line, std::size_t expected) {
  std::vector<float> out;
  out.reserve(expected);
  for (auto tok : sysfs::split_ws(line)) {
    const std::string t(tok);
    char* end = nullptr;
    const float v = std::strtof(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v)) return fail(make_error(Errc::Parse, "model: bad or non-finite number"));
    out.push_back(v);
  }
  if (out.size() != expected) return fail(make_error(Errc::Parse, "model: wrong number of values"));
  return out;
}
}  // namespace

Mlp::Mlp(int in, int hidden, int out)
    : w1(static_cast<std::size_t>(in) * hidden), b1(static_cast<std::size_t>(hidden)), w2(static_cast<std::size_t>(out) * hidden),
      b2(static_cast<std::size_t>(out)), in_(in), hidden_(hidden), out_(out) {}

void Mlp::forward_hidden(const float* x, std::vector<float>& h, std::vector<float>& probs) const {
  h.assign(static_cast<std::size_t>(hidden_), 0.0f);
  for (int j = 0; j < hidden_; ++j) {
    float a = b1[static_cast<std::size_t>(j)];
    const float* w = &w1[static_cast<std::size_t>(j) * in_];
    for (int i = 0; i < in_; ++i) a += w[i] * x[i];
    h[static_cast<std::size_t>(j)] = std::tanh(a);
  }
  probs.assign(static_cast<std::size_t>(out_), 0.0f);
  float mx = -1e30f;
  for (int k = 0; k < out_; ++k) {
    float a = b2[static_cast<std::size_t>(k)];
    const float* w = &w2[static_cast<std::size_t>(k) * hidden_];
    for (int j = 0; j < hidden_; ++j) a += w[j] * h[static_cast<std::size_t>(j)];
    probs[static_cast<std::size_t>(k)] = a;
    mx = std::max(mx, a);
  }
  float sum = 0;
  for (auto& p : probs) {
    p = std::exp(p - mx);
    sum += p;
  }
  for (auto& p : probs) p /= sum;
}

void Mlp::forward(const float* x, std::vector<float>& probs) const {
  std::vector<float> h;
  forward_hidden(x, h, probs);
}

int Mlp::argmax(const float* x) const {
  std::vector<float> p;
  forward(x, p);
  return static_cast<int>(std::max_element(p.begin(), p.end()) - p.begin());
}

std::string Mlp::serialize() const {
  std::ostringstream o;
  o << "OPTMODEL 1\nkind " << meta.kind << "\nfeature_version " << meta.feature_version << "\ninput " << in_ << "\nhidden " << hidden_
    << "\noutput " << out_ << "\ntrained_samples " << meta.trained_samples << "\ntrained_at " << meta.trained_at << "\nsource " << meta.source
    << "\nval_accuracy " << meta.val_accuracy << "\nw1 " << floats(w1) << "\nb1 " << floats(b1) << "\nw2 " << floats(w2) << "\nb2 " << floats(b2) << "\n";
  const std::string body = o.str();
  return body + "sha256 " + sha256_hex(body) + "\n";
}

std::string Mlp::checksum() const {
  const std::string s = serialize();
  return s.substr(s.rfind("sha256 ") + 7, 64);
}

Result<Mlp> Mlp::parse(std::string_view text) {
  if (text.size() > kMaxFile) return fail(make_error(Errc::Parse, "model: file too large"));
  const auto pos = text.rfind("sha256 ");
  if (pos == std::string_view::npos) return fail(make_error(Errc::Parse, "model: missing checksum"));
  const std::string_view body = text.substr(0, pos);
  std::string sum(text.substr(pos + 7));
  while (!sum.empty() && (sum.back() == '\n' || sum.back() == ' ')) sum.pop_back();
  if (sum != sha256_hex(body)) return fail(make_error(Errc::Parse, "model: checksum mismatch (corrupt or edited file)"));

  std::map<std::string, std::string> kv;
  for (auto line : sysfs::split_lines(body)) {
    const auto sp = line.find(' ');
    if (sp == std::string_view::npos) { if (line != "OPTMODEL") return fail(make_error(Errc::Parse, "model: malformed line")); continue; }
    kv[std::string(line.substr(0, sp))] = std::string(line.substr(sp + 1));
  }
  if (kv["OPTMODEL"] != "1") return fail(make_error(Errc::Parse, "model: unknown format/version"));
  auto num = [&](const char* k) -> std::optional<long long> {
    auto it = kv.find(k);
    return it == kv.end() ? std::nullopt : std::optional<long long>(std::atoll(it->second.c_str()));
  };
  const auto in = num("input"), hid = num("hidden"), out = num("output");
  if (!in || !hid || !out || *in < 1 || *in > kMaxIn || *hid < 1 || *hid > kMaxHidden || *out < 2 || *out > kMaxOut)
    return fail(make_error(Errc::Parse, "model: invalid dimensions"));
  Mlp m(static_cast<int>(*in), static_cast<int>(*hid), static_cast<int>(*out));
  m.meta.kind = kv["kind"];
  m.meta.feature_version = static_cast<int>(num("feature_version").value_or(0));
  m.meta.trained_samples = num("trained_samples").value_or(0);
  m.meta.trained_at = num("trained_at").value_or(0);
  m.meta.source = kv["source"];
  m.meta.val_accuracy = std::atof(kv["val_accuracy"].c_str());
  struct Part { const char* key; std::vector<float>* dst; std::size_t n; };
  for (const Part& p : {Part{"w1", &m.w1, m.w1.size()}, Part{"b1", &m.b1, m.b1.size()}, Part{"w2", &m.w2, m.w2.size()}, Part{"b2", &m.b2, m.b2.size()}}) {
    auto it = kv.find(p.key);
    if (it == kv.end()) return fail(make_error(Errc::Parse, std::string("model: missing ") + p.key));
    auto v = parse_floats(it->second, p.n);
    if (!v) return fail(v.error());
    *p.dst = std::move(*v);
  }
  return m;
}

Result<void> save_model(const std::filesystem::path& file, const Mlp& m) { return write_file_atomic(file, m.serialize(), 0640); }

Result<Mlp> load_model(const std::filesystem::path& file) {
  auto t = sysfs::read_text(file, kMaxFile + 1);
  if (!t) return fail(make_error(Errc::NotFound, "cannot read model " + file.string()));
  return Mlp::parse(*t + "\n");
}

}  // namespace opt::ml
