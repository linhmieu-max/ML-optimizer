#pragma once
// Persists the user's profile choice/lock in the daemon's own state directory
// (never in /sys). Atomic: write temp + fsync + rename.

#include <filesystem>

#include "core/result.hpp"
#include "policy/policy_engine.hpp"

namespace opt::policy {

Result<UserMode> load_user_mode(const std::filesystem::path& file);  // missing file -> default (auto)
Result<void> save_user_mode(const std::filesystem::path& file, const UserMode&);

}  // namespace opt::policy
