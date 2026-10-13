#pragma once
#include <string>
#include <string_view>

namespace opt {
/// Lower-case hex SHA-256 (self-contained, no OpenSSL dependency).
std::string sha256_hex(std::string_view data);
}  // namespace opt
