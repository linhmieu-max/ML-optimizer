#pragma once
// Backend-agnostic policy targets. The policy engine decides *what* it wants
// (e.g. CPU bias = Performance); the Phase 6 controllers translate that into
// concrete, capability-checked writes. Nothing in this header touches the system.

#include <string>
#include <string_view>

namespace opt::policy {

enum class Subsystem { Cpu, Gpu, Memory, Io, Background, RuntimePm };
inline constexpr std::size_t kSubsystemCount = 6;

enum class Bias { Power, Balanced, Performance };
enum class IoBias { Latency, Balanced, Throughput };
enum class Restriction { None, Light, Strong };   // background workload restriction
enum class MemoryBias { Default, Responsiveness, Throughput };
enum class RuntimePm { Performance, Auto, PowerSaving };

struct Targets {
  Bias cpu = Bias::Balanced;
  bool allow_boost = true;
  Bias gpu = Bias::Balanced;
  IoBias io = IoBias::Balanced;
  Restriction background = Restriction::None;
  MemoryBias memory = MemoryBias::Default;
  RuntimePm runtime_pm = RuntimePm::Auto;
  bool operator==(const Targets&) const = default;
};

/// Whether the policy may drive a subsystem right now.
///  Active      - policy decides, controller may act (Phase 6)
///  Observe     - someone else owns it (power-profiles-daemon, GameMode...): monitor only
///  Unavailable - the required kernel interface is missing
enum class ControlMode { Active, Observe, Unavailable };

struct SubsystemControl {
  ControlMode mode = ControlMode::Active;
  std::string note;
};

constexpr std::string_view to_string(Subsystem s) noexcept {
  switch (s) {
    case Subsystem::Cpu: return "cpu";
    case Subsystem::Gpu: return "gpu";
    case Subsystem::Memory: return "memory";
    case Subsystem::Io: return "io";
    case Subsystem::Background: return "background";
    case Subsystem::RuntimePm: return "runtime-pm";
  }
  return "?";
}
constexpr std::string_view to_string(Bias b) noexcept {
  return b == Bias::Power ? "power" : b == Bias::Balanced ? "balanced" : "performance";
}
constexpr std::string_view to_string(IoBias b) noexcept {
  return b == IoBias::Latency ? "latency" : b == IoBias::Balanced ? "balanced" : "throughput";
}
constexpr std::string_view to_string(Restriction r) noexcept {
  return r == Restriction::None ? "none" : r == Restriction::Light ? "light" : "strong";
}
constexpr std::string_view to_string(MemoryBias m) noexcept {
  return m == MemoryBias::Default ? "default" : m == MemoryBias::Responsiveness ? "responsiveness" : "throughput";
}
constexpr std::string_view to_string(RuntimePm r) noexcept {
  return r == RuntimePm::Performance ? "performance" : r == RuntimePm::Auto ? "auto" : "power-saving";
}
constexpr std::string_view to_string(ControlMode m) noexcept {
  return m == ControlMode::Active ? "active" : m == ControlMode::Observe ? "observe-only" : "unavailable";
}

}  // namespace opt::policy
