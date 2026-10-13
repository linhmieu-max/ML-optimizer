#include <filesystem>
#include <fstream>
#include <memory>

#include "check.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "workload/classifier.hpp"

namespace fs = std::filesystem;
using namespace opt;
using namespace opt::workload;
using std::chrono::seconds;

namespace {

void put(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::trunc) << content;
}
bool near(double a, double b, double eps = 0.5) { return a > b - eps && a < b + eps; }

// ---------- fake /proc ----------
void add_proc(const fs::path& root, int pid, int ppid, const std::string& comm, std::uint64_t ticks, const std::string& exe,
              const std::string& cgroup = "0::/user.slice/session.scope", const std::string& cmdline = "") {
  const fs::path d = root / "proc" / std::to_string(pid);
  // fields after ')': state ppid pgrp sess tty tpgid flags minflt cminflt majflt cmajflt utime stime cutime cstime prio nice threads itreal start vsize rss
  put(d / "stat", std::to_string(pid) + " (" + comm + ") S " + std::to_string(ppid) + " 1 1 0 -1 0 0 0 0 0 " +
                      std::to_string(ticks) + " 0 0 0 20 0 3 0 777 0 250");
  put(d / "cgroup", cgroup + "\n");
  if (!exe.empty()) {
    fs::remove(d / "exe");
    fs::create_symlink(exe, d / "exe");
    std::string c = cmdline;
    for (char& ch : c) if (ch == ' ') ch = '\0';
    put(d / "cmdline", c);
  }
}
fs::path make_root() {
  char t[] = "/tmp/opt-wl-XXXXXX";
  return ::mkdtemp(t);
}

void test_scanner() {
  const auto root = make_root();
  add_proc(root, 10, 1, "Web Content", 100, "/usr/lib/firefox/firefox", "0::/user.slice/app.scope");
  add_proc(root, 11, 10, "a) b (c", 0, "/usr/bin/x");            // tricky comm
  add_proc(root, 12, 2, "kworker/0:1", 0, "");                     // kernel thread, no exe
  add_proc(root, 13, 1, "game", 0, "/home/u/.steam/steamapps/common/G/game", "0::/user.slice/x.scope",
           "/home/u/.steam/steamapps/common/G/game --secret-token=abc");
  add_proc(root, 14, 1, "chrome", 0, "/opt/chrome/chrome", "0::/user.slice/x.scope", "/opt/chrome/chrome --type=renderer");
  put(root / "proc/net/tcp",
      "  sl  local_address rem_address   st\n"
      "   0: 0100007F:1F90 00000000:0000 0A\n"   // 127.0.0.1 listen: ignored
      "   1: 00000000:0050 00000000:0000 0A\n"   // 0.0.0.0:80 listen
      "   2: 0F01A8C0:C000 0100A8C0:0050 01\n"); // established: ignored
  put(root / "proc/net/tcp6", "  sl local rem st\n   0: 00000000000000000000000001000000:0016 00000000000000000000000000000000:0000 0A\n");
  put(root / "proc/asound/card0/pcm0p/sub0/status", "state: RUNNING\nowner_pid: 1\n");
  ProcessScanner sc(root, 100);
  const auto t0 = Clock::now();
  auto a = sc.scan(t0);
  CHECK(a && a->procs.size() == 5);
  const ProcessInfo* w = a->find(10);
  CHECK(w && w->comm == "web content" && w->exe == "firefox" && w->ppid == 1 && w->threads == 3 && !w->cpu_pct);
  CHECK(w->rss_kb > 0 && w->cgroup == "/user.slice/app.scope");
  CHECK(a->find(11) && a->find(11)->comm == "a) b (c");
  CHECK(a->find(12) && a->find(12)->exe.empty());
  CHECK(a->find(13) && (a->find(13)->cmd_flags & kCmdSteamApp));
  CHECK(a->find(14) && (a->find(14)->cmd_flags & kCmdChromiumRenderer));
  CHECK(a->listen_ports_nonlocal == 1);
  CHECK(a->audio_running == true);
  CHECK(a->has_ancestor(*a->find(11), [](const ProcessInfo& p) { return p.comm == "web content"; }));
  CHECK(!a->has_ancestor(*a->find(10), [](const ProcessInfo& p) { return p.comm == "web content"; }));

  add_proc(root, 10, 1, "Web Content", 300, "/usr/lib/firefox/firefox");  // +200 ticks in 2 s at 100 Hz = 100 %
  put(root / "proc/asound/card0/pcm0p/sub0/status", "closed\n");
  auto b = sc.scan(t0 + seconds(2));
  CHECK(b && b->find(10)->cpu_pct && near(*b->find(10)->cpu_pct, 100.0));
  CHECK(b->audio_running == false);
  fs::remove_all(root / "proc/11");  // process exits
  CHECK(sc.scan(t0 + seconds(3))->find(11) == nullptr);
  CHECK(!ProcessScanner(root / "nowhere").scan());
  fs::remove_all(root);
}

