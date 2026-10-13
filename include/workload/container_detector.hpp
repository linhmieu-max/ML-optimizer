#pragma once
#include "workload/detector_common.hpp"
namespace opt::workload {
/// Based on cgroup membership (docker/podman/crio/nspawn scopes), not on names.
Evidence detect_container(const DetectContext&);
}
