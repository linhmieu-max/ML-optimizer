// Dependency-free test runner (no GoogleTest needed).
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "check.hpp"
#include "core/capabilities.hpp"
#include "core/config.hpp"
#include "ipc/client.hpp"
#include "ipc/server.hpp"

namespace fs = std::filesystem;
using namespace opt;

void run_monitor_tests();
void run_workload_tests();
void run_policy_tests();
void run_safety_tests();
void run_control_tests();
void run_telemetry_tests();
void run_ml_tests();
void run_adapt_tests();
void run_tool_inventory_tests();

static void put(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << content << "\n";
}

static Result<int> half(int n) {
  if (n % 2) return fail(make_error(Errc::InvalidArgument, "odd"));
  return n / 2;
}

static void test_result() {
  auto a = half(4);
  CHECK(a && *a == 2);
  auto b = half(3);
  CHECK(!b && b.error().code == Errc::InvalidArgument);
  Result<void> v = {};
  CHECK(v.has_value());
}

static void test_config() {
  auto c = parse_config("[daemon]\nlog_level = debug\n[policy]\naggressiveness=2\n# comment\n[ipc]\nadmin_group = wheel\n");
  CHECK(c && c->log_level == log::Level::Debug && c->aggressiveness == 2 && c->admin_group == "wheel");
  CHECK(!parse_config("[policy]\naggressiveness = 9\n"));
  CHECK(!parse_config("[policy]\naggressiveness = 3x\n"));
  CHECK(!parse_config("[policy]\nbogus = 1\n"));
  CHECK(!parse_config("[nope]\n"));
  CHECK(!parse_config("orphan = 1\n"));
  auto mon = parse_config("[monitor]\ninterval_ms = 500\n");
  CHECK(mon && mon->monitor_interval_ms == 500);
  CHECK(!parse_config("[monitor]\ninterval_ms = 10\n"));
  CHECK(!parse_config("[monitor]\ninterval_ms = fast\n"));
  auto wl = parse_config("[workload]\ninterval_ms = 1500\n");
  CHECK(wl && wl->workload_interval_ms == 1500);
  CHECK(!parse_config("[workload]\ninterval_ms = 100\n"));
  auto pol = parse_config("[policy]\ninterval_ms = 3000\n");
  CHECK(pol && pol->policy_interval_ms == 3000 && pol->aggressiveness == 3);
  CHECK(!parse_config("[policy]\ninterval_ms = 1\n"));
  auto sf = parse_config("[safety]\ninterval_ms = 500\n");
  CHECK(sf && sf->safety_interval_ms == 500);
  CHECK(!parse_config("[safety]\ninterval_ms = 100000\n"));
  CHECK(!parse_config("[safety]\nwarm_margin = 1\n"));  // thresholds are deliberately not configurable
  CHECK(!parse_config("[ipc]\nsocket_path = relative/sock\n"));
  CHECK(!parse_config("[ipc]\nadmin_group = bad group!\n"));
  bool found = true;
  auto d = load_config("/nonexistent/opt.conf", &found);
  CHECK(d && !found && d->aggressiveness == 3);
}

