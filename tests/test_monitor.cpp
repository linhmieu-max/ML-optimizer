#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include "check.hpp"
#include "core/config.hpp"
#include "core/sysfs.hpp"
#include "daemon/scheduler.hpp"
#include "monitor/system_monitor.hpp"

namespace fs = std::filesystem;
using namespace opt;
using namespace opt::monitor;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

void put(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::trunc) << content << "\n";
}

fs::path make_root() {
  char tmpl[] = "/tmp/opt-mon-XXXXXX";
  return ::mkdtemp(tmpl);
}

bool near(double a, double b, double eps = 0.01) { return a > b - eps && a < b + eps; }

void test_sysfs() {
  const auto root = make_root();
  for (const char* n : {"thermal_zone0", "thermal_zone10", "thermal_zone2", "other"}) fs::create_directories(root / n);
  auto l = sysfs::list_dir(root, "thermal_zone");
  CHECK(l.size() == 3 && l[0].filename() == "thermal_zone0" && l[1].filename() == "thermal_zone2" &&
        l[2].filename() == "thermal_zone10");
  put(root / "n", "-274000");
  CHECK(sysfs::read_int(root / "n") == -274000);
  put(root / "bad", "12x");
  CHECK(!sysfs::read_int(root / "bad"));
  CHECK(!sysfs::read_int(root / "missing"));
  CHECK(sysfs::parse_double("0.09") && near(*sysfs::parse_double("0.09"), 0.09));
  CHECK(!sysfs::parse_double("x"));
  fs::remove_all(root);
}

