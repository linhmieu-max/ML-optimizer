#pragma once
// Wire format (protocol version 1)
//   frame    := uint32 big-endian length, then `length` payload bytes
//   request  := "OPT/1 <command> [arg...]"          (space separated tokens)
//   response := "OPT/1 OK\n<body>" | "OPT/1 ERR <errc>\n<body>"
// Tokens are restricted to [A-Za-z0-9._:=-]; commands to [a-z0-9-].

#include <cstddef>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "core/result.hpp"

namespace opt::ipc {

inline constexpr std::string_view kMagic = "OPT/1";
inline constexpr std::size_t kMaxRequestBytes = 1024;
inline constexpr std::size_t kMaxResponseBytes = 64 * 1024;
inline constexpr std::size_t kMaxArgs = 8;
inline constexpr std::size_t kMaxTokenLen = 64;

struct Request {
  std::string command;
  std::vector<std::string> args;
};

struct Response {
  bool ok = true;
  Errc code = Errc::Ok;
  std::string body;
};

inline Response ok_response(std::string body = {}) { return Response{true, Errc::Ok, std::move(body)}; }
inline Response error_response(Errc code, std::string msg) { return Response{false, code, std::move(msg)}; }

struct PeerCred {
  uid_t uid = static_cast<uid_t>(-1);
  gid_t gid = static_cast<gid_t>(-1);
  pid_t pid = 0;
};

Result<void> write_frame(int fd, std::string_view payload, std::size_t max_bytes);
Result<std::string> read_frame(int fd, std::size_t max_bytes);

Result<Request> parse_request(std::string_view payload);
std::string serialize_request(const Request&);
std::string serialize_response(const Response&);
Result<Response> parse_response(std::string_view payload);

/// SO_PEERCRED: identity of the connecting process as recorded by the kernel.
Result<PeerCred> peer_credentials(int fd);

}  // namespace opt::ipc
