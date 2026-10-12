#include "core/atomic_file.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace opt {
namespace fs = std::filesystem;

Result<void> write_file_atomic(const fs::path& file, std::string_view content, mode_t mode) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = file.string() + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) return fail(errno_error(Errc::Io, "cannot create " + tmp.string()));
  auto abort_with = [&](const std::string& what) {
    Error e = errno_error(Errc::Io, what + ": " + tmp.string());
    ::close(fd);
    ::unlink(tmp.c_str());
    return fail(std::move(e));
  };
  const char* p = content.data();
  std::size_t left = content.size();
  while (left > 0) {
    const ssize_t w = ::write(fd, p, left);
    if (w < 0) {
      if (errno == EINTR) continue;
      return abort_with("write failed");
    }
    p += w;
    left -= static_cast<std::size_t>(w);
  }
  if (::fsync(fd) != 0) return abort_with("fsync failed");
  ::close(fd);
  if (::rename(tmp.c_str(), file.c_str()) != 0) {
    Error e = errno_error(Errc::Io, "rename failed: " + file.string());
    ::unlink(tmp.c_str());
    return fail(std::move(e));
  }
  const int dfd = ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    (void)::fsync(dfd);
    ::close(dfd);
  }
  return {};
}

}  // namespace opt
