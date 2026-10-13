#pragma once
#include <vector>

#include "control/types.hpp"

namespace opt::control {
std::vector<Desired> plan_gpu(const ControlContext&);
}  // namespace opt::control
