#include "telemetry/ids.hpp"

#include <random>

#include "core/atomic_file.hpp"
#include "core/sha256.hpp"
#include "core/sysfs.hpp"

namespace opt::telemetry {
namespace {
std::string random_hex(std::size_t bytes) {
  std::random_device rd;
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < bytes; ++i) {
    const unsigned v = rd() & 0xff;
    out.push_back(hex[v >> 4]);
    out.push_back(hex[v & 15]);
  }
  return out;
}
bool valid_salt(const std::string& s) {
  if (s.size() != 32) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
}  // namespace

std::string make_session_id() { return random_hex(8); }

Result<std::string> rotate_salt(const std::filesystem::path& file) {
  const std::string salt = random_hex(16);
  if (auto r = write_file_atomic(file, salt + "\n", 0600); !r) return fail(r.error());
  return salt;
}

Result<std::string> load_or_create_salt(const std::filesystem::path& file) {
  if (auto t = sysfs::read_text(file, 128); t && valid_salt(*t)) return *t;
  return rotate_salt(file);
}

std::string device_id_hash(const std::string& salt, const std::string& machine_id) {
  return sha256_hex(salt + ":" + machine_id).substr(0, 16);
}

std::string read_machine_id(const std::filesystem::path& root) {
  return sysfs::read_text(root / "etc/machine-id", 128).value_or("");
}

}  // namespace opt::telemetry
