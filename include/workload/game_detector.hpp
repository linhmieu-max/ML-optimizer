#pragma once
#include "workload/detector_common.hpp"
namespace opt::workload {
/// Never decides from a single signal: needs launcher context, Steam/Proton/Wine
/// game processes, emulators or gamescope, optionally backed by GPU load.
/// A lone GPU-heavy process only yields a low "hint" score (LATENCY_SENSITIVE).
Evidence detect_game(const DetectContext&);
}  // namespace opt::workload