// ---------- classifier scenarios on synthetic snapshots ----------
struct World {
  ProcessSnapshot ps;
  std::unique_ptr<monitor::SystemSnapshot> sys = std::make_unique<monitor::SystemSnapshot>();
  int next = 100;

  World(double cpu_util = 10) {
    sys->cpu = monitor::CpuSample{};
    sys->cpu->utilization_pct = cpu_util;
    sys->memory = monitor::MemorySample{};
    sys->memory->total_kb = 8000000;
    sys->memory->available_kb = 5000000;
  }
  ProcessInfo& add(const std::string& comm, double cpu, const std::string& cgroup = "0::/user.slice/app.scope",
                   int ppid = 1, unsigned flags = 0, std::uint64_t rss_kb = 100000) {
    ProcessInfo p;
    p.pid = next++;
    p.ppid = ppid;
    p.comm = comm;
    p.exe = comm;
    p.cgroup = cgroup;
    p.cpu_pct = cpu;
    p.cmd_flags = flags;
    p.rss_kb = rss_kb;
    ps.procs.push_back(p);
    return ps.procs.back();
  }
  void gpu(double busy) {
    sys->gpu = monitor::GpuSample{};
    monitor::GpuDevice g;
    g.card = "card0";
    g.driver = "i915";
    g.busy_pct = busy;
    sys->gpu->devices.push_back(g);
  }
  void io(double read_mbps, double write_mbps) {
    sys->io.emplace();
    sys->io->total_read_bps = read_mbps * 1e6;
    sys->io->total_write_bps = write_mbps * 1e6;
  }
  DetectContext ctx() {
    ps.finalize();
    return DetectContext{ps, sys.get()};
  }
};

WorkloadClass classify(World& w) {
  Classifier c({0.35, 0.15, 1});  // no hysteresis delay for single-shot checks
  return c.evaluate(w.ctx(), Clock::now()).cls;
}

void test_classes() {
  {  World w(2); w.add("gnome-shell", 1.0); CHECK(classify(w) == WorkloadClass::Idle); }
  {  World w(25); w.add("gnome-shell", 9.0); w.add("nautilus", 6.0); CHECK(classify(w) == WorkloadClass::Desktop); }
  {  World w(85); w.add("gnome-shell", 2); w.add("ninja", 1); w.add("cc1plus", 95); w.add("cc1plus", 90);
     Classifier c({0.35, 0.15, 1}); auto& r = c.evaluate(w.ctx());
     CHECK(r.cls == WorkloadClass::Compile && r.confidence > 0.6 && (r.flags & kHighLoad) && !r.reasons.empty()); }
  {  World w(60); w.add("ffmpeg", 280); CHECK(classify(w) == WorkloadClass::Encode); }
  {  World w(70); w.add("blender", 300); w.gpu(70); CHECK(classify(w) == WorkloadClass::Render); }
  {  World w(40); w.add("zstd", 100); w.io(80, 60); CHECK(classify(w) == WorkloadClass::Archive); }
  {  World w(15); w.add("rsync", 5); w.io(90, 85); CHECK(classify(w) == WorkloadClass::FileCopy); }
  {  World w(15); w.io(120, 110); CHECK(classify(w) == WorkloadClass::FileCopy); }  // I/O pattern alone
  {  World w(50); w.add("qemu-system-x86", 120, "0::/machine.slice/x.scope", 1, 0, 4u * 1024 * 1024); CHECK(classify(w) == WorkloadClass::Vm); }
  {  World w(30); w.add("postgres", 30, "0::/docker/abc123"); w.add("dockerd", 2, "0::/system.slice/docker.service");
     CHECK(classify(w) == WorkloadClass::Container); }
  {  World w(12); w.add("nginx", 8, "0::/system.slice/nginx.service"); w.ps.listen_ports_nonlocal = 2; CHECK(classify(w) == WorkloadClass::Server); }
  {  World w(25); w.add("firefox", 10); w.add("web content", 12); w.add("web content", 8, "0::/user.slice/app.scope", 1, kCmdChromiumRenderer);
     w.add("gnome-shell", 6); CHECK(classify(w) == WorkloadClass::Browser); }
  {  World w(20); w.add("mpv", 12); w.add("gnome-shell", 3); w.ps.audio_running = true; w.gpu(15);
     Classifier c({0.35, 0.15, 1}); auto& r = c.evaluate(w.ctx());
     CHECK(r.cls == WorkloadClass::VideoPlayback && (r.flags & kLatencySensitive)); }
  {  World w(10); w.add("updatedb", 30, "0::/system.slice/plocate.service"); CHECK(classify(w) == WorkloadClass::Background); }
}