static void test_protocol() {
  auto r = ipc::parse_request("OPT/1 set-log-level debug");
  CHECK(r && r->command == "set-log-level" && r->args.size() == 1);
  CHECK(!ipc::parse_request("OPT/2 status"));
  CHECK(!ipc::parse_request("OPT/1 STATUS"));
  CHECK(!ipc::parse_request("OPT/1 status ../etc"));
  CHECK(!ipc::parse_request("OPT/1 status a;b"));
  CHECK(!ipc::parse_request("OPT/1  status"));
  CHECK(!ipc::parse_request("OPT/1 status a\nb"));
  CHECK(!ipc::parse_request("OPT/1 x 1 2 3 4 5 6 7 8 9"));

  auto ok = ipc::parse_response(ipc::serialize_response(ipc::ok_response("hi\n")));
  CHECK(ok && ok->ok && ok->body == "hi\n");
  auto er = ipc::parse_response(ipc::serialize_response(ipc::error_response(Errc::PermissionDenied, "no")));
  CHECK(er && !er->ok && er->code == Errc::PermissionDenied && er->body == "no");

  int sv[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  CHECK(ipc::write_frame(sv[0], "hello", 64).has_value());
  auto f = ipc::read_frame(sv[1], 64);
  CHECK(f && *f == "hello");
  const unsigned char huge[4] = {0x7f, 0xff, 0xff, 0xff};  // length far above the limit
  CHECK(::write(sv[0], huge, 4) == 4);
  CHECK(!ipc::read_frame(sv[1], 64));
  ::close(sv[0]);
  ::close(sv[1]);
}

static void test_capabilities() {
  char tmpl[] = "/tmp/opt-test-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  const fs::path cpu = root / "sys/devices/system/cpu";
  for (int i = 0; i < 4; ++i) fs::create_directories(cpu / ("cpu" + std::to_string(i)));
  put(cpu / "cpufreq/policy0/scaling_driver", "intel_pstate");
  put(cpu / "cpufreq/policy0/scaling_governor", "powersave");
  put(cpu / "cpufreq/policy0/scaling_available_governors", "performance powersave");
  put(cpu / "cpufreq/policy0/energy_performance_available_preferences", "default performance power");
  put(cpu / "cpufreq/policy0/energy_performance_preference", "performance");
  put(cpu / "intel_pstate/no_turbo", "0");
  put(root / "proc/pressure/cpu", "some avg10=0.00");
  put(root / "sys/class/thermal/thermal_zone0/type", "acpitz");
  put(root / "sys/class/thermal/thermal_zone0/trip_point_0_temp", "105000");
  fs::create_directories(root / "sys/class/thermal/cooling_device0");
  put(root / "sys/class/power_supply/BAT0/type", "Battery");
  put(root / "sys/class/power_supply/ADP1/type", "Mains");
  fs::create_directories(root / "sys/class/drm/card0-eDP-1");

  auto r = detect_capabilities(root, "6.12.101+deb13-amd64");
  CHECK(r.kernel_major == 6 && r.kernel_minor == 12);
  CHECK(r.available("cpufreq") && r.find("cpufreq")->detail.find("intel_pstate") != std::string::npos);
  CHECK(r.available("cpufreq.epp"));
  CHECK(r.available("cpu.boost"));
  CHECK(r.find("sched.uclamp")->status == CapStatus::Unsupported);  // no sysctl in fake tree
  CHECK(r.available("psi"));
  CHECK(r.find("cgroup.v2")->status == CapStatus::Unavailable);
  CHECK(r.available("thermal") && r.find("thermal")->detail.find("1 with trip") != std::string::npos);
  CHECK(r.available("power") && r.find("power")->detail.find("1 mains, 1 battery") != std::string::npos);
  CHECK(r.find("gpu.drm")->status == CapStatus::Unavailable);  // only a connector, not a card
  CHECK(r.find("gpu.devfreq")->status == CapStatus::Unavailable);
  CHECK(r.find("mem.zram")->status == CapStatus::Unavailable);
  CHECK(r.find("cpu.topology")->detail.find("4 logical") != std::string::npos);

  put(root / "proc/sys/kernel/sched_util_clamp_min", "1024");
  CHECK(detect_capabilities(root, "6.1.0").available("sched.uclamp"));
  auto old = detect_capabilities(root, "4.9.0");
  CHECK(old.find("psi")->status == CapStatus::Unsupported);
  CHECK(old.find("sched.uclamp")->status == CapStatus::Unsupported);
  CHECK(old.find("kernel")->status == CapStatus::Available);

  auto empty = detect_capabilities(root / "does-not-exist", "5.10.0");
  CHECK(empty.find("cpufreq")->status == CapStatus::Unavailable);
  CHECK(empty.find("thermal")->status == CapStatus::Unavailable);
  fs::remove_all(root);
}

static void test_ipc_roundtrip() {
  char tmpl[] = "/tmp/opt-test-XXXXXX";
  const fs::path dir = ::mkdtemp(tmpl);
  const fs::path sock = dir / "run/control.sock";
  ipc::IpcServer server(sock, [](const ipc::Request& req, const ipc::PeerCred& peer) {
    if (req.command == "whoami") return ipc::ok_response(std::to_string(peer.uid));
    if (req.command == "boom") throw std::runtime_error("x");
    return ipc::error_response(Errc::InvalidArgument, "unknown");
  });
  auto s = server.start();
  CHECK(s.has_value());

  auto r = ipc::call(sock, {"whoami", {}});
  CHECK(r && r->ok && r->body == std::to_string(::geteuid()));  // SO_PEERCRED reports our uid
  auto bad = ipc::call(sock, {"nope", {}});
  CHECK(bad && !bad->ok && bad->code == Errc::InvalidArgument);
  auto boom = ipc::call(sock, {"boom", {}});
  CHECK(boom && !boom->ok && boom->code == Errc::Internal);
  auto again = ipc::call(sock, {"whoami", {}});  // server survived the handler exception
  CHECK(again && again->ok);

  server.stop();
  CHECK(!fs::exists(sock));
  CHECK(!ipc::call(sock, {"whoami", {}}));
  fs::remove_all(dir);
}

int main() {
  log::set_level(log::Level::Safety);  // silence expected error logs
  test_result();
  test_config();
  test_protocol();
  test_capabilities();
  test_ipc_roundtrip();
  run_tool_inventory_tests();
  run_monitor_tests();
  run_workload_tests();
  run_policy_tests();
  run_safety_tests();
  run_control_tests();
  run_telemetry_tests();
  run_ml_tests();
  run_adapt_tests();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