void test_cpu() {
  const auto root = make_root();
  const std::string head = "cpu  100 0 100 800 0 0 0 0 0 0\ncpu0 50 0 50 400 0 0 0 0 0 0\ncpu1 50 0 50 400 0 0 0 0 0 0\n";
  put(root / "proc/stat", head + "intr 1\n");
  put(root / "proc/loadavg", "0.52 0.58 0.59 2/1234 5678");
  put(root / "sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "1000000");
  put(root / "sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq", "3000000");
  put(root / "sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "powersave");
  put(root / "sys/devices/system/cpu/intel_pstate/no_turbo", "0");
  CpuMonitor m(root);
  auto a = m.sample();
  CHECK(a && !a->utilization_pct && a->online_cpus == 2);
  CHECK(a->freq_avg_mhz && near(*a->freq_avg_mhz, 2000) && near(*a->freq_min_mhz, 1000) && near(*a->freq_max_mhz, 3000));
  CHECK(a->governor == "powersave" && a->turbo_enabled == true && a->load1 && near(*a->load1, 0.52));
  // +1000 total, +750 idle => 25 % busy; cpu0 +500/+250 => 50 %, cpu1 +500/+500 => 0 %
  put(root / "proc/stat", "cpu  200 0 250 1550 0 0 0 0 0 0\ncpu0 150 0 200 650 0 0 0 0 0 0\ncpu1 50 0 50 900 0 0 0 0 0 0\n");
  auto b = m.sample();
  CHECK(b && b->utilization_pct && near(*b->utilization_pct, 25.0));
  CHECK(b->per_cpu_pct.size() == 2 && near(b->per_cpu_pct[0], 50.0) && near(b->per_cpu_pct[1], 0.0));
  CHECK(!CpuMonitor(root / "nope").sample());
  fs::remove_all(root);
}

void test_memory() {
  const auto root = make_root();
  put(root / "proc/meminfo",
      "MemTotal: 8000000 kB\nMemFree: 500000 kB\nMemAvailable: 2000000 kB\nBuffers: 1 kB\nCached: 2 kB\n"
      "SwapTotal: 4000000 kB\nSwapFree: 3000000 kB\nDirty: 10 kB\nWriteback: 0 kB");
  put(root / "proc/vmstat", "pswpin 100\npswpout 200\nother 5");
  MemoryMonitor m(root);
  const auto t0 = Clock::now();
  auto a = m.sample(t0);
  CHECK(a && a->total_kb == 8000000 && near(a->used_pct(), 75.0) && !a->swap_in_pages_s);
  put(root / "proc/vmstat", "pswpin 300\npswpout 200");
  auto b = m.sample(t0 + seconds(2));
  CHECK(b && b->swap_in_pages_s && near(*b->swap_in_pages_s, 100.0) && near(*b->swap_out_pages_s, 0.0));
  put(root / "proc/meminfo", "MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 100 kB\nCached: 300 kB");  // old kernel
  auto c = m.sample(t0 + seconds(3));
  CHECK(c && c->available_kb == 500);
  fs::remove_all(root);
}

void test_psi() {
  const auto root = make_root();
  put(root / "proc/pressure/cpu", "some avg10=1.50 avg60=0.09 avg300=0.65 total=132229698\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0");
  put(root / "proc/pressure/io", "some avg10=0.12 avg60=0.11 avg300=0.05 total=6359490\nfull avg10=0.12 avg60=0.10 avg300=0.04 total=5052105");
  auto s = PsiMonitor(root).sample();
  CHECK(s && s->cpu && s->cpu->some && near(s->cpu->some->avg10, 1.5) && s->cpu->some->total_us == 132229698);
  CHECK(s->io && s->io->full && near(s->io->full->avg60, 0.10) && !s->memory);
  CHECK(!PsiMonitor(root / "none").sample());
  CHECK(PsiMonitor(root / "none").sample().error().code == Errc::Unsupported);
  fs::remove_all(root);
}

void test_thermal() {
  const auto root = make_root();
  const fs::path th = root / "sys/class/thermal";
  put(th / "thermal_zone2/type", "TCPU");
  put(th / "thermal_zone2/temp", "66050");
  put(th / "thermal_zone2/trip_point_0_temp", "103000");
  put(th / "thermal_zone2/trip_point_0_type", "passive");
  put(th / "thermal_zone2/trip_point_1_temp", "110000");
  put(th / "thermal_zone2/trip_point_1_type", "critical");
  put(th / "thermal_zone10/type", "iwlwifi_1");
  put(th / "thermal_zone10/temp", "46000");
  put(th / "thermal_zone10/trip_point_0_temp", "-274000");
  put(th / "thermal_zone10/trip_point_0_type", "active");
  put(th / "thermal_zone3/type", "broken");  // no temp file
  put(th / "cooling_device0/type", "Processor");
  put(th / "cooling_device0/cur_state", "0");
  put(th / "cooling_device0/max_state", "10");
  ThermalMonitor m(root);
  auto s = m.sample();
  CHECK(s && s->zones.size() == 3);
  CHECK(s->zones[0].name == "thermal_zone2" && s->zones[2].name == "thermal_zone10");  // natural order
  CHECK(s->zones[0].trips.size() == 2 && s->zones[2].trips.empty());                    // -274000 dropped
  CHECK(s->zones[1].temp_c == std::nullopt);
  CHECK(s->hottest() && s->hottest()->type == "TCPU" && near(*s->hottest()->temp_c, 66.05));
  CHECK(s->zones[0].critical_c() && near(*s->zones[0].critical_c(), 110.0));
  CHECK(s->cooling.size() == 1 && s->cooling[0].max_state == 10);
  put(th / "thermal_zone2/temp", "70000");
  put(th / "thermal_zone2/trip_point_1_temp", "50000");  // trips are cached
  auto s2 = m.sample();
  CHECK(near(*s2->zones[0].temp_c, 70.0) && near(*s2->zones[0].critical_c(), 110.0));
  fs::remove_all(root);
}

void test_power() {
  const auto root = make_root();
  const fs::path ps = root / "sys/class/power_supply";
  put(ps / "ADP1/type", "Mains");
  put(ps / "ADP1/online", "1");
  put(ps / "BAT0/type", "Battery");
  put(ps / "BAT0/status", "Charging");
  put(ps / "BAT0/capacity", "87");
  put(ps / "BAT0/current_now", "1000000");
  put(ps / "BAT0/voltage_now", "12000000");
  const fs::path rapl = root / "sys/class/powercap/intel-rapl:0";
  put(rapl / "energy_uj", "1000000");
  put(rapl / "max_energy_range_uj", "10000000");
  put(root / "sys/class/powercap/intel-rapl:0:0/energy_uj", "1");  // sub-domain ignored
  PowerMonitor m(root);
  const auto t0 = Clock::now();
  auto a = m.sample(t0);
  CHECK(a && a->on_ac == true && a->battery_pct == 87 && !a->rapl_pkg_w);
  CHECK(a->supplies[1].power_w && near(*a->supplies[1].power_w, 12.0));
  put(rapl / "energy_uj", "6000000");  // +5 J in 2 s
  auto b = m.sample(t0 + seconds(2));
  CHECK(b->rapl_pkg_w && near(*b->rapl_pkg_w, 2.5));
  put(rapl / "energy_uj", "1000000");  // wrapped: 10 - 6 + 1 = 5 J
  auto c = m.sample(t0 + seconds(4));
  CHECK(c->rapl_pkg_w && near(*c->rapl_pkg_w, 2.5));
  put(ps / "ADP1/online", "0");
  CHECK(m.sample(t0 + seconds(5))->on_ac == false);
  CHECK(!PowerMonitor(root / "none").sample());
  fs::remove_all(root);
}

void test_io() {
  const auto root = make_root();
  put(root / "sys/block/nvme0n1/queue/scheduler", "[none] mq-deadline");
  put(root / "sys/block/nvme0n1/queue/rotational", "0");
  fs::create_directories(root / "sys/block/nvme0n1/nvme0n1p1");  // partition lives below the disk
  auto stats = [&](int r_sect, int w_sect, int io_ms) {
    put(root / "proc/diskstats",
        "259 0 nvme0n1 10 0 " + std::to_string(r_sect) + " 5 20 0 " + std::to_string(w_sect) + " 6 0 " +
            std::to_string(io_ms) + " 7\n259 1 nvme0n1p1 1 0 1 1 1 0 1 1 0 1 1\n7 0 loop0 1 0 1 1 1 0 1 1 0 1 1\n");
  };
  stats(1000, 2000, 100);
  IoMonitor m(root);
  const auto t0 = Clock::now();
  auto a = m.sample(t0);
  CHECK(a && a->devices.size() == 1 && a->devices[0].name == "nvme0n1" && !a->total_read_bps);
  CHECK(a->devices[0].scheduler == "none" && !a->devices[0].rotational);
  stats(3048, 2000, 600);  // +2048 sectors read (1 MiB) in 2 s, busy 500 ms
  auto b = m.sample(t0 + seconds(2));
  CHECK(b->total_read_bps && near(*b->total_read_bps, 524288.0, 1) && near(*b->total_write_bps, 0.0));
  CHECK(b->devices[0].util_pct && near(*b->devices[0].util_pct, 25.0));
  fs::remove_all(root);
}

void test_gpu() {
  const auto root = make_root();
  const fs::path drm = root / "sys/class/drm";
  fs::create_directories(drm / "card0/device");
  fs::create_directories(root / "drivers/i915");
  fs::create_symlink(root / "drivers/i915", drm / "card0/device/driver");
  fs::create_directories(drm / "card0-eDP-1");
  put(drm / "card0/gt_cur_freq_mhz", "300");
  put(drm / "card0/gt_max_freq_mhz", "1100");
  put(drm / "card0/power/rc6_residency_ms", "1000");
  fs::create_directories(drm / "card1/device");
  fs::create_directories(root / "drivers/amdgpu");
  fs::create_symlink(root / "drivers/amdgpu", drm / "card1/device/driver");
  put(drm / "card1/device/gpu_busy_percent", "42");
  put(drm / "card1/device/pp_dpm_sclk", "0: 300Mhz\n1: 800Mhz *\n2: 1500Mhz");
  put(drm / "card1/device/hwmon/hwmon3/temp1_input", "55000");
  put(root / "sys/class/devfreq/soc/cur_freq", "800000000");
  put(root / "sys/class/devfreq/soc/governor", "simple_ondemand");
  GpuMonitor m(root);
  const auto t0 = Clock::now();
  auto a = m.sample(t0);
  CHECK(a && a->devices.size() == 2 && a->devices[0].card == "card0" && a->devices[0].driver == "i915");
  CHECK(a->devices[0].cur_mhz == 300 && a->devices[0].max_mhz == 1100 && !a->devices[0].busy_pct);
  CHECK(a->devices[1].busy_pct && near(*a->devices[1].busy_pct, 42) && a->devices[1].cur_mhz == 800);
  CHECK(a->devices[1].temp_c && near(*a->devices[1].temp_c, 55.0));
  CHECK(a->devfreq.size() == 1 && a->devfreq[0].cur_mhz && near(*a->devfreq[0].cur_mhz, 800.0));
  put(drm / "card0/power/rc6_residency_ms", "1800");  // asleep 800 ms of 1000 ms => ~20 % active
  auto b = m.sample(t0 + seconds(1));
  CHECK(b->devices[0].busy_pct && near(*b->devices[0].busy_pct, 20.0) && b->devices[0].busy_is_estimate);
  CHECK(!GpuMonitor(root / "none").sample());
  fs::remove_all(root);
}

void test_system_and_format() {
  {  // completely empty machine: every section empty, no crash
    const auto root = make_root();
    SystemMonitor sm(root);
    auto s = sm.sample();
    CHECK(!s.cpu && !s.memory && !s.psi && !s.thermal && !s.power && !s.io && !s.gpu);
    CHECK(format_summary(s).size() == 7 && format_summary(s)[0].second == "n/a");
    fs::remove_all(root);
  }
  const auto root = make_root();
  put(root / "proc/stat", "cpu  100 0 100 800 0 0 0 0 0 0\ncpu0 100 0 100 800 0 0 0 0 0 0\n");
  put(root / "proc/meminfo", "MemTotal: 8388608 kB\nMemAvailable: 4194304 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB");
  put(root / "proc/pressure/cpu", "some avg10=0.00 avg60=0.00 avg300=0.00 total=1");
  put(root / "sys/class/thermal/thermal_zone0/type", "acpitz");
  put(root / "sys/class/thermal/thermal_zone0/temp", "40000");
  put(root / "sys/class/thermal/thermal_zone0/trip_point_0_temp", "100000");
  put(root / "sys/class/thermal/thermal_zone0/trip_point_0_type", "critical");
  SystemMonitor sm(root);
  auto s = sm.sample();
  CHECK(s.sequence == 1 && s.cpu && s.memory && s.psi && s.thermal && !s.gpu);
  const std::string txt = format_snapshot(s);
  CHECK(txt.find("4.0 GiB / 8.0 GiB used (50%)") != std::string::npos);
  CHECK(txt.find("60.0 C below critical (100)") != std::string::npos);
  CHECK(txt.find("thermal zones:") != std::string::npos);
  CHECK(sm.sample().sequence == 2);
  fs::remove_all(root);
}

void test_scheduler() {
  std::atomic<int> a{0}, b{0};
  Scheduler s;
  s.add("fast", milliseconds(20), [&] { ++a; });
  s.add("throws", milliseconds(20), [&] { ++b; throw std::runtime_error("expected in test"); });
  s.start();
  std::this_thread::sleep_for(milliseconds(300));
  CHECK(a >= 5 && a <= 20);
  CHECK(b >= 5);  // a throwing task keeps being scheduled and does not kill the thread
  s.set_interval("fast", seconds(60));
  std::this_thread::sleep_for(milliseconds(60));
  const int frozen = a;
  std::this_thread::sleep_for(milliseconds(150));
  CHECK(a == frozen || a == frozen + 1);
  const auto t0 = std::chrono::steady_clock::now();
  s.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < seconds(1));
  s.stop();  // idempotent
}

}  // namespace

void run_monitor_tests() {
  test_sysfs();
  test_cpu();
  test_memory();
  test_psi();
  test_thermal();
  test_power();
  test_io();
  test_gpu();
  test_system_and_format();
  test_scheduler();
}
