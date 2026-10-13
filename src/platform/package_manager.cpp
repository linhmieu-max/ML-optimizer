#include "platform/package_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <initializer_list>
#include <poll.h>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace opt::platform {
namespace fs = std::filesystem;
namespace {

std::string trim(std::string s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

std::string unquote(std::string value) {
  value = trim(std::move(value));
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\'')))
    return value.substr(1, value.size() - 2);
  return value;
}

std::string read_os_release_value(const fs::path& file, std::string_view wanted) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos || line.substr(0, eq) != wanted) continue;
    return unquote(line.substr(eq + 1));
  }
  return {};
}

bool contains_word(std::string_view list, std::string_view value) {
  std::istringstream in{std::string(list)};
  std::string word;
  while (in >> word)
    if (word == value) return true;
  return false;
}

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> out;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) out.push_back(std::move(line));
  return out;
}

std::string package_field(std::string_view paragraph, std::string_view key) {
  const std::string prefix = std::string(key) + ": ";
  std::istringstream in{std::string(paragraph)};
  std::string line;
  while (std::getline(in, line))
    if (line.starts_with(prefix)) return line.substr(prefix.size());
  return {};
}

std::set<std::string> dpkg_installed(const fs::path& status_file) {
  std::set<std::string> out;
  std::ifstream in(status_file);
  if (!in) return out;
  std::string paragraph, line;
  auto consume = [&] {
    if (paragraph.empty()) return;
    const auto name = package_field(paragraph, "Package");
    const auto status = package_field(paragraph, "Status");
    if (!name.empty() && status == "install ok installed") out.insert(name);
    paragraph.clear();
  };
  while (std::getline(in, line)) {
    if (line.empty()) consume();
    else { paragraph += line; paragraph += '\n'; }
  }
  consume();
  return out;
}

std::set<std::string> pacman_installed(const fs::path& local_db) {
  std::set<std::string> out;
  std::error_code ec;
  if (!fs::is_directory(local_db, ec)) return out;
  for (fs::directory_iterator it(local_db, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    std::ifstream desc(it->path() / "desc");
    std::string line;
    bool want_name = false;
    while (std::getline(desc, line)) {
      if (line == "%NAME%") { want_name = true; continue; }
      if (want_name) { if (!line.empty()) out.insert(line); break; }
    }
  }
  return out;
}

std::optional<fs::path> existing_binary(const fs::path& root, std::initializer_list<std::string_view> paths) {
  // Avoid constructing a temporary vector<string> at each call site. GCC 16
  // diagnosed that initializer-list-to-vector path as -Wfree-nonheap-object.
  for (const auto p : paths) {
    const fs::path full = root / std::string(p);
    struct stat st{};
    if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
      return full;
  }
  return std::nullopt;
}

std::vector<std::string> privilege_prefix(const fs::path& root) {
  std::vector<std::string> argv;
  if (::geteuid() == 0) return argv;

  // Build argv incrementally; do not return initializer-list-built vectors.
  if (auto sudo = existing_binary(root, {"usr/bin/sudo", "bin/sudo"})) {
    argv.reserve(2);
    argv.push_back(sudo->string());
    argv.emplace_back("--");
    return argv;
  }
  if (auto doas = existing_binary(root, {"usr/bin/doas", "bin/doas"})) {
    argv.push_back(doas->string());
    return argv;
  }
  if (auto pkexec = existing_binary(root, {"usr/bin/pkexec", "bin/pkexec"})) {
    argv.push_back(pkexec->string());
    return argv;
  }
  return argv;
}

}  // namespace

std::string_view to_string(PackageManagerKind kind) noexcept {
  switch (kind) {
    case PackageManagerKind::AptGet: return "apt-get";
    case PackageManagerKind::Pacman: return "pacman";
    case PackageManagerKind::Dnf: return "dnf";
    case PackageManagerKind::Unknown: return "unknown";
  }
  return "unknown";
}

fs::path find_executable(const fs::path& root, std::string_view name) {
  const std::string n(name);
  for (const char* dir : {"usr/bin", "usr/sbin", "bin", "sbin", "usr/local/bin", "usr/local/sbin"}) {
    const fs::path candidate = root / dir / n;
    struct stat st{};
    if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
      return candidate;
  }
  return {};
}

bool executable_at(const fs::path& root, std::string_view name) { return !find_executable(root, name).empty(); }

