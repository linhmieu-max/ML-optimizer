#pragma once

#include <optional>
#include <string_view>

#include "core/types.hpp"
#include "policy/targets.hpp"
#include "workload/types.hpp"

namespace opt::policy {

/// Fixed targets of a profile (what a locked profile uses, unmodified).
Targets base_targets(Profile);

/// Workload-specific refinement of a profile's targets (never raises the CPU bias
/// above what the profile allows, except through the profile itself).
void tune_for_workload(Targets&, workload::WorkloadClass, unsigned flags, int aggressiveness);

/// "powersave" | "balanced" | "performance" (THERMAL_EMERGENCY is not user-selectable).
std::optional<Profile> parse_user_profile(std::string_view);
std::string_view user_name(Profile);

/// Performance -> Balanced -> Powersave; ThermalEmergency is unchanged.
Profile step_down(Profile, int steps);

}  // namespace opt::policy
