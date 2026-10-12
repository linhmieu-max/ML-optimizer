#include "workload/process_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <sys/stat.h>
#include <unistd.h>

#include "core/sysfs.hpp"

namespace opt::workload {
namespace fs = std::filesystem;
using namespace sysfs;
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

unsigned cmd_flags_from(std::string cmd) {
  cmd = lower(std::move(cmd));
  unsigned f = 0;
  if (cmd.find("steamapps/common") != std::string::npos || cmd.find("steamlibrary") != std::string::npos) f |= kCmdSteamApp;
  if (cmd.find("proton") != std::string::npos) f |= kCmdProton;
  if (cmd.find("--type=renderer") != std::string::npos) f |= kCmdChromiumRenderer;
  if (cmd.find("--type=gpu-process") != std::string::npos) f |= kCmdChromiumGpu;
  return f;
}

std::string cgroup_path(const std::string& text) {
  std::string chosen;
  for (auto line : split_lines(text)) {
    if (line.starts_with("0::")) return std::string(line.substr(3));
    if (chosen.empty()) {
      const auto a = line.find(':');
      const auto b = a == std::string_view::npos ? a : line.find(':', a + 1);
      if (b != std::string_view::npos) chosen = std::string(line.substr(b + 1));
    }
  }
  return chosen;
}

bool is_loopback_v4(std::string_view hex) { return hex.size() == 8 && hex.substr(6) == "7F"; }

int count_listening(const fs::path& file, bool v6) {
  auto t = read_text(file, 1 << 20);
  if (!t) return 0;
  int n = 0;
  bool first = true;
  for (auto line : split_lines(*t)) {
    if (first) { first = false; continue; }  // header
    auto f = split_ws(line);
    if (f.size() < 4 || f[3] != "0A") continue;  // 0A = LISTEN
    const auto colon = f[1].find(':');
    if (colon == std::string_view::npos) continue;
    const auto addr = f[1].substr(0, colon);
    const bool loop = v6 ? addr == "00000000000000000000000001000000" : is_loopback_v4(addr);
    if (!loop) ++n;
  }
  return n;
}

std::optional<bool> alsa_running(const fs::path& root) {
  const fs::path asound = root / "proc/asound";
  if (!path_exists(asound)) return std::nullopt;
  for (const auto& card : list_dir(asound, "card"))
    for (const auto& pcm : list_dir(card, "pcm")) {
      if (pcm.filename().string().back() != 'p') continue;  // playback only
      for (const auto& sub : list_dir(pcm, "sub"))
        if (auto st = read_text(sub / "status", 512); st && st->find("RUNNING") != std::string::npos) return true;
    }
  return false;
}

}  // namespace

void ProcessSnapshot::finalize() {
  index.clear();
  for (std::size_t i = 0; i < procs.size(); ++i) index[procs[i].pid] = i;
}

const ProcessInfo* ProcessSnapshot::find(int pid) const {
  auto it = index.find(pid);
  return it == index.end() ? nullptr : &procs[it->second];
}

ProcessScanner::ProcessScanner(fs::path root, long clk_tck)
    : root_(std::move(root)),
      clk_tck_(static_cast<double>(clk_tck > 0 ? clk_tck : std::max(1L, ::sysconf(_SC_CLK_TCK)))),
      page_kb_(static_cast<std::uint64_t>(std::max(1L, ::sysconf(_SC_PAGESIZE) / 1024))) {}

Result<ProcessSnapshot> ProcessScanner::scan(Clock::time_point now) {
  const fs::path proc = root_ / "proc";
  std::error_code ec;
  fs::directory_iterator it(proc, ec);
  if (ec) return fail(make_error(Errc::Unsupported, "/proc not readable", ec.value()));

  const double dt = prev_at_ ? std::chrono::duration<double>(now - *prev_at_).count() : 0.0;
  ProcessSnapshot snap;
  std::unordered_map<int, Static> next_static;
  std::unordered_map<int, Ticks> next_prev;

  for (; it != fs::directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const std::string name = it->path().filename().string();
    if (!all_digits(name) || name.size() > 9) continue;
    const int pid = std::stoi(name);
    auto stat = read_text(it->path() / "stat", 4096);
    if (!stat) continue;  // exited between readdir and read

    // pid (comm) S ppid ... ; comm may contain spaces and parentheses -> use the last ')'.
    const auto lp = stat->find('('), rp = stat->rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp < lp) continue;
    const auto f = split_ws(std::string_view(*stat).substr(rp + 1));
    if (f.size() < 22) continue;

    ProcessInfo p;
    p.pid = pid;
    p.comm = lower(stat->substr(lp + 1, rp - lp - 1));
    p.state = f[0].empty() ? '?' : f[0][0];
    p.ppid = static_cast<int>(parse_u64(f[1]).value_or(0));
    const std::uint64_t ticks = parse_u64(f[11]).value_or(0) + parse_u64(f[12]).value_or(0);
    p.threads = static_cast<int>(parse_u64(f[17]).value_or(1));
    const std::uint64_t start = parse_u64(f[19]).value_or(0);
    p.rss_kb = parse_u64(f[21]).value_or(0) * page_kb_;

    Static st;
    if (auto c = static_.find(pid); c != static_.end() && c->second.start == start) {
      st = c->second;
    } else {
      st.start = start;
      std::error_code lec;
      std::string exe = fs::read_symlink(it->path() / "exe", lec).filename().string();
      if (exe.ends_with(" (deleted)")) exe.resize(exe.size() - 10);
      st.exe = lower(exe);
      if (!lec) {
        if (auto cmd = read_text(it->path() / "cmdline", 4096)) st.flags = cmd_flags_from(*cmd);
      }
      if (auto cg = read_text(it->path() / "cgroup", 1024)) st.cgroup = cgroup_path(*cg);
      struct stat sb{};
      if (::stat(it->path().c_str(), &sb) == 0) st.uid = sb.st_uid;
    }
    p.exe = st.exe;
    p.cgroup = st.cgroup;
    p.cmd_flags = st.flags;
    p.uid = st.uid;

    if (auto pv = prev_.find(pid); pv != prev_.end() && pv->second.start == start && dt > 0 && ticks >= pv->second.ticks)
      p.cpu_pct = static_cast<double>(ticks - pv->second.ticks) / clk_tck_ / dt * 100.0;

    next_static[pid] = std::move(st);
    next_prev[pid] = {start, ticks};
    snap.procs.push_back(std::move(p));
  }

  static_ = std::move(next_static);
  prev_ = std::move(next_prev);
  prev_at_ = now;
  snap.listen_ports_nonlocal = count_listening(proc / "net/tcp", false) + count_listening(proc / "net/tcp6", true);
  snap.audio_running = alsa_running(root_);
  snap.finalize();
  return snap;
}

}  // namespace opt::workload
