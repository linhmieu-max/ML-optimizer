#pragma once
// Privacy: telemetry never stores the machine-id, hostname or user names. The device id is a
// truncated SHA-256 of (random per-installation salt + machine-id); deleting telemetry rotates the salt.

#include <filesystem>
#include <string>

#include "core/result.hpp"

namespace opt::telemetry {

std::string make_session_id();  // 16 random hex chars, new for every daemon start
Result<std::string> load_or_create_salt(const std::filesystem::path& file);
Result<std::string> rotate_salt(const std::filesystem::path& file);
std::string device_id_hash(const std::string& salt, const std::string& machine_id);  // 16 hex chars
std::string read_machine_id(const std::filesystem::path& root = "/");

}  // namespace opt::telemetry
