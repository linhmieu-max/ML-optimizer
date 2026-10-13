#pragma once

#include <string_view>

namespace opt {

/// Profiles from the design spec. Phase 1 only defines the vocabulary.
enum class Profile { Powersave, Balanced, Performance, ThermalEmergency };

/// Thermal safety state machine states (implemented in Phase 5).
enum class SafetyState { Normal, Warm, Hot, Emergency };

/// Result of probing a kernel/hardware interface.
///  Available   - interface present and usable
///  Unavailable - not present on this machine (hardware/driver/config)
///  Unsupported - kernel cannot provide it (too old, feature compiled out)
///  Unknown     - probe could not decide
enum class CapStatus { Available, Unavailable, Unsupported, Unknown };

inline constexpr int kAggressivenessMin = 1;
inline constexpr int kAggressivenessMax = 4;
inline constexpr int kAggressivenessDefault = 3;

constexpr std::string_view to_string(Profile p) noexcept {
  switch (p) {
    case Profile::Powersave: return "POWERSAVE";
    case Profile::Balanced: return "BALANCED";
    case Profile::Performance: return "PERFORMANCE";
    case Profile::ThermalEmergency: return "THERMAL_EMERGENCY";
  }
  return "UNKNOWN";
}

constexpr std::string_view to_string(SafetyState s) noexcept {
  switch (s) {
    case SafetyState::Normal: return "NORMAL";
    case SafetyState::Warm: return "WARM";
    case SafetyState::Hot: return "HOT";
    case SafetyState::Emergency: return "EMERGENCY";
  }
  return "UNKNOWN";
}

constexpr std::string_view to_string(CapStatus s) noexcept {
  switch (s) {
    case CapStatus::Available: return "available";
    case CapStatus::Unavailable: return "unavailable";
    case CapStatus::Unsupported: return "unsupported";
    case CapStatus::Unknown: return "unknown";
  }
  return "unknown";
}

constexpr std::string_view aggressiveness_name(int level) noexcept {
  switch (level) {
    case 1: return "conservative";
    case 2: return "balanced";
    case 3: return "aggressive";
    case 4: return "experimental";
  }
  return "invalid";
}

}  // namespace opt
