#include "control/gpu.hpp"

#include "core/sysfs.hpp"

namespace opt::control {
namespace fs = std::filesystem;
using namespace sysfs;
using policy::Bias;

// Intel i915 only (RPS min/max frequency within the hardware RPn..RP0 range).
// Other drivers are reported as having no controller, never guessed at.
std::vector<Desired> plan_gpu(const ControlContext& c) {
  std::vector<Desired> out;
  const auto& range = c.limits.gpu_mhz;
  if (!range) return out;
  const auto& t = c.decision.targets;
  const bool emergency = c.decision.profile == Profile::ThermalEmergency;
  if (t.gpu == Bias::Balanced && !emergency) return out;
  const long long span = range->max - range->min;

  for (const auto& card : list_dir(c.root / "sys/class/drm", "card")) {
    const std::string name = card.filename().string();
    if (!all_digits(name.substr(4))) continue;
    std::error_code ec;
    if (fs::read_symlink(card / "device/driver", ec).filename() != "i915") continue;

    // Legacy and per-GT sysfs layouts.
    for (const char* layout : {"gt_%s_freq_mhz", "gt/gt0/rps_%s_freq_mhz"}) {
      auto path = [&](const char* which) {
        std::string rel = layout;
        rel.replace(rel.find("%s"), 2, which);
        return rel;
      };
      const std::string max_rel = path("max"), min_rel = path("min");
      if (!path_exists(card / max_rel) || !path_exists(card / min_rel)) continue;
      auto add = [&](const std::string& rel, long long mhz, bool raises, const std::string& why, bool safety) {
        Desired d;
        d.controller = "gpu";
        d.subsystem = policy::Subsystem::Gpu;
        d.target = "/sys/class/drm/" + name + "/" + rel;
        d.value = std::to_string(mhz);
        d.raises_performance = raises;
        d.limit_kind = safety::LimitKind::GpuFreqMhz;
        d.numeric = mhz;
        d.safety_action = safety;
        d.reason = why;
        out.push_back(std::move(d));
      };
      if (emergency) add(max_rel, range->min + span / 4, false, "THERMAL EMERGENCY: GPU max at 25% of range", true);
      else if (t.gpu == Bias::Power) add(max_rel, range->min + span / 2, false, "GPU power bias: max at 50% of range", false);
      else if (t.gpu == Bias::Performance) add(min_rel, range->min + span / 2, true, "GPU performance bias: min at 50% of range", false);
      break;  // first layout that exists
    }
  }
  return out;
}

}  // namespace opt::control
