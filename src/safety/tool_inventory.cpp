#include "safety/tool_inventory.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <sys/stat.h>

namespace opt::safety {
namespace fs = std::filesystem;
namespace {

struct KnownTool {
  const char* id;
  const char* name;
  const char* description;
  std::vector<policy::Subsystem> subsystems;
  bool thermal;
  bool game_scoped;
  const char* package_debian;
  const char* package_arch;
  const char* package_fedora;
  const char* unit;
  std::vector<std::string> executables;
};

const std::vector<KnownTool>& catalog() {
  static const std::vector<KnownTool> items = {
      {"power-profiles-daemon", "power-profiles-daemon", "CPU energy/performance preference and platform profile",
       {policy::Subsystem::Cpu}, false, false, "power-profiles-daemon", "power-profiles-daemon", "power-profiles-daemon",
       "power-profiles-daemon.service", {"power-profiles-daemon", "powerprofilesctl"}},
      {"tuned", "TuneD", "CPU, I/O, sysctl and runtime-PM profiles",
       {policy::Subsystem::Cpu, policy::Subsystem::Io, policy::Subsystem::Memory, policy::Subsystem::RuntimePm}, false, false,
       "tuned", "tuned", "tuned", "tuned.service", {"tuned", "tuned-adm"}},
      {"auto-cpufreq", "auto-cpufreq", "CPU governor, EPP and turbo controls",
       {policy::Subsystem::Cpu}, false, false, "auto-cpufreq", "auto-cpufreq", "auto-cpufreq",
       "auto-cpufreq.service", {"auto-cpufreq"}},
      {"system76-power", "system76-power", "CPU/GPU power profiles",
       {policy::Subsystem::Cpu, policy::Subsystem::Gpu, policy::Subsystem::RuntimePm}, false, false,
       "system76-power", "system76-power", "system76-power", "system76-power.service", {"system76-power"}},
      {"tlp", "TLP", "CPU, disk and runtime-PM settings; service may remain active after setup",
       {policy::Subsystem::Cpu, policy::Subsystem::Io, policy::Subsystem::RuntimePm}, false, false,
       "tlp", "tlp", "tlp", "tlp.service", {"tlp"}},
      {"thermald", "thermald", "thermal management and cooling policy",
       {}, true, false, "thermald", "thermald", "thermald", "thermald.service", {"thermald"}},
      {"gamemode", "GameMode", "game-scoped CPU, GPU and I/O tuning",
       {policy::Subsystem::Cpu, policy::Subsystem::Gpu, policy::Subsystem::Io}, false, true,
       "gamemode", "gamemode", "gamemode", "gamemoded.service", {"gamemoded"}},
  };
  return items;
}

bool unit_exists(const fs::path& root, std::string_view unit) {
  if (unit.empty()) return false;
  for (const char* dir : {"etc/systemd/system", "run/systemd/system", "usr/lib/systemd/system", "lib/systemd/system"}) {
    std::error_code ec;
    const auto p = root / dir / unit;
    if (fs::exists(p, ec) || fs::is_symlink(p, ec)) return true;
  }
  return false;
}

std::vector<std::string> words(const std::string& s) {
  std::istringstream in(s);
  std::vector<std::string> out;
  std::string word;
  while (in >> word) out.push_back(std::move(word));
  return out;
}

struct UnitTables {
  bool queried = false;
  bool files_queried = false;
  std::map<std::string, UnitActivity> active;
  std::map<std::string, std::string> file_state;
};

UnitTables inspect_systemd(const fs::path& root) {
  UnitTables result;
  // Fake-root calls in tests must never inspect or interact with the host's systemd.
  if (root != "/") return result;
  const auto systemctl = platform::find_executable(root, "systemctl");
  if (systemctl.empty()) return result;
  const auto active = platform::run_capture({systemctl.string(), "list-units", "--all", "--type=service", "--no-legend", "--plain"},
                                            std::chrono::milliseconds(750), 1024 * 1024);
  const auto files = platform::run_capture({systemctl.string(), "list-unit-files", "--type=service", "--no-legend"},
                                           std::chrono::milliseconds(750), 1024 * 1024);
  result.queried = active.launched && !active.timed_out && active.exit_code == 0;
  if (result.queried) {
    std::istringstream in(active.output);
    std::string line;
    while (std::getline(in, line)) {
      auto w = words(line);
      if (w.size() < 4 || !w[0].ends_with(".service")) continue;
      result.active[w[0]] = classify_unit_active_state(w[2]);
    }
  }
  result.files_queried = files.launched && !files.timed_out && files.exit_code == 0;
  if (result.files_queried) {
    std::istringstream in(files.output);
    std::string line;
    while (std::getline(in, line)) {
      auto w = words(line);
      if (w.size() >= 2 && w[0].ends_with(".service")) result.file_state[w[0]] = w[1];
    }
  }
  return result;
}

std::string pkg_name(const KnownTool& k, platform::PackageManagerKind m) {
  switch (m) {
    case platform::PackageManagerKind::AptGet: return k.package_debian;
    case platform::PackageManagerKind::Pacman: return k.package_arch;
    case platform::PackageManagerKind::Dnf: return k.package_fedora;
    case platform::PackageManagerKind::Unknown: return {};
  }
  return {};
}

}  // namespace

UnitActivity classify_unit_active_state(std::string_view state) noexcept {
  if (state == "active" || state == "activating" || state == "deactivating" ||
      state == "reloading" || state == "maintenance")
    return UnitActivity::Active;
  if (state == "inactive") return UnitActivity::Inactive;
  if (state == "failed") return UnitActivity::Failed;
  return UnitActivity::Unknown;
}

bool ToolInventoryEntry::installed() const noexcept {
  return package_state == platform::PackageState::Installed || executable_found || unit_file_found;
}

bool ToolInventoryEntry::currently_owns_subsystems() const noexcept {
  return process_running || unit_activity == UnitActivity::Active;
}

ToolInventoryReport detect_tool_inventory(const std::vector<ExternalTool>& running, const fs::path& root) {
  ToolInventoryReport report;
  report.platform = platform::detect_platform(root);
  const auto packages = platform::installed_package_names(report.platform, root);
  const bool has_package_db = (report.platform.manager == platform::PackageManagerKind::AptGet && fs::exists(root / "var/lib/dpkg/status")) ||
                              (report.platform.manager == platform::PackageManagerKind::Pacman && fs::is_directory(root / "var/lib/pacman/local")) ||
                              (report.platform.manager == platform::PackageManagerKind::Dnf && !packages.empty());
  report.entries.reserve(catalog().size());
  for (const auto& k : catalog()) {
    ToolInventoryEntry e;
    e.id = k.id;
    e.tool.name = k.name;
    e.tool.description = k.description;
    e.tool.subsystems = k.subsystems;
    e.tool.thermal = k.thermal;
    e.tool.game_scoped = k.game_scoped;
    e.package_name = pkg_name(k, report.platform.manager);
    if (!e.package_name.empty() && packages.contains(e.package_name)) e.package_state = platform::PackageState::Installed;
    else if (has_package_db && !e.package_name.empty()) e.package_state = platform::PackageState::Missing;

    for (const auto& ex : k.executables) {
      if (!platform::find_executable(root, ex).empty()) { e.executable_found = true; break; }
    }
    e.unit_file_found = unit_exists(root, k.unit);
    e.unit_file_state = "unknown";

    for (const auto& t : running) {
      if (t.name == e.tool.name || (e.id == "gamemode" && t.name == "GameMode")) {
        e.process_running = t.pid > 0;
        e.tool.pid = t.pid;
        break;
      }
    }
    if ((e.executable_found || e.process_running) && e.package_state == platform::PackageState::Missing)
      e.package_state = platform::PackageState::Installed;  // local/custom install not owned by distro package DB
    report.entries.push_back(std::move(e));
  }
  refresh_tool_unit_states(report, root);
  return report;
}

void refresh_tool_unit_states(ToolInventoryReport& report, const fs::path& root) {
  const auto units = inspect_systemd(root);
  report.systemd_query_succeeded = units.queried;
  for (auto& entry : report.entries) {
    const auto def = std::find_if(catalog().begin(), catalog().end(), [&](const KnownTool& k) { return entry.id == k.id; });
    if (def == catalog().end()) continue;
    entry.unit_file_found = unit_exists(root, def->unit);
    if (units.files_queried) {
      entry.unit_file_state = "unknown";
      if (const auto it = units.file_state.find(def->unit); it != units.file_state.end()) entry.unit_file_state = it->second;
    }
    // Do not erase a last-known active owner when the system manager is
    // temporarily unreachable; this biases toward observe-only rather than conflict.
    if (units.queried) {
      if (const auto it = units.active.find(def->unit); it != units.active.end()) entry.unit_activity = it->second;
      else entry.unit_activity = UnitActivity::Inactive;
    }
  }
}

std::vector<ExternalTool> effective_tool_owners(const ToolInventoryReport& report,
                                                const std::vector<ExternalTool>& process_tools) {
  std::vector<ExternalTool> out = process_tools;
  for (const auto& e : report.entries) {
    // Process ownership comes from the fresh caller snapshot; the inventory may
    // be cached for tens of seconds, so only use its systemd state here.
    if (e.unit_activity != UnitActivity::Active) continue;
    const bool already = std::any_of(out.begin(), out.end(), [&](const ExternalTool& t) { return t.name == e.tool.name; });
    if (!already) out.push_back(e.tool);
  }
  return out;
}

std::string format_tool_inventory(const ToolInventoryReport& report) {
  using platform::PackageState;
  std::ostringstream out;
  out << "Distribution: " << report.platform.pretty_name << " (ID=" << (report.platform.id.empty() ? "unknown" : report.platform.id) << ")\n";
  out << "Package manager: " << platform::to_string(report.platform.manager);
  if (!report.platform.manager_path.empty()) out << " (" << report.platform.manager_path.string() << ")";
  else out << " (not found / unsupported distro)";
  out << "\nSystemd unit query: " << (report.systemd_query_succeeded ? "available" : "unavailable or query failed") << "\n\n";
  out << "External performance/thermal tools (read-only inventory):\n";
  for (const auto& e : report.entries) {
    out << "  " << e.tool.name << ": ";
    if (e.currently_owns_subsystems()) out << "ACTIVE OWNER";
    else if (e.installed()) out << "installed, not active";
    else if (e.package_state == PackageState::Missing) out << "not installed";
    else out << "unknown";
    if (e.process_running) out << "; process pid=" << e.tool.pid;
    if (!e.package_name.empty()) {
      out << "; package " << e.package_name << "=";
      switch (e.package_state) {
        case PackageState::Installed: out << "installed"; break;
        case PackageState::Missing: out << "missing"; break;
        case PackageState::Unknown: out << "unknown"; break;
      }
    }
    if (e.unit_file_found) out << "; unit file present";
    if (e.unit_activity == UnitActivity::Active) out << "; unit active";
    else if (e.unit_activity == UnitActivity::Inactive) out << "; unit inactive";
    else if (e.unit_activity == UnitActivity::Failed) out << "; unit failed";
    if (!e.unit_file_state.empty() && e.unit_file_state != "unknown") out << "; unit-file=" << e.unit_file_state;
    if (!e.tool.subsystems.empty()) {
      out << "; may own:";
      for (const auto s : e.tool.subsystems) out << ' ' << policy::to_string(s);
    }
    if (e.tool.thermal) out << "; thermal subsystem";
    if (e.tool.game_scoped) out << "; game-scoped";
    out << '\n';
  }
  out << "\nPolicy rule: only live processes or active services become subsystem owners. Installed-but-inactive tools are informational.\n";
  out << "No external tool is stopped, disabled, installed, or reconfigured by this inventory.\n";
  return out.str();
}

}  // namespace opt::safety
