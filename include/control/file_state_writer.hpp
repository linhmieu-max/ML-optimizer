#pragma once
// The ONLY code that writes to /sys and /proc/sys. It refuses anything that is not on
// the allowlist, resolves symlinks and re-checks that the real path stays inside
// /sys or /proc/sys, writes a single short line, and never creates files.

#include <filesystem>

#include "safety/rollback.hpp"

namespace opt::control {

class FileStateWriter final : public safety::StateWriter {
 public:
  /// `root` re-bases "/sys" and "/proc" (tests use a fake tree).
  explicit FileStateWriter(std::filesystem::path root = "/") : root_(std::move(root)) {}
  Result<std::string> read(const std::string& target) override;
  Result<void> write(const std::string& target, const std::string& value) override;

 private:
  Result<std::filesystem::path> resolve(const std::string& target) const;
  std::filesystem::path root_;
};

/// "[none] mq-deadline" -> "none" (active choice); io.weight keeps its first line only.
std::string normalize_value(const std::string& target, const std::string& text);

}  // namespace opt::control
