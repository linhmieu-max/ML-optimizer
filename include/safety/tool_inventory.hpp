#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "platform/package_manager.hpp"
#include "safety/conflict_detector.hpp"

namespace opt::safety {

enum class UnitActivity { Active, Inactive, Failed, Unknown };

/// Map systemd's ACTIVE column to an ownership decision. Transitional states
/// still reserve the subsystem; treating them as inactive could briefly let the
/// optimizer fight a service while it is starting or stopping.
UnitActivity classify_unit_active_state(std::string_view state) noexcept;

struct ToolInventoryEntry {
  std::string id;
  ExternalTool tool;
  std::string package_name;
  platform::PackageState package_state = platform::PackageState::Unknown;
  bool executable_found = false;
  bool unit_file_found = false;
  std::string unit_file_state;
  UnitActivity unit_activity = UnitActivity::Unknown;
  bool process_running = false;

  bool installed() const noexcept;
  bool currently_owns_subsystems() const noexcept;
};

struct ToolInventoryReport {
  platform::PlatformInfo platform;
  std::vector<ToolInventoryEntry> entries;
  bool systemd_query_succeeded = false;
};

/// Read-only inventory of well-known external performance/thermal tools.
/// `running` is populated by the /proc process scanner. Package databases and
/// systemd are only queried for an explicit `conflicts`/inventory request or a
/// bounded daemon refresh; no service is changed, stopped, enabled or disabled.
ToolInventoryReport detect_tool_inventory(const std::vector<ExternalTool>& running,
                                          const std::filesystem::path& root = "/");

/// Refresh only unit activity (quick, read-only) while preserving the less
/// frequently refreshed package database. If systemd is temporarily unreachable,
/// previously known activity is kept conservatively rather than declaring owners idle.
void refresh_tool_unit_states(ToolInventoryReport& report, const std::filesystem::path& root = "/");

/// Add system services that are active even if their one-shot process has exited
/// (notably TLP). Only active services or live processes become policy owners;
/// installed-but-inactive packages never suppress optimizer control.
std::vector<ExternalTool> effective_tool_owners(const ToolInventoryReport& report,
                                                const std::vector<ExternalTool>& process_tools);

std::string format_tool_inventory(const ToolInventoryReport& report);

}  // namespace opt::safety
