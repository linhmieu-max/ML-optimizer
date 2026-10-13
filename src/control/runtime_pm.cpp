#include "control/runtime_pm.hpp"

#include "core/sysfs.hpp"

namespace opt::control {
using namespace sysfs;

// OPT-IN ([controllers] runtime_pm = on), battery only. Blanket "auto" on every device is a
// known source of glitches (USB peripherals, audio pops, NVMe/Wi-Fi latency), so only PCI
// display (0x03), bridge (0x06), system (0x08) and signal-processing (0x11) functions that
// are "on" (or already "auto") are proposed.
std::vector<Desired> plan_runtime_pm(const ControlContext& c) {
  std::vector<Desired> out;
  if (!c.runtime_pm_enabled || c.decision.targets.runtime_pm != policy::RuntimePm::PowerSaving) return out;
  if (!(c.sys && c.sys->power && c.sys->power->on_ac == false)) return out;
  for (const auto& dev : list_dir(c.root / "sys/bus/pci/devices")) {
    const auto cls = read_text(dev / "class");
    if (!cls || cls->size() < 4) continue;
    const std::string major = cls->substr(0, 4);  // "0x03"
    if (major != "0x03" && major != "0x06" && major != "0x08" && major != "0x11") continue;
    // "on" is a pending change; "auto" is either already satisfied or a value we set earlier. Either way the
    // target stays in the desired set, otherwise our own change would look "no longer wanted" and be reverted.
    const auto ctl = read_text(dev / "power/control");
    if (ctl != "on" && ctl != "auto") continue;
    Desired d;
    d.controller = "runtime-pm";
    d.subsystem = policy::Subsystem::RuntimePm;
    d.target = "/sys/bus/pci/devices/" + dev.filename().string() + "/power/control";
    d.value = "auto";
    d.reason = "power saving on battery: runtime PM for PCI class " + major;
    out.push_back(std::move(d));
  }
  return out;
}

}  // namespace opt::control
