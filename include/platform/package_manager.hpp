#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"

namespace opt::platform {

enum class PackageManagerKind { AptGet, Pacman, Dnf, Unknown };

struct PlatformInfo {
  std::string id;
  std::string pretty_name;
  std::string id_like;
  PackageManagerKind manager = PackageManagerKind::Unknown;
  std::filesystem::path manager_path;
};

enum class PackageState { Installed, Missing, Unknown };

struct OptionalPackage {
  std::string id;  // stable CLI feature identifier; never an arbitrary package name
  std::string label;
  std::string package_debian;
  std::string package_arch;
  std::string package_fedora;
  std::string purpose;
  std::string warning;
};

struct CommandResult {
  bool launched = false;
  bool timed_out = false;
  int exit_code = -1;
  std::string output;
};

PlatformInfo detect_platform(const std::filesystem::path& root = "/");
std::string_view to_string(PackageManagerKind kind) noexcept;
std::vector<OptionalPackage> optional_packages();
std::optional<std::string> package_name_for(const OptionalPackage&, PackageManagerKind);
PackageState package_status(std::string_view package, const PlatformInfo&,
                            const std::filesystem::path& root = "/");
std::set<std::string> installed_package_names(const PlatformInfo&,
                                             const std::filesystem::path& root = "/");

/// Run a fixed argv vector without a shell. Intended only for read-only inspection
/// commands such as systemctl/rpm. The caller must use trusted executable paths.
CommandResult run_capture(const std::vector<std::string>& argv,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(1500),
                          std::size_t max_output = 1024 * 1024);

/// Launch a native package-manager transaction without a shell. The package must
/// already have come from the built-in allowlist. This function does not ask for
/// confirmation; callers must obtain explicit interactive confirmation first.
Result<int> install_native_package(const PlatformInfo&, std::string_view package);

bool executable_at(const std::filesystem::path& root, std::string_view name);
std::filesystem::path find_executable(const std::filesystem::path& root, std::string_view name);

}  // namespace opt::platform