void test_game_rules() {
  {  // Steam game process + GPU: GAME and latency sensitive
    World w(55); w.add("steam", 2); w.add("gnome-shell", 8);
    w.add("game", 160, "0::/user.slice/app.scope", 1, kCmdSteamApp); w.gpu(80);
    Classifier c({0.35, 0.15, 1}); auto& r = c.evaluate(w.ctx());
    CHECK(r.cls == WorkloadClass::Game && (r.flags & kLatencySensitive) && r.confidence > 0.5);
  }
  {  // Wine game under a launcher
    World w(50); const int launcher = w.add("lutris", 2).pid;  // copy the pid: add() may reallocate
    w.add("wineserver", 3);
    w.add("game.exe", 150, "0::/user.slice/app.scope", launcher);
    w.gpu(70);
    CHECK(classify(w) == WorkloadClass::Game);
  }
  {  // Wine helper processes alone are not a game
    World w(10); w.add("services.exe", 12); w.add("winedevice.exe", 8); CHECK(classify(w) != WorkloadClass::Game);
  }
  {  // Heavy app + lots of RAM + busy GPU, no launcher/markers: NOT a game, but flagged
    World w(60); w.add("someapp", 120, "0::/user.slice/app.scope", 1, 0, 6u * 1024 * 1024); w.gpu(75);
    w.sys->memory->available_kb = 600000;
    Classifier c({0.35, 0.15, 1}); auto& r = c.evaluate(w.ctx());
    CHECK(r.cls != WorkloadClass::Game);
    CHECK((r.flags & kLatencySensitive) && (r.flags & kResourceHeavy));
  }
  {  // Compiler with busy GPU is not a game
    World w(90); w.add("cc1plus", 95); w.add("cc1plus", 95); w.gpu(70); CHECK(classify(w) == WorkloadClass::Compile);
  }
}

void test_flags() {
  World w(80); w.add("gnome-shell", 5);
  Classifier c; auto& r = c.evaluate(w.ctx());
  CHECK((r.flags & kHighLoad) && !(r.flags & kResourceHeavy) && flags_to_string(r.flags) == "HIGH_LOAD");
  World m(10); m.sys->memory->available_kb = 400000;  // 95 % used
  Classifier c2; CHECK(c2.evaluate(m.ctx()).flags & kResourceHeavy);
  CHECK(flags_to_string(0) == "none");
}

void test_hysteresis() {
  Classifier c({0.35, 0.15, 3});
  const auto t0 = Clock::now();
  World compile(85); compile.add("cc1plus", 95); compile.add("cc1plus", 90); compile.add("make", 1);
  CHECK(c.evaluate(compile.ctx(), t0).cls == WorkloadClass::Compile);  // first evaluation adopts immediately

  World desk(20); desk.add("gnome-shell", 9); desk.add("nautilus", 6);
  CHECK(c.evaluate(desk.ctx(), t0 + seconds(1)).cls == WorkloadClass::Compile);
  CHECK(c.current().pending == WorkloadClass::Desktop && c.current().pending_streak == 1);
  CHECK(c.evaluate(desk.ctx(), t0 + seconds(2)).cls == WorkloadClass::Compile);
  CHECK(c.evaluate(compile.ctx(), t0 + seconds(3)).cls == WorkloadClass::Compile);  // challenger vanished
  CHECK(!c.current().pending);
  for (int i = 4; i < 6; ++i) CHECK(c.evaluate(desk.ctx(), t0 + seconds(i)).cls == WorkloadClass::Compile);
  CHECK(c.evaluate(desk.ctx(), t0 + seconds(6)).cls == WorkloadClass::Desktop);     // 3rd consecutive win
  CHECK(c.current().since == t0 + seconds(6) && c.current().evaluations == 7);
  {  // when all evidence disappears the stale specific class is dropped (after confirmation), landing on IDLE
    Classifier k({0.35, 0.15, 3});
    k.evaluate(compile.ctx(), t0);
    World nothing(100);  // saturated CPU, no recognisable processes: every score is 0
    CHECK(k.evaluate(nothing.ctx(), t0 + seconds(1)).cls == WorkloadClass::Compile);
    CHECK(k.evaluate(nothing.ctx(), t0 + seconds(2)).cls == WorkloadClass::Compile);
    CHECK(k.evaluate(nothing.ctx(), t0 + seconds(3)).cls == WorkloadClass::Idle);
  }
  const std::string txt = format_workload(c.current(), t0 + seconds(10));
  CHECK(txt.find("DESKTOP") != std::string::npos && txt.find("for 4s") != std::string::npos);
  CHECK(txt.find("Busiest processes") != std::string::npos);
}

void test_names_privacy() {
  ProcessInfo p;
  p.comm = "cc1plus";
  CHECK(is_compile_worker(p));
  p.comm = "clang-19";
  CHECK(is_compile_worker(p));
  p.comm = "chrome_crashpad";
  CHECK(is_browser(p));  // prefix rule is intentionally broad
  p.comm = "ccache";
  CHECK(!is_compile_worker(p) && is_compile_orchestrator(p));
  p.comm = "bash";
  CHECK(!is_browser(p) && !is_server_daemon(p) && !is_vm(p));
}

}  // namespace

void run_workload_tests() {
  log::set_level(log::Level::Safety);
  test_scanner();
  test_classes();
  test_game_rules();
  test_flags();
  test_hysteresis();
  test_names_privacy();
}
