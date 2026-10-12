#include "ipc/client.hpp"

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace opt::ipc {

Result<Response> call(const std::filesystem::path& socket_path, const Request& request, int timeout_ms) {
  const std::string p = socket_path.string();
  sockaddr_un addr{};
  if (p.empty() || p.size() >= sizeof addr.sun_path)
    return fail(make_error(Errc::InvalidArgument, "socket path empty or too long"));
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, p.c_str(), p.size() + 1);

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return fail(errno_error(Errc::Io, "socket() failed"));
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};

  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
    return fail(errno_error(Errc::Io, "cannot connect to " + p));
  if (auto w = write_frame(fd, serialize_request(request), kMaxRequestBytes); !w) return fail(w.error());
  auto frame = read_frame(fd, kMaxResponseBytes);
  if (!frame) return fail(frame.error());
  return parse_response(*frame);
}

}  // namespace opt::ipc
