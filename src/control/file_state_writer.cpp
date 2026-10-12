#include "control/file_state_writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include "core/sysfs.hpp"
#include "safety/safety_gate.hpp"

namespace opt::control {
namespace fs = std::filesystem;

std::string normalize_value(const std::string& target, const std::string& text) {
  if (target.ends_with("/io.weight")) {
    const auto nl = text.find('\n');
    return nl == std::string::npos ? text : text.substr(0, nl);
  }
  const auto a = text.find('['), b = text.find(']');
  if (a != std::string::npos && b != std::string::npos && b > a) return text.substr(a + 1, b - a - 1);
  return text;
}

Result<fs::path> FileStateWriter::resolve(const std::string& target) const {
  if (!safety::is_allowed_target(target)) return fail(make_error(Errc::PermissionDenied, "target not on the writable allowlist: " + target));
  std::error_code ec;
  const fs::path rootc = fs::weakly_canonical(root_, ec);
  const fs::path real = fs::canonical(root_ / target.substr(1), ec);
  if (ec) return fail(make_error(Errc::NotFound, "no such file: " + target, ec.value()));
  // After resolving symlinks the file must still live under /sys or /proc/sys.
  auto inside = [&](const fs::path& base) {
    const auto rel = real.lexically_relative(base);
    return !rel.empty() && *rel.begin() != "..";
  };
  if (!inside(rootc / "sys") && !inside(rootc / "proc/sys"))
    return fail(make_error(Errc::PermissionDenied, "resolved path escapes /sys and /proc/sys: " + real.string()));
  if (!fs::is_regular_file(real, ec)) return fail(make_error(Errc::InvalidArgument, "not a regular file: " + target));
  return real;
}

Result<std::string> FileStateWriter::read(const std::string& target) {
  auto real = resolve(target);
  if (!real) return fail(real.error());
  auto text = sysfs::read_text(*real, 4096);
  if (!text) return fail(errno_error(Errc::Io, "cannot read " + target));
  return normalize_value(target, *text);
}

Result<void> FileStateWriter::write(const std::string& target, const std::string& value) {
  if (value.empty() || value.size() > 128 || value.find('\n') != std::string::npos || value.find('\0') != std::string::npos)
    return fail(make_error(Errc::InvalidArgument, "value must be one short line"));
  auto real = resolve(target);
  if (!real) return fail(real.error());
  const int fd = ::open(real->c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);  // never O_CREAT (O_TRUNC is ignored by sysfs, needed for regular test files)
  if (fd < 0) return fail(errno_error(Errc::Io, "cannot open " + target + " for writing"));
  const std::string line = value + "\n";
  const ssize_t w = ::write(fd, line.data(), line.size());  // one write(): sysfs parses per call
  const int saved = errno;
  ::close(fd);
  if (w != static_cast<ssize_t>(line.size())) {
    errno = saved;
    return fail(errno_error(Errc::Io, "write to " + target + " rejected by the kernel"));
  }
  return {};
}

}  // namespace opt::control
