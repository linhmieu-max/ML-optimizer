#include "core/sysfs.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace opt::sysfs {
namespace fs = std::filesystem;
namespace {

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool natural_less(const std::string& a, const std::string& b) {
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (is_digit(a[i]) && is_digit(b[j])) {
      std::size_t i2 = i, j2 = j;
      while (i2 < a.size() && is_digit(a[i2])) ++i2;
      while (j2 < b.size() && is_digit(b[j2])) ++j2;
      std::string_view na(a.data() + i, i2 - i), nb(b.data() + j, j2 - j);
      while (na.size() > 1 && na.front() == '0') na.remove_prefix(1);
      while (nb.size() > 1 && nb.front() == '0') nb.remove_prefix(1);
      if (na.size() != nb.size()) return na.size() < nb.size();
      if (na != nb) return na < nb;
      i = i2;
      j = j2;
    } else {
      if (a[i] != b[j]) return a[i] < b[j];
      ++i;
      ++j;
    }
  }
  return (a.size() - i) < (b.size() - j);
}

}  // namespace

std::optional<std::string> read_text(const fs::path& p, std::size_t max) {
  const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string s;
  char buf[4096];
  while (s.size() < max) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return std::nullopt;
    }
    if (n == 0) break;
    s.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

std::optional<long long> read_int(const fs::path& p) {
  auto t = read_text(p, 64);
  if (!t) return std::nullopt;
  long long v = 0;
  auto [ptr, ec] = std::from_chars(t->data(), t->data() + t->size(), v);
  if (ec != std::errc{} || ptr != t->data() + t->size()) return std::nullopt;
  return v;
}

std::optional<std::uint64_t> parse_u64(std::string_view s) {
  std::uint64_t v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

std::optional<double> parse_double(std::string_view s) {
  if (s.empty()) return std::nullopt;
  const std::string t(s);
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);  // daemon never calls setlocale: "C" locale
  if (end == t.c_str() || *end != '\0') return std::nullopt;
  return v;
}

std::optional<double> read_double(const fs::path& p) {
  auto t = read_text(p, 64);
  return t ? parse_double(*t) : std::nullopt;
}

bool path_exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

bool all_digits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), is_digit);
}

std::vector<fs::path> list_dir(const fs::path& dir, std::string_view prefix) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename().string().starts_with(prefix)) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end(),
            [](const fs::path& a, const fs::path& b) { return natural_less(a.filename().string(), b.filename().string()); });
  return out;
}

std::vector<std::string_view> split_ws(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

std::vector<std::string_view> split_lines(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    auto nl = s.find('\n', start);
    if (nl == std::string_view::npos) nl = s.size();
    if (nl > start) out.push_back(s.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

}  // namespace opt::sysfs
