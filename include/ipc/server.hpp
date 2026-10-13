#pragma once

#include <filesystem>
#include <functional>
#include <thread>

#include "ipc/protocol.hpp"

namespace opt::ipc {

/// Single-threaded accept loop on a Unix domain socket. One short-lived
/// request/response per connection, 2 s I/O timeouts, bounded frame sizes.
/// The handler is responsible for authorization (it receives SO_PEERCRED data).
class IpcServer {
 public:
  using Handler = std::function<Response(const Request&, const PeerCred&)>;

  IpcServer(std::filesystem::path socket_path, Handler handler);
  ~IpcServer();
  IpcServer(const IpcServer&) = delete;
  IpcServer& operator=(const IpcServer&) = delete;

  Result<void> start();
  void stop();  // idempotent; removes the socket file

 private:
  void loop(std::stop_token);
  void serve(int client_fd);

  std::filesystem::path path_;
  Handler handler_;
  int listen_fd_ = -1;
  int wake_fd_ = -1;
  std::jthread thread_;
};

}  // namespace opt::ipc
