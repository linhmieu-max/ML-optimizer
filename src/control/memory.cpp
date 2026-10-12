#include "control/memory.hpp"

#include "core/sysfs.hpp"

namespace opt::control {
using namespace sysfs;

// Responsiveness: lower vm.swappiness to 30 when it is higher.
// Keep the target managed while the policy remains active, even when
// the current value is already <= 30. This prevents ControlManager
// from treating an already-satisfied target as stale and rolling it back.
// Never increase swappiness when it is already <= 30.
// Do not modify swappiness when swap is unavailable or memory pressure is high.
//
// Throughput: hands off (dirty ratios are data-loss relevant and are
// deliberately not touched).
std::vector<Desired> plan_memory(const ControlContext& c) {
    std::vector<Desired> out;
    const auto& d = c.decision;

    if (d.targets.memory != policy::MemoryBias::Responsiveness ||
        d.pressure.memory) {
        return out;
    }

    if (!c.caps.available("mem.swap")) {
        return out;
    }

    const auto cur = read_int(c.root / "proc/sys/vm/swappiness");
    if (!cur) {
        return out;
    }

    Desired x;
    x.controller = "memory";
    x.subsystem = policy::Subsystem::Memory;
    x.target = "/proc/sys/vm/swappiness";

    if (*cur > 30) {
        x.value = "30";
        x.reason = "favour responsiveness: swappiness " +
                   std::to_string(*cur) + " -> 30";
    } else {
        // Preserve ownership/intent without changing a value that is
        // already at or below 30. ControlManager will see the target
        // as wanted, and its equality check will prevent a write.
        x.value = std::to_string(*cur);
        x.reason = "retain current swappiness within responsiveness limit";
    }

    out.push_back(std::move(x));
    return out;
}

}  // namespace opt::control
