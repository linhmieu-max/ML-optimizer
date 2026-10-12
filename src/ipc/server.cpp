#include "ipc/server.hpp"

#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/logging.hpp"

namespace opt::ipc {
namespace {
constexpr timeval kIoTimeout{2, 0};
}

IpcServer::IpcServer(std::filesystem::path socket_path, Handler handler)
    : path_(std::move(socket_path)), handler_(std::move(handler)) {}

IpcServer::~IpcServer() { stop(); }

Result<void> IpcServer::start() {
  namespace fs = std::filesystem;
  const std::string p = path_.string();
  sockaddr_un addr{};
  if (p.empty() || p.size() >= sizeof addr.sun_path)
    return fail(make_error(Errc::InvalidArgument, "socket path empty or too long"));

  std::error_code ec;
  const fs::path dir = path_.parent_path();
  if (!fs::exists(dir, ec)) {
    fs::create_directories(dir, ec);
    if (ec) return fail(make_error(Errc::Io, "cannot create " + dir.string(), ec.value()));
    fs::permissions(dir, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                             fs::perms::others_read | fs::perms::others_exec, ec);
  }

  // Remove a stale socket left by a crashed instance; never touch anything else.
  struct stat st{};
  if (::lstat(p.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) return fail(make_error(Errc::Io, p + " exists and is not a socket"));
    ::unlink(p.c_str());
  }

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) return fail(errno_error(Errc::Io, "socket() failed"));
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, p.c_str(), p.size() + 1);

  auto cleanup = [&](Error e) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return fail(std::move(e));
  };
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
    return cleanup(errno_error(Errc::Io, "bind(" + p + ") failed"));
  // Any local user may connect; authorization happens per command via SO_PEERCRED.
  if (::chmod(p.c_str(), 0666) != 0) {
    auto e = errno_error(Errc::Io, "chmod(" + p + ") failed");
    ::unlink(p.c_str());
    return cleanup(std::move(e));
  }
  if (::listen(listen_fd_, 16) != 0) {
    auto e = errno_error(Errc::Io, "listen() failed");
    ::unlink(p.c_str());
    return cleanup(std::move(e));
  }
  wake_fd_ = ::eventfd(0, EFD_CLOEXEC);
  if (wake_fd_ < 0) {
    auto e = errno_error(Errc::Io, "eventfd() failed");
    ::unlink(p.c_str());
    return cleanup(std::move(e));
  }
  thread_ = std::jthread([this](std::stop_token token) { loop(token); });
  log::info("ipc", "listening", {{"socket", p}});
  return {};
}

void IpcServer::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    const std::uint64_t one = 1;
    if (wake_fd_ >= 0) (void)!::write(wake_fd_, &one, sizeof one);
    thread_.join();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(path_.c_str());
  }
  if (wake_fd_ >= 0) {
    ::close(wake_fd_);
    wake_fd_ = -1;
  }
}

void IpcServer::loop(std::stop_token st) {
  pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
  while (!st.stop_requested()) {
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      log::error("ipc", "poll failed", {{"errno", std::strerror(errno)}});
      return;
    }
    if (fds[1].revents & POLLIN) return;
    if (fds[0].revents & POLLIN) {
      const int c = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (c < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED)
          log::warn("ipc", "accept failed", {{"errno", std::strerror(errno)}});
        continue;
      }
      serve(c);
      ::close(c);
    }
  }
}

void IpcServer::serve(int fd) {
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &kIoTimeout, sizeof kIoTimeout);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &kIoTimeout, sizeof kIoTimeout);

  auto reply = [&](const Response& r) {
    if (auto w = write_frame(fd, serialize_response(r), kMaxResponseBytes); !w)
      log::debug("ipc", "reply failed", {{"error", describe(w.error())}});
  };

  auto peer = peer_credentials(fd);
  if (!peer) return reply(error_response(Errc::Internal, "cannot identify peer"));
  auto frame = read_frame(fd, kMaxRequestBytes);
  if (!frame) {
    log::debug("ipc", "bad frame", {{"error", describe(frame.error())}, {"pid", std::to_string(peer->pid)}});
    return reply(error_response(frame.error().code, frame.error().message));
  }
  auto req = parse_request(*frame);
  if (!req) return reply(error_response(req.error().code, req.error().message));

  try {
    reply(handler_(*req, *peer));
  } catch (const std::exception& e) {
    log::error("ipc", "handler threw", {{"what", e.what()}});
    reply(error_response(Errc::Internal, "internal error"));
  }
}

}  // namespace opt::ipc
