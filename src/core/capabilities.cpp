#include "core/capabilities.hpp"

#include "core/sysfs.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

namespace opt {
namespace fs = std::filesystem;
namespace {

constexpr long kCgroup2Magic = 0x63677270;

using sysfs::all_digits;
using sysfs::list_dir;
using sysfs::path_exists;
using sysfs::read_text;

std::string join(const std::vector<std::string>& v, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

void add(CapabilityReport& r, std::string id, CapStatus s, std::string detail) {
  r.add(Capability{std::move(id), s, std::move(detail)});
}

// ---- individual probes -----------------------------------------------------

void probe_kernel(CapabilityReport& r) {
  if (r.kernel_major == 0) {
    add(r, "kernel", CapStatus::Unknown, "cannot parse release '" + r.kernel_release + "'");
  } else if (r.kernel_major < 4) {
    add(r, "kernel", CapStatus::Unsupported, r.kernel_release + " (< 4.x)");
  } else {
    add(r, "kernel", CapStatus::Available, r.kernel_release);
  }
}

void probe_cpu(const fs::path& root, CapabilityReport& r) {
  int cpus = 0;
  for (const auto& p : list_dir(root / "sys/devices/system/cpu", "cpu"))
    if (all_digits(p.filename().string().substr(3))) ++cpus;
  if (cpus == 0) cpus = static_cast<int>(std::thread::hardware_concurrency());

  std::string vendor = "unknown-vendor";
  if (auto info = read_text(root / "proc/cpuinfo", 16384)) {
    const auto pos = info->find("vendor_id");
    if (pos != std::string::npos) {
      const auto colon = info->find(':', pos);
      const auto eol = info->find('\n', pos);
      if (colon != std::string::npos && colon < eol) {
        vendor = info->substr(colon + 1, eol - colon - 1);
        vendor.erase(0, vendor.find_first_not_of(" \t"));
      }
    }
  }
  utsname u{};
  const std::string arch = ::uname(&u) == 0 ? u.machine : "unknown-arch";
  add(r, "cpu.topology", CapStatus::Available,
      vendor + " " + arch + ", " + std::to_string(cpus) + " logical CPUs");
}

void probe_cpufreq(const fs::path& root, CapabilityReport& r) {
  const fs::path cpu = root / "sys/devices/system/cpu";
  fs::path policy = cpu / "cpufreq/policy0";
  if (!path_exists(policy)) policy = cpu / "cpu0/cpufreq";

  if (!path_exists(policy)) {
    add(r, "cpufreq", CapStatus::Unavailable, "no cpufreq policy exposed");
    add(r, "cpufreq.epp", CapStatus::Unavailable, "requires cpufreq");
  } else {
    const auto driver = read_text(policy / "scaling_driver").value_or("?");
    const auto gov = read_text(policy / "scaling_governor").value_or("?");
    const auto avail = read_text(policy / "scaling_available_governors").value_or("?");
    add(r, "cpufreq", CapStatus::Available,
        "driver=" + driver + " governor=" + gov + " available=[" + avail + "]");
    if (auto epp = read_text(policy / "energy_performance_available_preferences")) {
      add(r, "cpufreq.epp", CapStatus::Available,
          "current=" + read_text(policy / "energy_performance_preference").value_or("?") +
              " available=[" + *epp + "]");
    } else {
      add(r, "cpufreq.epp", CapStatus::Unavailable, "energy_performance_preference not exposed");
    }
  }

  if (auto nt = read_text(cpu / "intel_pstate/no_turbo")) {
    add(r, "cpu.boost", CapStatus::Available,
        "intel_pstate no_turbo=" + *nt + (*nt == "0" ? " (turbo enabled)" : " (turbo disabled)"));
  } else if (auto b = read_text(cpu / "cpufreq/boost")) {
    add(r, "cpu.boost", CapStatus::Available, "cpufreq boost=" + *b);
  } else {
    add(r, "cpu.boost", CapStatus::Unavailable, "no boost control interface");
  }
}

void probe_uclamp(const fs::path& root, CapabilityReport& r) {
  if (!r.kernel_at_least(5, 3)) {
    add(r, "sched.uclamp", CapStatus::Unsupported, "kernel < 5.3");
  } else if (path_exists(root / "proc/sys/kernel/sched_util_clamp_min")) {
    add(r, "sched.uclamp", CapStatus::Available, "sched_util_clamp sysctls present");
  } else {
    add(r, "sched.uclamp", CapStatus::Unsupported,
        "sched_util_clamp sysctls missing (CONFIG_UCLAMP_TASK not set)");
  }
}

void probe_psi(const fs::path& root, CapabilityReport& r) {
  if (!r.kernel_at_least(4, 20)) {
    add(r, "psi", CapStatus::Unsupported, "kernel < 4.20");
    return;
  }
  std::vector<std::string> ok;
  bool present = false;
  for (const char* res : {"cpu", "memory", "io"}) {
    const fs::path p = root / "proc/pressure" / res;
    if (!path_exists(p)) continue;
    present = true;
    if (read_text(p)) ok.push_back(res);
  }
  if (!ok.empty()) add(r, "psi", CapStatus::Available, join(ok, ", "));
  else if (present) add(r, "psi", CapStatus::Unavailable, "present but unreadable (psi=0 on cmdline?)");
  else add(r, "psi", CapStatus::Unavailable, "/proc/pressure missing (CONFIG_PSI not set?)");
}

void probe_cgroup(const fs::path& root, CapabilityReport& r) {
  const fs::path cg = root / "sys/fs/cgroup";
  struct statfs sfs{};
  if (::statfs(cg.c_str(), &sfs) == 0 && static_cast<long>(sfs.f_type) == kCgroup2Magic) {
    add(r, "cgroup.v2", CapStatus::Available,
        "controllers: " + read_text(cg / "cgroup.controllers").value_or("?"));
  } else {
    add(r, "cgroup.v2", CapStatus::Unavailable, "unified cgroup v2 hierarchy not mounted at /sys/fs/cgroup");
  }
}

void probe_thermal(const fs::path& root, CapabilityReport& r) {
  const fs::path th = root / "sys/class/thermal";
  const auto zones = list_dir(th, "thermal_zone");
  const auto coolers = list_dir(th, "cooling_device");
  if (zones.empty()) {
    add(r, "thermal", CapStatus::Unavailable, "no thermal zones");
    return;
  }
  int with_trips = 0;
  for (const auto& z : zones)
    if (path_exists(z / "trip_point_0_temp")) ++with_trips;
  add(r, "thermal", CapStatus::Available,
      std::to_string(zones.size()) + " zones (" + std::to_string(with_trips) + " with trip points), " +
          std::to_string(coolers.size()) + " cooling devices");
}

void probe_power(const fs::path& root, CapabilityReport& r) {
  int mains = 0, battery = 0, other = 0;
  for (const auto& p : list_dir(root / "sys/class/power_supply")) {
    const auto type = read_text(p / "type").value_or("");
    if (type == "Mains") ++mains;
    else if (type == "Battery") ++battery;
    else ++other;
  }
  if (mains + battery + other == 0) {
    add(r, "power", CapStatus::Unavailable, "no power_supply devices");
  } else {
    add(r, "power", CapStatus::Available,
        std::to_string(mains) + " mains, " + std::to_string(battery) + " battery, " +
            std::to_string(other) + " other");
  }
}

void probe_rapl(const fs::path& root, CapabilityReport& r) {
  const auto doms = list_dir(root / "sys/class/powercap", "intel-rapl:");
  int pkgs = 0;
  bool readable = false;
  for (const auto& d : doms) {
    const std::string n = d.filename().string();
    if (n.find(':', 11) != std::string::npos) continue;  // sub-domain (core/uncore/dram)
    ++pkgs;
    if (read_text(d / "energy_uj")) readable = true;
  }
  if (pkgs == 0) add(r, "power.rapl", CapStatus::Unavailable, "no powercap RAPL package domain");
  else if (!readable) add(r, "power.rapl", CapStatus::Unavailable, "energy_uj not readable (needs root)");
  else add(r, "power.rapl", CapStatus::Available, std::to_string(pkgs) + " package domain(s)");
}

void probe_workload_inputs(const fs::path& root, CapabilityReport& r) {
  if (read_text(root / "proc/self/stat")) add(r, "proc.scan", CapStatus::Available, "/proc readable");
  else add(r, "proc.scan", CapStatus::Unavailable, "/proc/self/stat unreadable");
  add(r, "audio.alsa", path_exists(root / "proc/asound") ? CapStatus::Available : CapStatus::Unavailable,
      path_exists(root / "proc/asound") ? "ALSA proc interface present (playback detection)" : "no /proc/asound");
  add(r, "net.listen", read_text(root / "proc/net/tcp") ? CapStatus::Available : CapStatus::Unavailable,
      "/proc/net/tcp (listening socket count)");
}

void probe_gpu(const fs::path& root, CapabilityReport& r) {
  std::vector<std::string> cards;
  for (const auto& p : list_dir(root / "sys/class/drm", "card")) {
    const std::string name = p.filename().string();
    if (!all_digits(name.substr(4))) continue;  // skip connectors like card0-eDP-1
    std::error_code ec;
    const fs::path drv = fs::read_symlink(p / "device/driver", ec);
    cards.push_back(name + "=" + (ec ? std::string("unknown") : drv.filename().string()));
  }
  if (cards.empty()) add(r, "gpu.drm", CapStatus::Unavailable, "no DRM devices");
  else add(r, "gpu.drm", CapStatus::Available, join(cards, ", "));

  std::vector<std::string> dev;
  for (const auto& p : list_dir(root / "sys/class/devfreq")) dev.push_back(p.filename().string());
  if (dev.empty()) add(r, "gpu.devfreq", CapStatus::Unavailable, "no devfreq devices");
  else add(r, "gpu.devfreq", CapStatus::Available, join(dev, ", "));
}

void probe_memory(const fs::path& root, CapabilityReport& r) {
  int swaps = 0;
  if (auto s = read_text(root / "proc/swaps")) {
    swaps = static_cast<int>(std::count(s->begin(), s->end(), '\n'));  // header excluded
  }
  if (swaps > 0) add(r, "mem.swap", CapStatus::Available, std::to_string(swaps) + " active swap area(s)");
  else add(r, "mem.swap", CapStatus::Unavailable, "no active swap");

  const auto z = list_dir(root / "sys/block", "zram");
  if (z.empty()) add(r, "mem.zram", CapStatus::Unavailable, "no zram devices");
  else add(r, "mem.zram", CapStatus::Available, std::to_string(z.size()) + " zram device(s)");
}

void probe_io(const fs::path& root, CapabilityReport& r) {
  std::vector<std::string> devs;
  for (const auto& p : list_dir(root / "sys/block")) {
    auto sched = read_text(p / "queue/scheduler");
    if (!sched) continue;
    std::string cur = "?";
    const auto a = sched->find('['), b = sched->find(']');
    if (a != std::string::npos && b != std::string::npos && b > a) cur = sched->substr(a + 1, b - a - 1);
    devs.push_back(p.filename().string() + "=" + cur);
  }
  if (devs.empty()) add(r, "io.scheduler", CapStatus::Unavailable, "no block device exposes queue/scheduler");
  else add(r, "io.scheduler", CapStatus::Available, join(devs, ", "));
}

}  // namespace

const Capability* CapabilityReport::find(std::string_view id) const noexcept {
  for (const auto& c : caps_)
    if (c.id == id) return &c;
  return nullptr;
}

bool CapabilityReport::available(std::string_view id) const noexcept {
  const Capability* c = find(id);
  return c && c->status == CapStatus::Available;
}

bool CapabilityReport::kernel_at_least(int major, int minor) const noexcept {
  return kernel_major > major || (kernel_major == major && kernel_minor >= minor);
}

CapabilityReport detect_capabilities(const fs::path& root, std::string kernel_release) {
  CapabilityReport r;
  if (kernel_release.empty()) {
    utsname u{};
    if (::uname(&u) == 0) kernel_release = u.release;
  }
  r.kernel_release = kernel_release;
  std::sscanf(kernel_release.c_str(), "%d.%d", &r.kernel_major, &r.kernel_minor);

  probe_kernel(r);
  probe_cpu(root, r);
  probe_cpufreq(root, r);
  probe_uclamp(root, r);
  probe_psi(root, r);
  probe_cgroup(root, r);
  probe_memory(root, r);
  probe_io(root, r);
  probe_thermal(root, r);
  probe_power(root, r);
  probe_rapl(root, r);
  probe_workload_inputs(root, r);
  probe_gpu(root, r);
  add(r, "init.systemd", path_exists(root / "run/systemd/system") ? CapStatus::Available : CapStatus::Unavailable,
      path_exists(root / "run/systemd/system") ? "systemd is the running init" : "systemd is not the running init");
  return r;
}

std::string format_report(const CapabilityReport& r) {
  auto pad = [](std::string s, std::size_t w) {
    if (s.size() < w) s.append(w - s.size(), ' ');
    return s;
  };
  std::string out;
  for (const auto& c : r.all()) {
    out += "  " + pad(c.id, 15) + " " + pad(std::string(to_string(c.status)), 12) + " " + c.detail + "\n";
  }
  return out;
}

}  // namespace opt
