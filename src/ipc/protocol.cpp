#include "ipc/protocol.hpp"

#include <cstdint>
#include <sys/socket.h>

namespace opt::ipc {
namespace {

bool token_char(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
         c == '_' || c == '-' || c == ':' || c == '=';
}

bool valid_token(std::string_view t) noexcept {
  if (t.empty() || t.size() > kMaxTokenLen) return false;
  for (char c : t)
    if (!token_char(c)) return false;
  return true;
}

bool valid_command(std::string_view t) noexcept {
  if (t.empty() || t.size() > 32) return false;
  for (char c : t)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (true) {
    const auto pos = s.find(sep, start);
    if (pos == std::string_view::npos) {
      out.push_back(s.substr(start));
      return out;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
}

Result<void> write_all(int fd, const char* p, std::size_t n) {
  while (n > 0) {
    const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return fail(make_error(Errc::Timeout, "send timed out"));
      return fail(errno_error(Errc::Io, "send failed"));
    }
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return {};
}

Result<void> read_all(int fd, char* p, std::size_t n) {
  while (n > 0) {
    const ssize_t r = ::recv(fd, p, n, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return fail(make_error(Errc::Timeout, "receive timed out"));
      return fail(errno_error(Errc::Io, "recv failed"));
    }
    if (r == 0) return fail(make_error(Errc::Protocol, "connection closed mid-frame"));
    p += r;
    n -= static_cast<std::size_t>(r);
  }
  return {};
}

Result<Errc> parse_errc(std::string_view name) {
  for (int i = 0; i < kErrcCount; ++i)
    if (to_string(static_cast<Errc>(i)) == name) return static_cast<Errc>(i);
  return fail(make_error(Errc::Protocol, "unknown error code in response"));
}

}  // namespace

Result<void> write_frame(int fd, std::string_view payload, std::size_t max_bytes) {
  if (payload.empty() || payload.size() > max_bytes)
    return fail(make_error(Errc::InvalidArgument, "frame size out of range"));
  const auto len = static_cast<std::uint32_t>(payload.size());
  std::string buf;
  buf.reserve(4 + payload.size());
  buf.push_back(static_cast<char>((len >> 24) & 0xff));
  buf.push_back(static_cast<char>((len >> 16) & 0xff));
  buf.push_back(static_cast<char>((len >> 8) & 0xff));
  buf.push_back(static_cast<char>(len & 0xff));
  buf.append(payload);
  return write_all(fd, buf.data(), buf.size());
}

Result<std::string> read_frame(int fd, std::size_t max_bytes) {
  unsigned char hdr[4];
  if (auto r = read_all(fd, reinterpret_cast<char*>(hdr), 4); !r) return fail(r.error());
  const std::uint32_t len = (std::uint32_t{hdr[0]} << 24) | (std::uint32_t{hdr[1]} << 16) |
                            (std::uint32_t{hdr[2]} << 8) | std::uint32_t{hdr[3]};
  if (len == 0 || len > max_bytes) return fail(make_error(Errc::Protocol, "frame length out of range"));
  std::string payload(len, '\0');
  if (auto r = read_all(fd, payload.data(), payload.size()); !r) return fail(r.error());
  return payload;
}

Result<Request> parse_request(std::string_view payload) {
  const auto tokens = split(payload, ' ');
  if (tokens.size() < 2 || tokens[0] != kMagic)
    return fail(make_error(Errc::Protocol, "bad request header (expected OPT/1)"));
  if (!valid_command(tokens[1])) return fail(make_error(Errc::InvalidArgument, "invalid command name"));
  if (tokens.size() - 2 > kMaxArgs) return fail(make_error(Errc::InvalidArgument, "too many arguments"));
  Request req;
  req.command = std::string(tokens[1]);
  for (std::size_t i = 2; i < tokens.size(); ++i) {
    if (!valid_token(tokens[i])) return fail(make_error(Errc::InvalidArgument, "invalid argument token"));
    req.args.emplace_back(tokens[i]);
  }
  return req;
}

std::string serialize_request(const Request& r) {
  std::string s(kMagic);
  s += ' ';
  s += r.command;
  for (const auto& a : r.args) {
    s += ' ';
    s += a;
  }
  return s;
}

std::string serialize_response(const Response& r) {
  std::string s(kMagic);
  if (r.ok) {
    s += " OK\n";
  } else {
    s += " ERR ";
    s += to_string(r.code);
    s += '\n';
  }
  s += r.body;
  return s;
}

Result<Response> parse_response(std::string_view payload) {
  const auto nl = payload.find('\n');
  const std::string_view head = payload.substr(0, nl);
  const std::string_view body = nl == std::string_view::npos ? std::string_view{} : payload.substr(nl + 1);
  const auto t = split(head, ' ');
  if (t.size() < 2 || t[0] != kMagic) return fail(make_error(Errc::Protocol, "bad response header"));
  Response r;
  r.body = std::string(body);
  if (t[1] == "OK" && t.size() == 2) return r;
  if (t[1] == "ERR" && t.size() == 3) {
    auto code = parse_errc(t[2]);
    if (!code) return fail(code.error());
    r.ok = false;
    r.code = *code;
    return r;
  }
  return fail(make_error(Errc::Protocol, "bad response status"));
}

Result<PeerCred> peer_credentials(int fd) {
  ucred cred{};
  socklen_t len = sizeof cred;
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
    return fail(errno_error(Errc::Io, "SO_PEERCRED failed"));
  return PeerCred{cred.uid, cred.gid, cred.pid};
}

}  // namespace opt::ipc
