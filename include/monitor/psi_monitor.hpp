#pragma once
#include <filesystem>

#include "core/result.hpp"
#include "monitor/types.hpp"

namespace opt::monitor {

class PsiMonitor {
 public:
  explicit PsiMonitor(std::filesystem::path root = "/") : root_(std::move(root)) {}
  /// Unsupported when no /proc/pressure file is readable.
  Result<PsiSample> sample() const;

 private:
  std::filesystem::path root_;
};

}  // namespace opt::monitor
