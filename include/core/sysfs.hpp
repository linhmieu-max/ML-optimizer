#pragma once
// Small read-only helpers for /proc and /sys. Nothing here ever writes.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opt::sysfs {

/// Reads up to `max` bytes; trailing whitespace trimmed. nullopt on any error
/// (including EOPNOTSUPP/EIO/ENODATA some sysfs files return).
std::optional<std::string> read_text(const std::filesystem::path&, std::size_t max = 65536);
std::optional<long long> read_int(const std::filesystem::path&);
std::optional<double> read_double(const std::filesystem::path&);

std::optional<std::uint64_t> parse_u64(std::string_view);
std::optional<double> parse_double(std::string_view);

bool path_exists(const std::filesystem::path&);
bool all_digits(std::string_view);

/// Entries whose file name starts with `prefix`, in natural order
/// (thermal_zone2 before thermal_zone10).
std::vector<std::filesystem::path> list_dir(const std::filesystem::path&, std::string_view prefix = {});

std::vector<std::string_view> split_ws(std::string_view);
std::vector<std::string_view> split_lines(std::string_view);  // non-empty lines

}  // namespace opt::sysfs
