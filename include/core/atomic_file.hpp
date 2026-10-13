#pragma once

#include <filesystem>
#include <string_view>
#include <sys/types.h>

#include "core/result.hpp"

namespace opt {

/// Crash-safe replace: write temp file, fsync, rename over `file`, fsync the directory.
/// Creates missing parent directories (0750). Used for the daemon's own state files only.
Result<void> write_file_atomic(const std::filesystem::path& file, std::string_view content, mode_t mode = 0640);

}  // namespace opt