PlatformInfo detect_platform(const fs::path& root) {
  PlatformInfo p;
  const auto os_release = root / "etc/os-release";
  p.id = read_os_release_value(os_release, "ID");
  p.pretty_name = read_os_release_value(os_release, "PRETTY_NAME");
  p.id_like = read_os_release_value(os_release, "ID_LIKE");
  std::transform(p.id.begin(), p.id.end(), p.id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(p.id_like.begin(), p.id_like.end(), p.id_like.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (p.pretty_name.empty()) p.pretty_name = p.id.empty() ? "Unknown Linux distribution" : p.id;

  const bool debian = p.id == "debian" || p.id == "ubuntu" || p.id == "linuxmint" ||
                      contains_word(p.id_like, "debian") || contains_word(p.id_like, "ubuntu");
  const bool arch = p.id == "arch" || p.id == "manjaro" || p.id == "endeavouros" || contains_word(p.id_like, "arch");
  const bool fedora = p.id == "fedora" || p.id == "rhel" || p.id == "centos" || p.id == "rocky" ||
                      p.id == "almalinux" || contains_word(p.id_like, "fedora") || contains_word(p.id_like, "rhel");
  if (debian) {
    p.manager = PackageManagerKind::AptGet;
    p.manager_path = find_executable(root, "apt-get");
  } else if (arch) {
    p.manager = PackageManagerKind::Pacman;
    p.manager_path = find_executable(root, "pacman");
  } else if (fedora) {
    p.manager = PackageManagerKind::Dnf;
    p.manager_path = find_executable(root, "dnf");
  }
  if (p.manager_path.empty()) p.manager = PackageManagerKind::Unknown;
  return p;
}

std::vector<OptionalPackage> optional_packages() {
  return {
      {"sensors", "Additional hardware sensor diagnostics", "lm-sensors", "lm_sensors", "lm_sensors",
       "Provides the `sensors` diagnostic command and extra hwmon visibility. ML Optimizer continues to work without it.", ""},
      {"thermald", "Intel thermald service (optional)", "thermald", "thermald", "thermald",
       "Optional thermal-management daemon; not required by ML Optimizer or its thermal safety layer.",
       "This daemon may overlap with existing thermal/power management. ML Optimizer will not stop or disable either tool."},
  };
}

std::optional<std::string> package_name_for(const OptionalPackage& pkg, PackageManagerKind manager) {
  std::string name;
  switch (manager) {
    case PackageManagerKind::AptGet: name = pkg.package_debian; break;
    case PackageManagerKind::Pacman: name = pkg.package_arch; break;
    case PackageManagerKind::Dnf: name = pkg.package_fedora; break;
    case PackageManagerKind::Unknown: return std::nullopt;
  }
  if (name.empty()) return std::nullopt;
  return name;
}

std::set<std::string> installed_package_names(const PlatformInfo& platform, const fs::path& root) {
  switch (platform.manager) {
    case PackageManagerKind::AptGet: return dpkg_installed(root / "var/lib/dpkg/status");
    case PackageManagerKind::Pacman: return pacman_installed(root / "var/lib/pacman/local");
    case PackageManagerKind::Dnf: {
      std::set<std::string> out;
      if (root != "/") return out;
      const auto rpm = find_executable(root, "rpm");
      if (rpm.empty()) return out;
      const auto result = run_capture({rpm.string(), "-qa", "--qf", "%{NAME}\\n"}, std::chrono::milliseconds(2500), 4 * 1024 * 1024);
      if (!result.launched || result.timed_out || result.exit_code != 0) return out;
      for (auto& line : split_lines(result.output)) if (!trim(line).empty()) out.insert(trim(line));
      return out;
    }
    case PackageManagerKind::Unknown: return {};
  }
  return {};
}

PackageState package_status(std::string_view package, const PlatformInfo& platform, const fs::path& root) {
  if (package.empty() || platform.manager == PackageManagerKind::Unknown) return PackageState::Unknown;
  if (platform.manager == PackageManagerKind::Dnf && root == "/") {
    const auto rpm = find_executable(root, "rpm");
    if (rpm.empty()) return PackageState::Unknown;
    const auto result = run_capture({rpm.string(), "-q", "--quiet", std::string(package)}, std::chrono::milliseconds(1500), 4096);
    if (!result.launched || result.timed_out) return PackageState::Unknown;
    return result.exit_code == 0 ? PackageState::Installed : PackageState::Missing;
  }
  const auto names = installed_package_names(platform, root);
  if (names.empty()) {
    // An empty database may be a real empty set or an inaccessible/unavailable DB.
    std::error_code ec;
    const bool db_exists = platform.manager == PackageManagerKind::AptGet ? fs::exists(root / "var/lib/dpkg/status", ec)
                         : platform.manager == PackageManagerKind::Pacman ? fs::is_directory(root / "var/lib/pacman/local", ec)
                         : false;
    if (!db_exists) return PackageState::Unknown;
  }
  return names.contains(std::string(package)) ? PackageState::Installed : PackageState::Missing;
}

CommandResult run_capture(const std::vector<std::string>& argv, std::chrono::milliseconds timeout, std::size_t max_output) {
  CommandResult out;
  if (argv.empty() || argv.front().empty()) return out;
  std::vector<char*> child_argv;
  child_argv.reserve(argv.size() + 1);
  for (const auto& arg : argv) child_argv.push_back(const_cast<char*>(arg.c_str()));
  child_argv.push_back(nullptr);

  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) return out;
  const int old_flags = ::fcntl(fds[0], F_GETFL, 0);
  if (old_flags >= 0) (void)::fcntl(fds[0], F_SETFL, old_flags | O_NONBLOCK);
  const pid_t pid = ::fork();
  if (pid < 0) { ::close(fds[0]); ::close(fds[1]); return out; }
  if (pid == 0) {
    ::close(fds[0]);
    (void)::dup2(fds[1], STDOUT_FILENO);
    const int nullfd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (nullfd >= 0) (void)::dup2(nullfd, STDERR_FILENO);
    ::close(fds[1]);
    ::execv(child_argv[0], child_argv.data());
    _exit(127);
  }
  out.launched = true;
  ::close(fds[1]);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool eof = false;
  bool exited = false;
  int status = 0;
  char buf[8192];
  while (!(eof && exited)) {
    while (true) {
      const ssize_t n = ::read(fds[0], buf, sizeof buf);
      if (n > 0) {
        const auto room = max_output > out.output.size() ? max_output - out.output.size() : 0;
        if (room > 0) out.output.append(buf, std::min<std::size_t>(room, static_cast<std::size_t>(n)));
        continue;
      }
      if (n == 0) eof = true;
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) eof = true;
      break;
    }
    if (!exited) {
      const pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid) exited = true;
      else if (w < 0 && errno != EINTR) exited = true;
    }
    if (eof && exited) break;
    if (std::chrono::steady_clock::now() >= deadline) {
      out.timed_out = true;
      (void)::kill(pid, SIGKILL);
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      exited = true;
      // Drain already-buffered output once, then stop.
      const ssize_t n = ::read(fds[0], buf, sizeof buf);
      if (n > 0) {
        const auto room = max_output > out.output.size() ? max_output - out.output.size() : 0;
        if (room > 0) out.output.append(buf, std::min<std::size_t>(room, static_cast<std::size_t>(n)));
      }
      break;
    }
    pollfd fd{fds[0], POLLIN | POLLHUP, 0};
    (void)::poll(&fd, 1, 25);
  }
  ::close(fds[0]);
  if (exited && WIFEXITED(status)) out.exit_code = WEXITSTATUS(status);
  else if (exited && WIFSIGNALED(status)) out.exit_code = 128 + WTERMSIG(status);
  return out;
}

