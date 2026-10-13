#pragma once
// What the ML layer hands to the policy engine. It is a SUGGESTION only: the engine
// validates it (mode, safety state, confidence, step size, power source) and the result
// still goes through the safety gate before any controller acts.

#include <array>
#include <string>

#include "core/types.hpp"

namespace opt::ml {

struct MlAdvice {
  bool available = false;                 // models loaded and a prediction exists
  Profile suggested = Profile::Balanced;  // POWERSAVE / BALANCED / PERFORMANCE only
  double confidence = 0;                  // EFFECTIVE confidence: probability x measured agreement with the rules
  std::array<double, 3> probs{};          // powersave, balanced, performance
  std::string note;
};

}  // namespace opt::ml
