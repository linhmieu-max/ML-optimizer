#include "monitor/psi_monitor.hpp"

#include "core/sysfs.hpp"

namespace opt::monitor {
using namespace sysfs;
namespace {

std::optional<PsiLine> parse_line(std::string_view line, std::string_view kind) {
  auto f = split_ws(line);
  if (f.size() < 5 || f[0] != kind) return std::nullopt;
  PsiLine p;
  for (std::size_t i = 1; i < f.size(); ++i) {
    const auto eq = f[i].find('=');
    if (eq == std::string_view::npos) continue;
    const auto k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
    if (k == "avg10") p.avg10 = parse_double(v).value_or(0);
    else if (k == "avg60") p.avg60 = parse_double(v).value_or(0);
    else if (k == "avg300") p.avg300 = parse_double(v).value_or(0);
    else if (k == "total") p.total_us = parse_u64(v).value_or(0);
  }
  return p;
}

std::optional<PsiResource> read_resource(const std::filesystem::path& p) {
  auto t = read_text(p);
  if (!t) return std::nullopt;
  PsiResource r;
  for (auto line : split_lines(*t)) {
    if (auto s = parse_line(line, "some")) r.some = s;
    if (auto fl = parse_line(line, "full")) r.full = fl;
  }
  if (!r.some && !r.full) return std::nullopt;
  return r;
}

}  // namespace

Result<PsiSample> PsiMonitor::sample() const {
  PsiSample s;
  s.cpu = read_resource(root_ / "proc/pressure/cpu");
  s.memory = read_resource(root_ / "proc/pressure/memory");
  s.io = read_resource(root_ / "proc/pressure/io");
  if (!s.cpu && !s.memory && !s.io) return fail(make_error(Errc::Unsupported, "PSI not available"));
  return s;
}

}  // namespace opt::monitor
