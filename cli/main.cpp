#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>

#include "core/config.hpp"
#include "core/version.hpp"
#include "ipc/client.hpp"
#include "platform/package_manager.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitDaemonError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNoDaemon = 3;

void usage(FILE* out) {
  std::fprintf(out,
               "ml-optimizer %s\n"
               "usage: ml-optimizer [--socket PATH] <command> [args...]\n\n"
               "  status              daemon, kernel, hardware capabilities, safety state\n"
               "  detect              re-probe capabilities (read-only)\n"
               "  profile [NAME] [--lock|--unlock]  show or set powersave|balanced|performance|auto [admin to set]\n"
               "  policy              advisory policy decision: profile, targets, subsystem control, reasons\n"
               "  training [ACTION]   local adaptation status; ACTION on|off|run|reset-local [admin]\n"
               "  model [MODE]        ML advisor: models, agreement with the rules, forecast; MODE off|observe|suggest [admin]\n"
               "  history [N]         recent activity per workload/policy (last 24 h + hourly summaries)\n"
               "  telemetry           local telemetry status; [admin] on | off | delete --yes\n"
               "  controllers [MODE]  what the controllers did / would do; MODE off|dry-run|apply [admin]\n"
               "  safety              thermal safety state, sensors/limits, hardware limits, rollback journal\n"
               "  reset               [admin] profile -> auto and roll back every optimizer change\n"
               "  conflicts           installed/running tools, systemd units and subsystem conflicts (read-only)\n"
               "  packages            optional diagnostics packages and package-manager detection (local, read-only)\n"
               "  install FEATURE     ask before installing allowlisted optional feature: sensors|thermald\n"
               "  workload [--watch [SEC]] current workload class, confidence, features, reasons\n"
               "  monitor [--watch [SEC]]  live CPU/RAM/PSI/thermal/power/I-O/GPU snapshot\n"
               "  ping | version      connectivity / version check\n"
               "  help                list every command the daemon knows (incl. planned ones)\n"
               "  set-log-level LVL   [admin] change daemon log level at runtime\n"
               "  reload-config       [admin] re-read the configuration file\n\n"
               "Socket: --socket, $ML_OPTIMIZER_SOCKET, or %s\n",
               opt::kVersion, opt::kDefaultSocketPath);
}



bool command_available(const std::string& name) {
  return !opt::platform::find_executable("/", name).empty();
}

int show_packages() {
  using namespace opt::platform;
  const auto platform = detect_platform();
  std::printf("Distribution: %s (ID=%s)\n", platform.pretty_name.c_str(), platform.id.empty() ? "unknown" : platform.id.c_str());
  std::printf("Native package manager: %s", std::string(to_string(platform.manager)).c_str());
  if (!platform.manager_path.empty()) std::printf(" (%s)", platform.manager_path.c_str());
  std::printf("\n\nOptional packages (none are required for the optimizer core):\n");
  for (const auto& item : optional_packages()) {
    const auto pkg = package_name_for(item, platform.manager);
    std::printf("  %s — %s\n", item.id.c_str(), item.label.c_str());
    if (!pkg) {
      std::printf("    package: unsupported on detected platform\n");
      continue;
    }
    auto state = package_status(*pkg, platform);
    if (item.id == "sensors" && command_available("sensors")) state = PackageState::Installed;
    if (item.id == "thermald" && command_available("thermald")) state = PackageState::Installed;
    std::printf("    package: %s (%s)\n", pkg->c_str(), state == PackageState::Installed ? "installed" :
                 state == PackageState::Missing ? "not installed" : "status unknown");
    std::printf("    purpose: %s\n", item.purpose.c_str());
    if (!item.warning.empty()) std::printf("    caution: %s\n", item.warning.c_str());
  }
  std::printf("\nNo package is installed automatically. To request an optional package, run:\n"
              "  ml-optimizer install sensors\n"
              "  ml-optimizer install thermald   (thermal-management overlap warning)\n"
              "The native package manager may download packages from your configured repositories only after confirmation.\n");
  if (platform.manager == PackageManagerKind::Unknown)
    std::printf("Supported native package managers: apt-get (Debian/Ubuntu), pacman (Arch), dnf (Fedora).\n");
  return kExitOk;
}

