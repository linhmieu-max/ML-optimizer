#pragma once
// The only place that lists well-known program names. Matching is done on the
// lower-case comm (15 chars, from /proc/pid/stat) and exe basename.

#include "workload/process_scanner.hpp"

namespace opt::workload {

bool is_compositor(const ProcessInfo&);
bool is_browser(const ProcessInfo&);
bool is_compile_worker(const ProcessInfo&);
bool is_compile_orchestrator(const ProcessInfo&);
bool is_render(const ProcessInfo&);
bool is_encoder(const ProcessInfo&);
bool is_archiver(const ProcessInfo&);
bool is_copy_tool(const ProcessInfo&);
bool is_vm(const ProcessInfo&);
bool is_media_player(const ProcessInfo&);
bool is_server_daemon(const ProcessInfo&);
bool is_launcher(const ProcessInfo&);        // steam, lutris, gamescope, pressure-vessel...
bool is_emulator(const ProcessInfo&);
bool is_wine_game_candidate(const ProcessInfo&);  // "*.exe" that is not a Wine system helper
bool is_container_runtime(const ProcessInfo&);
bool in_container_cgroup(const ProcessInfo&);
bool in_system_slice(const ProcessInfo&);
bool in_user_slice(const ProcessInfo&);

}  // namespace opt::workload
