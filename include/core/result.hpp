#pragma once
// Error handling abstraction.
//
// opt::Result<T> is std::expected<T, Error> when the standard library provides
// it (requires -std=c++23 on GCC 12-14 / Clang 16+), otherwise the vendored
// header-only tl::expected (third_party/tl/expected.hpp, CC0). The two share
// the API subset used by this project: has_value(), operator*, ->, error().

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <version>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
#include <expected>
#define OPT_HAS_STD_EXPECTED 1
#else
#include <tl/expected.hpp>
#define OPT_HAS_STD_EXPECTED 0
#endif

namespace opt {

enum class Errc {
  Ok,
  Io,
  NotFound,
  PermissionDenied,
  InvalidArgument,
  Unsupported,
  Parse,
  Protocol,
  Timeout,
  Internal,
};

inline constexpr int kErrcCount = 10;

constexpr std::string_view to_string(Errc c) noexcept {
  switch (c) {
    case Errc::Ok: return "ok";
    case Errc::Io: return "io";
    case Errc::NotFound: return "not-found";
    case Errc::PermissionDenied: return "permission-denied";
    case Errc::InvalidArgument: return "invalid-argument";
    case Errc::Unsupported: return "unsupported";
    case Errc::Parse: return "parse";
    case Errc::Protocol: return "protocol";
    case Errc::Timeout: return "timeout";
    case Errc::Internal: return "internal";
  }
  return "internal";
}

struct Error {
  Errc code = Errc::Internal;
  std::string message;
  int sys_errno = 0;
};

inline Error make_error(Errc code, std::string message, int sys_errno = 0) {
  return Error{code, std::move(message), sys_errno};
}

/// Captures errno at call time.
inline Error errno_error(Errc code, std::string message) {
  const int saved = errno;
  return Error{code, std::move(message), saved};
}

inline std::string describe(const Error& e) {
  std::string s(to_string(e.code));
  s += ": ";
  s += e.message;
  if (e.sys_errno != 0) {
    s += " (";
    s += std::strerror(e.sys_errno);
    s += ')';
  }
  return s;
}

#if OPT_HAS_STD_EXPECTED
template <class T>
using Result = std::expected<T, Error>;
inline std::unexpected<Error> fail(Error e) { return std::unexpected<Error>(std::move(e)); }
#else
template <class T>
using Result = tl::expected<T, Error>;
inline tl::unexpected<Error> fail(Error e) { return tl::make_unexpected(std::move(e)); }
#endif

}  // namespace opt
