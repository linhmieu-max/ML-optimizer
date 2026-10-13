#pragma once
#include "workload/detector_common.hpp"
namespace opt::workload {
// Name + resource based rules for the remaining classes.
Evidence detect_render(const DetectContext&);
Evidence detect_encode(const DetectContext&);
Evidence detect_archive(const DetectContext&);
Evidence detect_file_copy(const DetectContext&);
Evidence detect_vm(const DetectContext&);
Evidence detect_server(const DetectContext&);
Evidence detect_video(const DetectContext&);
/// DESKTOP / BACKGROUND / IDLE baselines (always computed).
Evidence detect_desktop(const DetectContext&);
Evidence detect_background(const DetectContext&);
Evidence detect_idle(const DetectContext&);
}  // namespace opt::workload
