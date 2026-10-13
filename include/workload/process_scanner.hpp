#pragma once
// Read-only /proc scanner. Privacy: command lines are inspected only for a few
// fixed markers (flags) and are never stored; only process names are kept.

#include <filesystem>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

#include "core/result.hpp"
#include "monitor/types.hpp"

namespace opt::workload {

using monitor::Clock;

enum CmdFlag : unsigned {
  kCmdSteamApp = 1,         // path contains steamapps/common or SteamLibrary
  kCmdProton = 2,           // proton marker
  kCmdChromiumRenderer = 4, // --type=renderer
  kCmdChromiumGpu = 8,      // --type=gpu-process
};

struct ProcessInfo {
  int pid = 0, ppid = 0;
  std::string comm;    // lower-case, as in /proc/pid/stat (15 chars max)
  std::string exe;     // lower-case basename of /proc/pid/exe ("" if unreadable/kernel thread)
  std::string cgroup;  // cgroup path (v2 "0::" entry or first v1 line)
  char state = '?';
  int threads = 1;
  uid_t uid = 0;
  std::uint64_t rss_kb = 0;
  unsigned cmd_flags = 0;
  std::optional<double> cpu_pct;  // of one core; nullopt on first sight
};

struct ProcessSnapshot {
  std::vector<ProcessInfo> procs;
  std::unordered_map<int, std::size_t> index;
  int listen_ports_nonlocal = 0;         // TCP LISTEN sockets not bound to loopback
  std::optional<bool> audio_running;     // ALSA playback substream in RUNNING state

  void finalize();
  const ProcessInfo* find(int pid) const;
  /// True if any ancestor (up to 16 levels) satisfies `pred`.
  template <class Pred>
  bool has_ancestor(const ProcessInfo& p, Pred pred) const {
    int pid = p.ppid;
    for (int depth = 0; depth < 16 && pid > 1; ++depth) {
      const ProcessInfo* a = find(pid);
      if (!a) return false;
      if (pred(*a)) return true;
      pid = a->ppid;
    }
    return false;
  }
};

class ProcessScanner {
 public:
  /// clk_tck = 0 -> sysconf(_SC_CLK_TCK). `root` re-bases /proc for tests.
  explicit ProcessScanner(std::filesystem::path root = "/", long clk_tck = 0);
  Result<ProcessSnapshot> scan(Clock::time_point now = Clock::now());

 private:
  struct Static {
    std::uint64_t start = 0;
    std::string exe, cgroup;
    unsigned flags = 0;
    uid_t uid = 0;
  };
  struct Ticks {
    std::uint64_t start = 0, ticks = 0;
  };
  std::filesystem::path root_;
  double clk_tck_;
  std::uint64_t page_kb_;
  std::unordered_map<int, Static> static_;
  std::unordered_map<int, Ticks> prev_;
  std::optional<Clock::time_point> prev_at_;
};

}  // namespace opt::workload
