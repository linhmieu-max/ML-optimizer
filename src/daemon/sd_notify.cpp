#include "sd_notify.hpp"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace opt::sd {

bool notify(std::string_view state) {
  const char* env = std::getenv("NOTIFY_SOCKET");
  if (!env || !*env) return false;
  const std::string_view path(env);
  sockaddr_un addr{};
  if (path.size() < 2 || (path[0] != '/' && path[0] != '@') || path.size() >= sizeof addr.sun_path)
    return false;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.data(), path.size());
  if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';  // abstract namespace

  const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());
  const ssize_t n = ::sendto(fd, state.data(), state.size(), MSG_NOSIGNAL,
                             reinterpret_cast<const sockaddr*>(&addr), len);
  ::close(fd);
  return n == static_cast<ssize_t>(state.size());
}

std::uint64_t watchdog_usec() {
  const char* usec = std::getenv("WATCHDOG_USEC");
  if (!usec) return 0;
  if (const char* pid = std::getenv("WATCHDOG_PID")) {
    if (std::strtol(pid, nullptr, 10) != static_cast<long>(::getpid())) return 0;
  }
  std::uint64_t v = 0;
  const std::string_view sv(usec);
  auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
  return (ec == std::errc{} && p == sv.data() + sv.size()) ? v : 0;
}

}  // namespace opt::sd
