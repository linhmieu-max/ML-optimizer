#pragma once
// Process-level detection of known external owners. Phase 10 enriches this
// snapshot with package and systemd state; no tool is ever stopped or disabled.

#include <string>
#include <vector>

#include "policy/targets.hpp"
#include "workload/process_scanner.hpp"

namespace opt::safety {

struct ExternalTool {
  std::string name;
  std::string description;
  std::vector<policy::Subsystem> subsystems;  // subsystems it controls
  bool thermal = false;       // also manages thermal (informational)
  bool game_scoped = false;   // only relevant while a game runs (GameMode)
  int pid = 0;
};

std::vector<ExternalTool> detect_running_tools(const workload::ProcessSnapshot&);

}  // namespace opt::safety
