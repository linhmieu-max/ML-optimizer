#pragma once
// Fixed-layout numeric features (all in [0, 1]). Only aggregates are used: process NAMES are
// reduced to category CPU shares and never leave this function.

#include <array>
#include <optional>

#include "core/types.hpp"
#include "monitor/system_monitor.hpp"
#include "workload/process_scanner.hpp"
#include "workload/types.hpp"

namespace opt::ml {

inline constexpr int kFeatureVersion = 1;  // bump when the layout or scaling changes: cached models are rejected

enum Feature : int {
  F_CPU_UTIL, F_CPU_FREQ_RATIO, F_LOAD, F_RAM_USED, F_SWAP, F_PSI_CPU, F_PSI_MEM, F_PSI_IO, F_IO_READ, F_IO_WRITE,
  F_GPU_BUSY, F_AUDIO, F_COMPOSITOR, F_SESSION, F_SYSTEM, F_TOP_PROC, F_ACTIVE_PROCS, F_LISTEN_PORTS,
  F_BROWSER_CPU, F_COMPILE_CPU, F_COMPILE_WORKERS, F_COMPILE_ORCH, F_PLAYER_PRESENT, F_PLAYER_CPU, F_CONTAINER_CPU,
  F_VM_CPU, F_SERVER_CPU, F_ARCHIVER_CPU, F_ENCODER_CPU, F_RENDER_CPU, F_COPY_TOOL, F_GAME_CPU, F_LAUNCHER, F_BROWSER_PROCS,
  kFeatureCount
};

using FeatureVector = std::array<float, kFeatureCount>;
const char* feature_name(int index);

struct FeatureEnv {
  std::optional<double> cpu_hw_max_mhz;
};

FeatureVector extract_features(const workload::ProcessSnapshot&, const monitor::SystemSnapshot*, const FeatureEnv&);

// ---- features of the profile model (it distils the rule table, then adapts) ----
inline constexpr int kProfileFeatureCount = 23;
using ProfileFeatures = std::array<float, kProfileFeatureCount>;

struct ProfileFeatureInput {
  workload::WorkloadClass cls = workload::WorkloadClass::Idle;
  double confidence = 0;
  unsigned flags = 0;
  std::optional<bool> on_ac;
  std::optional<int> battery_pct;
  std::optional<double> thermal_margin_c;
  double psi_cpu10 = 0;
  double cpu_util = 0;
  int aggressiveness = kAggressivenessDefault;
};

ProfileFeatures profile_features(const ProfileFeatureInput&);

}  // namespace opt::ml