Result<int> install_native_package(const PlatformInfo& platform, std::string_view package) {
  // This API is not an arbitrary package-manager wrapper: caller must pass an
  // allowlisted package id resolved by package_name_for().
  if (package.empty() || package.size() > 64 ||
      !std::all_of(package.begin(), package.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '_' || c == '-';
      }))
    return fail(make_error(Errc::InvalidArgument, "invalid package identifier"));
  const auto offers = optional_packages();
  const bool allowlisted = std::any_of(offers.begin(), offers.end(), [&](const OptionalPackage& offer) {
    const auto allowed_name = package_name_for(offer, platform.manager);
    return allowed_name && *allowed_name == package;
  });
  if (!allowlisted)
    return fail(make_error(Errc::PermissionDenied, "package is not in the built-in optional-package allowlist"));
  if (platform.manager == PackageManagerKind::Unknown || platform.manager_path.empty())
    return fail(make_error(Errc::Unsupported, "no supported native package manager detected (Debian/Ubuntu apt, Arch pacman, Fedora dnf)"));

  std::vector<std::string> argv = privilege_prefix("/");
  if (::geteuid() != 0 && argv.empty())
    return fail(make_error(Errc::PermissionDenied, "no sudo/doas/pkexec found; rerun this command from an authorized root terminal"));
  argv.push_back(platform.manager_path.string());
  switch (platform.manager) {
    case PackageManagerKind::AptGet: argv.insert(argv.end(), {"install", "--no-install-recommends", std::string(package)}); break;
    case PackageManagerKind::Pacman: argv.insert(argv.end(), {"-S", std::string(package)}); break;
    case PackageManagerKind::Dnf: argv.insert(argv.end(), {"install", std::string(package)}); break;
    case PackageManagerKind::Unknown: return fail(make_error(Errc::Unsupported, "unknown package manager"));
  }
  std::vector<char*> args;
  args.reserve(argv.size() + 1);
  for (auto& a : argv) args.push_back(a.data());
  args.push_back(nullptr);
  const pid_t pid = ::fork();
  if (pid < 0) return fail(errno_error(Errc::Io, "cannot fork package manager"));
  if (pid == 0) {
    ::execv(args[0], args.data());
    _exit(127);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    return fail(errno_error(Errc::Io, "waitpid package manager failed"));
  }
  if (!WIFEXITED(status)) return fail(make_error(Errc::Io, "package manager terminated abnormally"));
  return WEXITSTATUS(status);
}

}  // namespace opt::platform