std::optional<int> handle_local_package_command(const opt::ipc::Request& req) {
  using namespace opt::platform;
  if (req.command == "packages") {
    if (!req.args.empty()) {
      std::fprintf(stderr, "usage: ml-optimizer packages\n");
      return kExitUsage;
    }
    return show_packages();
  }
  if (req.command != "install") return std::nullopt;
  if (req.args.size() != 1) {
    std::fprintf(stderr, "usage: ml-optimizer install <sensors|thermald>\n");
    return kExitUsage;
  }

  const auto catalog = optional_packages();
  const auto it = std::find_if(catalog.begin(), catalog.end(), [&](const OptionalPackage& p) { return p.id == req.args[0]; });
  if (it == catalog.end()) {
    std::fprintf(stderr, "ml-optimizer: '%s' is not an installable feature. Allowed: sensors, thermald.\n",
                 req.args[0].c_str());
    std::fprintf(stderr, "The installer accepts feature IDs only, never arbitrary package names.\n");
    return kExitUsage;
  }
  const auto platform = detect_platform();
  const auto package = package_name_for(*it, platform.manager);
  if (!package || platform.manager_path.empty()) {
    std::fprintf(stderr, "ml-optimizer: unsupported distro/package-manager combination. No installation attempted.\n");
    return kExitDaemonError;
  }
  auto state = package_status(*package, platform);
  if ((it->id == "sensors" && command_available("sensors")) ||
      (it->id == "thermald" && command_available("thermald"))) state = PackageState::Installed;
  if (state == PackageState::Installed) {
    std::printf("Already installed: feature '%s' (package %s). No changes made.\n", it->id.c_str(), package->c_str());
    return kExitOk;
  }
  if (!::isatty(STDIN_FILENO) || !::isatty(STDERR_FILENO)) {
    std::fprintf(stderr, "ml-optimizer: installation requires an interactive terminal so explicit confirmation can be obtained. No changes made.\n");
    return kExitUsage;
  }

  std::printf("Optional feature: %s\nPackage: %s\nDistribution: %s\nPackage manager: %s\n\n",
              it->label.c_str(), package->c_str(), platform.pretty_name.c_str(), std::string(to_string(platform.manager)).c_str());
  std::printf("Purpose: %s\n", it->purpose.c_str());
  if (!it->warning.empty()) {
    std::printf("WARNING: %s\n", it->warning.c_str());
    std::printf("To acknowledge this subsystem overlap, type INSTALL THERMALD: ");
    std::string warning_answer;
    if (!std::getline(std::cin, warning_answer) || warning_answer != "INSTALL THERMALD") {
      std::printf("Cancelled. No changes made.\n");
      return kExitOk;
    }
  }
  std::printf("The package manager may download and install packages from configured repositories.\n"
              "ML Optimizer will not stop or disable existing tools. The package manager may install/start the requested package's service according to distro policy.\n"
              "Type YES to proceed: ");
  std::string answer;
  if (!std::getline(std::cin, answer) || answer != "YES") {
    std::printf("Cancelled. No changes made.\n");
    return kExitOk;
  }

  auto result = install_native_package(platform, *package);
  if (!result) {
    std::fprintf(stderr, "ml-optimizer: installation was not started/completed: %s\n", opt::describe(result.error()).c_str());
    return kExitDaemonError;
  }
  if (*result != 0) {
    std::fprintf(stderr, "ml-optimizer: package manager exited with status %d. Review its output; no automatic rollback was attempted.\n", *result);
    return *result > 0 && *result < 126 ? *result : kExitDaemonError;
  }
  std::printf("Package transaction completed. Re-check external owners with: ml-optimizer conflicts\n");
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path socket = opt::kDefaultSocketPath;
  if (const char* env = std::getenv("ML_OPTIMIZER_SOCKET"); env && *env) socket = env;

  int i = 1;
  for (; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "--socket" || a == "-s") && i + 1 < argc) socket = argv[++i];
    else if (a.rfind("--socket=", 0) == 0) socket = a.substr(9);
    else if (a == "--help" || a == "-h") { usage(stdout); return kExitOk; }
    else if (a == "--version") { std::printf("ml-optimizer %s\n", opt::kVersion); return kExitOk; }
    else break;
  }
  if (i >= argc) {
    usage(stderr);
    return kExitUsage;
  }

  opt::ipc::Request req;
  req.command = argv[i++];
  for (; i < argc; ++i) req.args.emplace_back(argv[i]);

  // Inventory and installation are local CLI operations. Installation is
  // limited to built-in feature IDs and requires a real interactive YES.
  if (auto local = handle_local_package_command(req)) return *local;

  // `--watch [SEC]` is a client-side loop; the daemon only ever sees the plain command.
  int watch_sec = 0;
  if ((req.command == "monitor" || req.command == "workload" || req.command == "policy" || req.command == "safety") && !req.args.empty() && (req.args[0] == "--watch" || req.args[0] == "-w")) {
    watch_sec = 2;
    if (req.args.size() > 1) watch_sec = std::max(1, std::atoi(req.args[1].c_str()));
    req.args.clear();
  }
  for (;;) {
    auto resp = opt::ipc::call(socket, req);
  if (!resp) {
    const auto& e = resp.error();
    if (e.code == opt::Errc::InvalidArgument || e.code == opt::Errc::Protocol) {
      std::fprintf(stderr, "ml-optimizer: %s\n", opt::describe(e).c_str());
      return kExitUsage;
    }
    std::fprintf(stderr, "ml-optimizer: %s\n", opt::describe(e).c_str());
    if (e.sys_errno == ENOENT || e.sys_errno == ECONNREFUSED)
      std::fprintf(stderr, "is the daemon running?  systemctl status ml-optimizerd\n");
    else if (e.sys_errno == EACCES)
      std::fprintf(stderr, "no permission to open the control socket\n");
    return kExitNoDaemon;
  }
  if (!resp->ok) {
    std::fprintf(stderr, "ml-optimizer: %s: %s", std::string(opt::to_string(resp->code)).c_str(), resp->body.c_str());
    if (resp->body.empty() || resp->body.back() != '\n') std::fputc('\n', stderr);
    return kExitDaemonError;
  }
  if (watch_sec > 0 && ::isatty(STDOUT_FILENO)) std::fputs("\033[H\033[2J", stdout);  // clear screen
  std::fputs(resp->body.c_str(), stdout);
  if (watch_sec == 0) return kExitOk;
  std::fflush(stdout);
  std::this_thread::sleep_for(std::chrono::seconds(watch_sec));
  }
}
