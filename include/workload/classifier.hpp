#pragma once

#include <string>

#include "workload/detector_common.hpp"
#include "workload/types.hpp"

namespace opt::workload {

struct ClassifierConfig {
  double min_specific_score = 0.35;  // below this a specific class is ignored
  double switch_margin = 0.15;       // challenger must beat the current class by this much...
  int confirm_evaluations = 3;       // ...for this many consecutive evaluations
};

/// Rule-based, explainable classifier with hysteresis. Not thread-safe.
/// The result is advisory input for the policy engine: it never touches the system.
class Classifier {
 public:
  explicit Classifier(ClassifierConfig cfg = {}) : cfg_(cfg) {}
  const WorkloadResult& evaluate(const DetectContext& ctx, Clock::time_point now = Clock::now());
  const WorkloadResult& current() const { return result_; }

 private:
  ClassifierConfig cfg_;
  WorkloadResult result_;
  bool initialized_ = false;
};

std::string format_workload(const WorkloadResult&, Clock::time_point now = Clock::now());

}  // namespace opt::workload
