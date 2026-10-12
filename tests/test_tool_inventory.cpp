#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

#include "check.hpp"
#include "platform/package_manager.hpp"
#include "safety/tool_inventory.hpp"

namespace fs = std::filesystem;
using namespace opt;

namespace {
fs::path make_temp() {
  char t[] = "/tmp/opt-tools-XXXXXX";
  return ::mkdtemp(t);
}
void put(const fs::path& p, const std::string& value) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << value;
}
void executable(const fs::path& p) {
  put(p, "not executed in tests\n");
  ::chmod(p.c_str(), 0755);
}
}

void run_tool_inventory_tests() {
  CHECK(safety::classify_unit_active_state("active") == safety::UnitActivity::Active);
  CHECK(safety::classify_unit_active_state("activating") == safety::UnitActivity::Active);
  CHECK(safety::classify_unit_active_state("deactivating") == safety::UnitActivity::Active);
  CHECK(safety::classify_unit_active_state("reloading") == safety::UnitActivity::Active);
  CHECK(safety::classify_unit_active_state("maintenance") == safety::UnitActivity::Active);
  CHECK(safety::classify_unit_active_state("inactive") == safety::UnitActivity::Inactive);
  CHECK(safety::classify_unit_active_state("failed") == safety::UnitActivity::Failed);
  CHECK(safety::classify_unit_active_state("unknown-state") == safety::UnitActivity::Unknown);

  const fs::path root = make_temp();
  put(root / "etc/os-release", "ID=debian\nPRETTY_NAME=\"Debian Test\"\nID_LIKE=debian\n");
  executable(root / "usr/bin/apt-get");
  put(root / "var/lib/dpkg/status",
      "Package: power-profiles-daemon\nStatus: install ok installed\nVersion: 1\n\n"
      "Package: lm-sensors\nStatus: install ok installed\nVersion: 1\n\n");
  put(root / "usr/lib/systemd/system/power-profiles-daemon.service", "[Unit]\nDescription=fixture\n");
  executable(root / "usr/bin/power-profiles-daemon");

  const auto platform = platform::detect_platform(root);
  CHECK(platform.id == "debian");
  CHECK(platform.pretty_name == "Debian Test");
  CHECK(platform.manager == platform::PackageManagerKind::AptGet);
  CHECK(!platform.manager_path.empty());
  CHECK(platform::package_status("lm-sensors", platform, root) == platform::PackageState::Installed);
  CHECK(platform::package_status("thermald", platform, root) == platform::PackageState::Missing);

  const auto optional = platform::optional_packages();
  CHECK(optional.size() >= 2);
  CHECK(platform::package_name_for(optional[0], platform::PackageManagerKind::AptGet) == std::optional<std::string>("lm-sensors"));
  CHECK(platform::package_name_for(optional[0], platform::PackageManagerKind::Pacman) == std::optional<std::string>("lm_sensors"));

  safety::ExternalTool ppd;
  ppd.name = "power-profiles-daemon";
  ppd.description = "running fixture";
  ppd.pid = 42;
  ppd.subsystems = {policy::Subsystem::Cpu};
  const auto report = safety::detect_tool_inventory({ppd}, root);
  CHECK(report.platform.manager == platform::PackageManagerKind::AptGet);
  CHECK(!report.systemd_query_succeeded);  // fake-root tests never query host systemd
  const auto it = std::find_if(report.entries.begin(), report.entries.end(), [](const auto& e) { return e.id == "power-profiles-daemon"; });
  CHECK(it != report.entries.end());
  if (it != report.entries.end()) {
    CHECK(it->installed());
    CHECK(it->process_running && it->tool.pid == 42);
    CHECK(it->unit_file_found);
    CHECK(it->currently_owns_subsystems());
  }

  const auto owners = safety::effective_tool_owners(report, {ppd});
  CHECK(owners.size() == 1 && owners.front().name == "power-profiles-daemon");
  const auto no_process_owners = safety::effective_tool_owners(report, {});
  CHECK(no_process_owners.empty());  // installed unit/package without active process/unit is not a conflict

  // An active or transitional systemd owner must suppress overlapping control
  // even when the service's process has already exited (e.g. one-shot TLP).
  safety::ToolInventoryReport unit_only;
  safety::ToolInventoryEntry unit_owner;
  unit_owner.id = "tlp";
  unit_owner.tool.name = "TLP";
  unit_owner.tool.subsystems = {policy::Subsystem::Cpu, policy::Subsystem::Io};
  unit_owner.unit_activity = safety::classify_unit_active_state("active");
  unit_only.entries.push_back(unit_owner);
  auto unit_owners = safety::effective_tool_owners(unit_only, {});
  CHECK(unit_owners.size() == 1 && unit_owners.front().name == "TLP");
  unit_only.entries.front().unit_activity = safety::classify_unit_active_state("activating");
  unit_owners = safety::effective_tool_owners(unit_only, {});
  CHECK(unit_owners.size() == 1 && unit_owners.front().name == "TLP");
  unit_only.entries.front().unit_activity = safety::classify_unit_active_state("inactive");
  CHECK(safety::effective_tool_owners(unit_only, {}).empty());

  const auto rejected = platform::install_native_package(platform, "arbitrary-user-package");
  CHECK(!rejected && rejected.error().code == Errc::PermissionDenied);  // installer enforces its own allowlist
  const auto formatted = safety::format_tool_inventory(report);
  CHECK(formatted.find("Debian Test") != std::string::npos);
  CHECK(formatted.find("ACTIVE OWNER") != std::string::npos);
  CHECK(formatted.find("stopped") != std::string::npos);

  // Arch is detected from os-release and the local pacman database only; no
  // package-manager binary is ever executed during a fake-root unit test.
  put(root / "etc/os-release", "ID=arch\nPRETTY_NAME=Arch Test\n");
  executable(root / "usr/bin/pacman");
  put(root / "var/lib/pacman/local/lm_sensors-1.0-1/desc", "%NAME%\nlm_sensors\n%VERSION%\n1.0-1\n");
  const auto arch = platform::detect_platform(root);
  CHECK(arch.manager == platform::PackageManagerKind::Pacman);
  CHECK(platform::package_status("lm_sensors", arch, root) == platform::PackageState::Installed);
  CHECK(platform::package_name_for(optional[0], arch.manager) == std::optional<std::string>("lm_sensors"));

  // Fedora backend detection uses dnf; package-state queries are only executed
  // for the real root, never against this fixture.
  put(root / "etc/os-release", "ID=fedora\nPRETTY_NAME=Fedora Test\nID_LIKE=fedora\n");
  executable(root / "usr/bin/dnf");
  executable(root / "usr/bin/rpm");
  const auto fedora = platform::detect_platform(root);
  CHECK(fedora.manager == platform::PackageManagerKind::Dnf);
  CHECK(platform::package_name_for(optional[0], fedora.manager) == std::optional<std::string>("lm_sensors"));
  CHECK(platform::package_name_for(optional[1], fedora.manager) == std::optional<std::string>("thermald"));
  CHECK(platform::package_status("thermald", fedora, root) == platform::PackageState::Unknown);

  // An unsupported OS is reported instead of guessing a package manager.
  put(root / "etc/os-release", "ID=unknownos\nPRETTY_NAME=Unknown OS\n");
  const auto unknown = platform::detect_platform(root);
  CHECK(unknown.manager == platform::PackageManagerKind::Unknown);
  CHECK(!platform::package_name_for(optional[0], unknown.manager));
  fs::remove_all(root);
}
