#pragma once
// The single chokepoint every future system change must pass (spec: policy ->
// capability -> conflict -> thermal safety -> controller). Pure decision logic.

#include <optional>
#include <string>
#include <string_view>

#include "core/types.hpp"
#include "policy/policy_engine.hpp"
#include "safety/hardware_limits.hpp"

namespace opt::safety {

enum class GateStage { Allowed, Target, Policy, Capability, Conflict, Thermal, Limits };
constexpr std::string_view to_string(GateStage s) noexcept {
  switch (s) {
    case GateStage::Allowed: return "allowed";
    case GateStage::Target: return "target";
    case GateStage::Policy: return "policy";
    case GateStage::Capability: return "capability";
    case GateStage::Conflict: return "conflict";
    case GateStage::Thermal: return "thermal";
    case GateStage::Limits: return "limits";
  }
  return "?";
}

struct Verdict {
  bool allowed = false;
  GateStage stage = GateStage::Policy;
  std::string reason;
};

struct ChangeIntent {
  policy::Subsystem subsystem = policy::Subsystem::Cpu;
  std::string target;                    // absolute path of the file to write
  std::string value;
  bool raises_performance = false;       // would increase power/performance/boost
  std::optional<LimitKind> limit_kind;   // numeric values are range-checked
  std::optional<long long> numeric;
  /// A protective action by the thermal safety layer: may lower performance even on a subsystem
  /// another tool owns. It can never raise performance and still passes allowlist + hardware limits.
  bool safety_action = false;
};

struct GateContext {
  const policy::PolicyDecision* decision = nullptr;  // current advisory decision (required)
  SafetyState safety = SafetyState::Normal;
  const HardwareLimits* limits = nullptr;
};

/// Lexical allowlist of writable targets (no "..", no "//", absolute). The Phase 6
/// writer must additionally resolve symlinks and re-check the resolved path.
bool is_allowed_target(std::string_view path);

Verdict check_change(const ChangeIntent&, const GateContext&);

}  // namespace opt::safety
