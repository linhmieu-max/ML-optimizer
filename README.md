# Linux Optimizer — Phase 10 (adaptive monitoring, controllers, telemetry, local ML, external-tool integration)

**Source revision:** 0.10.1. This revision cleans up GCC 16 warning-triggering construction patterns in the optional-package privilege argv builder and workload/telemetry test fixtures; behavior is unchanged.

Adaptive Linux system optimizer daemon (C++20, systemd, root): monitors the machine, classifies the
workload, decides a profile, and (Phase 6) applies it through gated, journaled controllers.

> **Controllers default to `dry-run`**: they compute and log what *would* change and write nothing.
> Review `linux-optimizer controllers`, then set `[controllers] mode = apply` (or run
> `sudo linux-optimizer controllers apply` for the current run only).

## Build

Requirements: g++ >= 11 or clang >= 14, CMake >= 3.20, Ninja/Make, **SQLite3 development files**
(Debian/Ubuntu: `libsqlite3-dev`, Fedora: `sqlite-devel`, Arch: `sqlite`). No other dependencies (no
libsystemd, no OpenSSL, no test framework).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure      # or ./build/opt_tests
```

`opt::Result<T>` is `std::expected` when compiled with `-DCMAKE_CXX_STANDARD=23`
(GCC 12-14 only expose `<expected>` in C++23 mode) and the vendored
`third_party/tl/expected.hpp` (CC0) otherwise. Both are tested.

## Try it without installing (no root needed)

```bash
cat > /tmp/opt.conf <<'CONF'
[ipc]
socket_path = /tmp/opt/control.sock
CONF
./build/linux-optimizerd --config /tmp/opt.conf --check     # validate + print capabilities
./build/linux-optimizerd --config /tmp/opt.conf &
export LINUX_OPTIMIZER_SOCKET=/tmp/opt/control.sock
./build/linux-optimizer status
```

## Install as a service

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build
sudo cmake --install build
sudo cp /etc/linux-optimizer/linux-optimizer.conf.example /etc/linux-optimizer/linux-optimizer.conf
sudo groupadd -f linux-optimizer          # optional: members may run admin commands
sudo systemctl daemon-reload
sudo systemctl enable --now linux-optimizerd
linux-optimizer status
```

## Commands

| command | access | notes |
|---|---|---|
| `status`, `detect`, `ping`, `version`, `help` | everyone | `detect` re-probes live; `status` includes live values |
| `profile` | everyone | show user mode + effective profile |
| `profile powersave\|balanced\|performance\|auto [--lock\|--unlock]` | root / group | set mode; lock freezes adaptation |
| `policy [--watch [SEC]]` | everyone | decision: profile, targets, per-subsystem control, reasons |
| `model [off\|observe\|suggest]` | everyone / root to set | ML advisor: models, agreement with the rules, predictions, CPU demand forecast |
| `training [on\|off\|run\|reset-local]` | everyone to view / root to change | local adaptation: idle gate, job progress, checkpoint, last results, what was learned on this machine |
| `history [N]` | everyone | last 24 h per hour/workload/policy + hourly summaries |
| `telemetry [on\|off\|delete --yes]` | everyone to view / root to change | status; pause; permanently delete everything |
| `controllers [off\|dry-run\|apply]` | everyone / root to set | what controllers did or would do, outstanding changes |
| `safety` | everyone | thermal state, sensors + limits, hardware limits, rollback journal |
| `reset` | root / group | profile -> auto, roll back every optimizer change (cannot reset thermal safety) |
| `conflicts` | everyone | installed/running tools, systemd units and subsystem ownership (read-only) |
| `workload [--watch [SEC]]` | everyone | current class, confidence, features, reasons, scores, busiest processes |
| `monitor [--watch [SEC]]` | everyone | latest sample (details: per-CPU, thermal zones + trips, cooling, supplies, disks, devfreq) |
| `set-log-level <lvl>`, `reload-config` | root / group `linux-optimizer` | daemon-internal only |
| `packages` | local CLI | read-only distro/package-manager report and optional packages |
| `install sensors\|thermald` | local CLI | allowlisted optional package; interactive typed confirmation required |

CLI exit codes: 0 ok, 1 daemon returned an error, 2 usage/protocol, 3 daemon unreachable.

## Monitor (Phase 2)

`src/monitor/*` — one class per subsystem, each reading only files that exist
(all paths re-basable for tests). A `Scheduler` thread samples every
`[monitor] interval_ms` (default 1000, 250-60000, reloadable) and publishes an
immutable snapshot; IPC handlers only read the latest snapshot.

| section | source | notes |
|---|---|---|
| CPU | `/proc/stat`, `cpufreq/scaling_cur_freq`, `/proc/loadavg` | utilization from counter deltas; first sample has none |
| Memory | `/proc/meminfo`, `/proc/vmstat` | `MemAvailable` fallback for old kernels; swap page rates |
| PSI | `/proc/pressure/*` | some/full avg10/60/300 |
| Thermal | `/sys/class/thermal` | trips cached, `-274000` placeholders dropped, margin to *critical* |
| Power | `power_supply`, `powercap/intel-rapl:*` | AC/battery; package watts from RAPL (root, wrap-safe) |
| I/O | `/proc/diskstats`, `/sys/block` | whole disks only; MB/s, IOPS, util % |
| GPU | `/sys/class/drm`, `devfreq` | i915/xe freq; i915 "busy" is an **estimate** from RC6 residency; amdgpu busy%/temp; NVIDIA reported as unmonitored |

`linux-optimizerd --check` prints capabilities plus a live 500 ms sample
without opening the socket (run with sudo to include RAPL).

## Workload classifier (Phase 3)

`src/workload/*`: `ProcessScanner` (read-only `/proc` scan, per-process CPU%, static data
cached per pid+start time) -> detectors -> `Classifier` (scores + hysteresis). Runs every
`[workload] interval_ms` (default 2000, 500-60000).

Classes: IDLE DESKTOP BROWSER VIDEO_PLAYBACK GAME COMPILE RENDER ENCODE FILE_COPY ARCHIVE VM
CONTAINER SERVER BACKGROUND. Each has an explainable score (0..1) with reasons
(`linux-optimizer workload`).

- **GAME is never decided from one signal.** It needs Steam/Proton markers, a Wine `.exe`
  game process (Wine helpers excluded), an emulator or gamescope, optionally backed by GPU
  load. A heavy app with a busy GPU and lots of RAM but no such evidence is *not* a game; it
  only raises `LATENCY_SENSITIVE` (game score >= 0.25) and `RESOURCE_HEAVY`/`HIGH_LOAD`.
- **Features** for the policy engine: `HIGH_LOAD` (CPU >= 70 %, PSI cpu >= 25, GPU >= 85),
  `RESOURCE_HEAVY` (RAM >= 85 %, swap I/O, big busy process), `LATENCY_SENSITIVE`
  (game/video evidence, or audio playing during desktop/browser use).
- **Hysteresis**: the first evaluation is adopted at once; afterwards a challenger must beat
  the current class by 0.15 on 3 consecutive evaluations. A specific class whose own evidence
  fell below 0.35 can be replaced without the margin. Confidence = score - 0.5 x best rival
  (DESKTOP/BACKGROUND/IDLE are context, not rivals of a specific workload).
- Signals: process names (`names.cpp` is the only list), cgroup paths (containers, system vs
  user slice), ancestry (launchers), `/proc/net/tcp` listening sockets, ALSA playback state,
  and the Phase 2 snapshot (CPU, GPU, I/O, PSI, memory).
- **Privacy**: command lines are scanned only for a few fixed markers (`steamapps/common`,
  `proton`, `--type=renderer`, `--type=gpu-process`) and discarded; only lower-cased process
  names are kept, in memory only.

Thresholds are initial hand-tuned values, to be refined with telemetry (Phases 7-9).

## Policy engine (Phase 4)

`src/policy/*` turns workload + power + thermal margin + PSI into a **decision**:
a profile (POWERSAVE / BALANCED / PERFORMANCE / THERMAL_EMERGENCY) and backend-agnostic
targets (CPU/GPU bias, boost, I/O bias, background restriction, memory bias, runtime PM).
**It is advisory: nothing applies the targets until the Phase 6 controllers**, and every
write will still pass capability, conflict, thermal and rollback validation.

- **Modes**: `auto` (rules choose), `manual` (user profile fixed; targets still adapt to the
  workload; thermal derating still applies), `locked` (profile *and* targets frozen, no
  adaptation). Only thermal safety (HOT caps at BALANCED, EMERGENCY forces THERMAL_EMERGENCY) can override a lock. The choice is
  persisted in `state_dir/policy.state` (atomic write, strict parser; never in /sys).
- **Aggressiveness** 1-4 (default 3): 1 stays near BALANCED (PERFORMANCE only with proven
  thermal headroom); 2 PERFORMANCE for game/render/encode/compile on AC; 3 also idle/background
  -> POWERSAVE and heavy VM/container/server -> PERFORMANCE; 4 also loaded desktop/browser and
  gaming on battery (>= 40 %). Battery <= 15 % always forces POWERSAVE.
- **Feedback loop** (`feedback.cpp`): thermal margin to the nearest hot/critical kernel trip
  derates the profile (15 C -> 1 step, 7 C -> 2 steps, +5 C hysteresis, 15 s hold); sustained PSI
  pressure raises BALANCED to PERFORMANCE on AC, restricts background work on memory pressure
  and favours I/O latency on I/O pressure.
- **Stability**: a non-urgent profile change waits for a dwell time (20 s, growing up to 5 min
  when the profile flaps); urgent (thermal derate, critical battery), user-initiated and
  safety-driven changes apply immediately.
- **Not fighting other tools** (`safety/conflict_detector`, `safety/tool_inventory`): live
  processes and active systemd units for power-profiles-daemon, TuneD, auto-cpufreq,
  system76-power and TLP make overlapping subsystems `observe-only`; GameMode is game-scoped,
  and thermald is reported as a thermal owner. Package and unit inventory is read-only; installed
  but inactive tools are informational. Nothing is stopped, disabled or masked.
- `[policy] interval_ms` (default 2000, 500-60000) sets the evaluation period; `profile ...`
  triggers an immediate re-evaluation.

## Safety and rollback (Phase 5)

`src/safety/*`. Nothing here applies tuning yet (controllers: Phase 6) but every future write
is forced through it.

- **Thermal safety** (`thermal_safety`, `thermal_guard`): NORMAL -> WARM -> HOT -> EMERGENCY, run
  on its **own thread with its own sensor reads** (independent of monitor, scheduler, policy, ML).
  Limits per sensor, in priority order: hardware hwmon `crit`/`max` -> kernel thermal-zone `hot`/
  `critical` trip -> conservative fallback (100 C, CPU-class sensors only). Entry at limit-15/-8/-3 C
  (WARM/HOT/EMERGENCY); WARM/HOT need 2 consecutive readings, EMERGENCY is immediate; leaving a level
  needs 5 C more margin plus a 10 s hold counted from the last reading that still warranted it
  (one level at a time). Implausible readings are ignored; sensors that disappear after working
  push the state to at least WARM (fail-safe). Storage/Wi-Fi/battery sensors are informational.
  Thresholds are deliberately **not configurable**; only the polling interval is.
- **Effects**: WARM blocks any performance *increase*; HOT also caps the profile at BALANCED and
  disables boost; EMERGENCY forces THERMAL_EMERGENCY: the guard thread immediately applies protective throttling (CPU max frequency at 50 %, no boost, GPU max at 25 % of range) even on a subsystem another tool owns; these actions can only LOWER performance and still obey the allowlist and hardware limits. Afterwards anything no longer wanted is reverted. These apply
  to auto, manual **and locked** profiles.
- **Survives restarts**: the state is saved (`state_dir/safety.state`) and restored if younger than
  10 min; normal hysteresis then decides when to leave it.
- **Hardware limits** (`hardware_limits`): cpuinfo min/max frequency, i915 RPn..RP0, RAPL firmware max
  power. Numeric requests outside them (no overclocking) or without a known limit are refused.
- **Rollback** (`rollback.hpp`): each change is read -> journaled (old state, new state, time, reason,
  policy; fsync) -> written -> marked applied, so a crash can always be undone. Recovery runs newest
  first at startup, on `reset`, on shutdown and whenever a change is no longer wanted; a target that another tool changed
  in the meantime is left alone. A change whose current value cannot be read is refused. One record per target keeps the ORIGINAL value (chains collapse); the value read back after a write
  is journaled (the kernel may normalise it). Journal is torn-line tolerant and compacted.
- **Change gate** (`safety_gate`): target allowlist (exact tuning files, no `..`/`//`; power limits and
  cgroup.procs are not allowlisted) -> policy -> capability -> conflict (observe-only) -> thermal state ->
  hardware limits. Blocked changes log `[SAFETY]`.

## Controllers (Phase 6)

`src/control/*`. Controllers never write: each turns the policy targets into *desired changes*; the
`ControlManager` filters by ownership (observe-only subsystems are skipped and previously applied
changes released), reverts what is no longer wanted, runs every change through the gate (allowlist ->
policy -> capability -> conflict -> thermal state -> hardware limits), journals it and only then writes
through `FileStateWriter` (the only code that touches /sys and /proc/sys: allowlist, symlink resolution
and containment re-check, never creates files, one short line per write).

**BALANCED is hands-off**: it proposes nothing and restores whatever was changed before. Only POWERSAVE,
PERFORMANCE and the thermal emergency act.

| backend | what it does | notes |
|---|---|---|
| cpufreq | EPP `power`/`performance` (governor `performance` only if there is no EPP); boost off (`no_turbo`/`boost`) when not allowed; emergency: `scaling_max_freq` at 50 % | never forces the `powersave` governor; observe-only when power-profiles-daemon/TLP/TuneD own the CPU |
| gpu | Intel i915 only: POWERSAVE max at 50 % of RPn..RP0, PERFORMANCE min at 50 %, emergency max at 25 % | other drivers: nothing is guessed; values validated against hardware range |
| memory | RESPONSIVENESS: `vm.swappiness` -> 30 (only lowers, needs swap, not under memory pressure) | dirty ratios deliberately untouched |
| io | rotational disks only: `bfq` (latency) / `mq-deadline` (throughput) | NVMe/SSD/dm/md untouched |
| cgroup | background restriction = `system.slice/cpu.weight` 50 (light) / 20 (strong) | weights only matter under contention; nothing stopped; systemd owns the tree, so a value it resets makes us back off for 5 min |
| runtime-pm | **opt-in** (`runtime_pm = on`), battery + POWERSAVE only, PCI display/bridge/system/signal classes currently `on` -> `auto` | USB, audio, storage, network never touched |
| uclamp | proposes nothing (per-task attribute, no reliable foreground identity); unsupported on kernels without `CONFIG_UCLAMP_TASK` | |

If another tool changes a value we set, we leave it alone, back off for 5 minutes and never overwrite it on
revert. `apply` mode needs root (otherwise it falls back to dry-run). Leaving `apply` restores everything.
In `dry-run` thermal protective actions are also only reported; the hardware/firmware throttling remains.

## Telemetry (Phase 7)

`src/database/*`, `src/telemetry/*`. **Local only**: the service runs with `PrivateNetwork=yes` and nothing
in the code can send data anywhere.

- **Tier 1, detailed** (`telemetry` table, one row every `[telemetry] interval_ms`, default 10 s): timestamp,
  session id, device id hash, kernel version, workload + confidence, CPU utilisation/frequency, GPU
  utilisation/frequency (i915 busy is an RC6 estimate), RAM used, swap activity, PSI cpu/memory/io, temperature,
  power, energy delta, selected policy + generic target words, I/O throughput, safety action, rollback count,
  reward, success flag. `fps`, `frame_time` and `latency` have no data source yet and are stored as NULL
  (never invented).
- **Tier 2, long-term** (`summaries`): one row per (hour, workload, policy): samples, duration, avg CPU/GPU,
  avg/peak temperature, avg power, energy, latency/fps/frame time, avg reward, successes/failures.
  Complete hours are rolled up hourly (watermark in `meta`, idempotent). A detailed row is **never deleted
  before it was rolled up**, even if maintenance did not run for weeks.
- **Retention**: detailed rows `detail_retention_days` (7), summaries `summary_retention_days` (365), plus a hard
  cap `max_db_mb` (64, database + WAL): the oldest detailed rows go first, then the oldest summaries. WAL is
  checkpointed/truncated and the file compacted (incremental auto-vacuum), so the database cannot grow without bound.
- **Privacy**: only numbers, the workload class name, the profile name and generic target words are stored.
  Never process names, command lines, paths, user names or hostnames (verified in tests and on a live database).
  The device id is the first 16 hex chars of SHA-256(random per-installation salt + machine-id); the machine-id
  itself is never stored. `telemetry delete --yes` removes both tiers, compacts the file and **rotates the salt**
  so later data cannot be linked to the deleted data. Files are 0640 (salt 0600) inside the 0750 state directory.
- **Reward** (`telemetry/reward.hpp`): multi-objective and normalised before combining:
  `0.35 performance + 0.30 responsiveness - 0.15 power - 0.15 thermal - 0.05 instability`, renormalised over the
  components that are actually known (about -0.35..0.65 when everything is known). It is stored for the Phase 8/9
  learning layer; it does not influence decisions yet. `success` = thermal state below HOT and no failed change.
- **Robustness**: a corrupt database is quarantined (`.corrupt-<time>`, max 3 kept) and replaced by a fresh one; a
  telemetry failure never stops the daemon (after 5 consecutive failures it pauses for 10 minutes); the schema
  is versioned (`PRAGMA user_version`).

## ML advisor (Phase 8, optional)

`src/ml/*`. Rules stay the core; ML is a **suggestion layer** that can be switched off (`[ml] enabled = off`,
or `linux-optimizer model off`). It has no way to touch the system: its output is an `MlAdvice` that the policy
engine validates, after which any resulting change still passes thermal safety, hardware limits, conflict
checks, the change gate and rollback like every other change.

- **Models** (`model.hpp`, `fit.hpp`): two small one-hidden-layer MLPs (tanh + softmax), no dependencies.
  *workload model* (34 aggregate features -> 14 classes) and *profile model* (23 features -> POWERSAVE/BALANCED/
  PERFORMANCE). Features are numbers in [0, 1]; process names are reduced to category CPU shares and never kept.
  Model files are text with a SHA-256 checksum and strict size/finite-value checks; edited or truncated files are rejected.
- **Base model = distilled prior, not trained on your data**: the workload model is fitted on synthetic archetypes
  of every class, the profile model on the outputs of the rule table over random situations (about 99 % hold-out
  accuracy on both). It is built once at the first start (about 2 s, nice 19, interruptible), cached in
  `state_dir/ml/` and reloaded afterwards; a cache with another feature version or a damaged file is rebuilt.
  At first it therefore mostly reproduces the rules; Phase 9 (local adaptation) is what can make it differ.
- **Pessimistic confidence**: effective confidence = model probability x measured agreement with the rule engine
  (running mean over the first 50 observations, then a slow EMA, scaled by min(1, n/50)). A new installation starts
  near 0, so the rules keep priority until agreement has been demonstrated.
- **Resource-demand forecast**: Holt linear smoothing of CPU utilisation (next-interval value, trend, running RMSE);
  no training needed.
- **Modes** (`[ml] mode`): `observe` (default) records predictions and agreement and applies nothing;
  `suggest` lets an accepted suggestion move the AUTO profile by one step.
- **A suggestion is accepted only if** mode is suggest, the profile is AUTO (never manual/locked), thermal state is
  NORMAL with no derating, no urgent rule applies, aggressiveness >= 2, effective confidence >= `min_confidence`
  (0.75), it differs from the rules by exactly one step, a *higher* profile additionally needs AC power and >= 20 C
  of proven thermal headroom, and a *lower* one is refused for latency-sensitive work. Every outcome (used or why not)
  is shown by `policy`.
- Local adaptation is described in the next section.

## Local adaptation (Phase 9)

`src/ml/{trainer,training_gate,dataset,adaptation}.*`. Base model -> local adaptation -> machine-specific model.

- **What is recorded**: next to every telemetry row, one training sample (features quantised to one byte each, the rule
  classifier's class, the rule table's profile, the applied profile, whether the controllers really applied it, reward,
  success). Same database, same privacy rules, deleted by `telemetry delete`, kept `30` days / `300000` rows at most, trimmed
  first when the size cap is hit. Local learning therefore needs `[telemetry] enabled = on`.
- **When it may run** (`TrainingGate`, spec section 17): never during GAME, COMPILE, RENDER, ENCODE, DESKTOP, BROWSER, SERVER
  (or any other busy class); BACKGROUND only with a reduced duty cycle (sleeps 3x the compute time); IDLE only if the machine
  is *really* idle - CPU < 15 %, GPU < 15 %, PSI < 5, thermal state NORMAL with >= 25 C margin, >= 1 GiB and 20 % RAM free, no
  audio/latency-sensitive activity, **AC power** (battery only if `training_on_battery = on` and >= 80 %) - and has been so for
  `training_idle_seconds` (60). Any failing condition blocks at once and restarts the idle clock.
- **How it runs**: a dedicated thread at nice 19 / SCHED_IDLE, checking the gate before **every batch** (about a millisecond).
  When a workload appears it pauses, writes a **checkpoint** (weights, Adam state, RNG, shuffled order, position inside the
  epoch) and releases the CPU; it resumes from that checkpoint when the machine has been idle again - also after a daemon
  restart. Pausing and resuming gives bit-identical results to an uninterrupted run (tested). A checkpoint for other data,
  another configuration or with a bad checksum is discarded, never applied.
- **What it learns**: the *workload* model is tuned to this machine's feature ranges against the rule classifier (labels only
  when confidence >= 0.5). The *profile* model learns from outcomes, and only from samples whose profile was really applied by
  the controllers (`mode = apply`): success with reward >= 0.15 reinforces the applied profile, a failure (thermal HOT or worse,
  failed change) teaches **one step lower**, anything else is ignored. In dry-run the profile outcome says nothing about the
  profile, so profile adaptation simply does not happen. Every job replays the base prior so nothing is forgotten.
- **Promotion tests**: a candidate replaces the current model only if its weights are finite, it keeps >= 80 % on a fixed hold-out
  of the base prior, **and** it beats the current model by >= 0.5 points on this machine's own held-out samples. Otherwise it is
  rejected and the current model stays. The previous local model is kept as `.prev`.
- **Safety unchanged**: an adapted model is still just advice (observe by default, one step at most, never under thermal
  restriction, never in manual/locked); its disagreement with the rules lowers its effective confidence automatically.
- **Controls**: `linux-optimizer training` (status + learned insights per workload/profile), `training off|on|run|reset-local`.
  `reset-local` deletes local models and checkpoints and restores the base models.

## Phase 9 limits

- Not verified on real hardware over days of use; the thresholds above are initial values.
- fps / frame time / latency are still not measured, so the reward only uses CPU frequency, PSI, power and thermal margin.

## External tools and optional packages (Phase 10)

`src/safety/tool_inventory.cpp` extends process detection with read-only package and systemd
inspection. It recognises power-profiles-daemon, TuneD, auto-cpufreq, system76-power, TLP,
thermald and GameMode. Distro package state is read from dpkg, pacman local databases or RPM;
systemd state is queried with fixed-argument `systemctl list-units` / `list-unit-files` invocations
(no shell). If systemd/package metadata is unavailable, the report says so and falls back to
process/executable evidence rather than claiming a tool is absent. A live process or active unit
is an owner; an installed-but-inactive package is informational only. The daemon refreshes the
package/executable inventory every five minutes and systemd unit activity every five seconds;
`linux-optimizer conflicts` requests a fresh read-only report. If systemd is unavailable, the last
known unit state is preserved conservatively and process evidence remains in use.

The CLI also has two **local** commands which work even if the daemon is stopped:

```bash
linux-optimizer packages
linux-optimizer install sensors
linux-optimizer install thermald
```

- `packages` detects Debian/Ubuntu (`apt-get`), Arch (`pacman`) or Fedora (`dnf`) and lists
  allowlisted optional packages.
- `sensors` installs only the distro-native `lm-sensors` / `lm_sensors` package for additional
  diagnostics. It is not required; thermal monitoring still reads kernel interfaces directly.
- `thermald` is explicitly optional and can overlap with thermal management. The CLI requires a
  separate typed `INSTALL THERMALD` acknowledgement, then a second typed `YES` before invoking the
  native package manager.
- Arbitrary package names are rejected. The installer never uses a shell, never downloads or runs
  third-party scripts, and never installs automatically. Native package-manager repository policy
  remains in force. The manager may install/start the requested package's service according to the
  distribution's packaging policy; Linux Optimizer itself never stops or disables any service.
- Unsupported distributions are reported without guessing a manager or attempting installation.

## Design notes

- **Privilege split:** only `linux-optimizerd` runs as root. The CLI is an
  ordinary user program talking to `/run/linux-optimizer/control.sock`
  (mode 0666, directory 0755). The daemon reads the peer's uid via
  `SO_PEERCRED` and authorizes *per command*; unknown commands, malformed
  tokens (`[A-Za-z0-9._:=-]` only), oversize frames (1 KiB request) and slow
  clients (2 s I/O timeout) are rejected. No shell, no `system()`, no arbitrary
  sysfs access from clients.
- **Capability detection** (`src/core/capabilities.cpp`) probes `/proc` and
  `/sys` read-only, statuses: available / unavailable / unsupported / unknown.
  uclamp is judged by the `sched_util_clamp_*` sysctls + kernel >= 5.3.
  Paths are re-based on a root directory so the probes are unit-tested against
  a fake tree (kernel 4.x behaviour included).
- **Config** is a strict INI file; unknown keys fail startup (exit 78) or, on
  reload, keep the previous configuration.
- **Logging** levels TRACE..SAFETY, `key=value` fields, control characters
  neutralised; journald priority prefixes when `JOURNAL_STREAM` is set.
- **systemd:** `Type=notify` (raw `sd_notify`, no libsystemd), watchdog,
  `RuntimeDirectory`/`StateDirectory`, sandboxing incl. `PrivateNetwork=yes`
  (telemetry is local-only by design). Phase 6 will add explicit
  `ReadWritePaths=` for the controllers.

## Known limitations

- The IPC server handles one connection at a time; a client that connects and
  stays silent delays others for up to 2 s (bounded by the timeout).
- A stale socket file is replaced on start; running two daemons against the same
  socket path is not detected (systemd guarantees a single instance).
- The systemd unit passed `systemd-analyze verify` but was not run under a live
  systemd in the development sandbox.
- No real foreground-window detection (root daemon, Wayland): DESKTOP is inferred from
  compositor/session CPU activity, not from input events or the focused window.
- Classification is heuristic; unknown programs fall back to DESKTOP/BACKGROUND/IDLE. VM detection
  uses process names only; SERVER needs a known daemon name.
- GPU monitoring of i915 has no true utilization counter in sysfs (RC6-based estimate only); NVIDIA has none.
- Monitors were verified against fake trees and a container, not yet on real Alder Lake hardware.
- Thermal safety and all controllers were verified against fake /sys trees, not on real hardware
  (the development container has no cpufreq/thermal/GPU). First run on real hardware: keep `dry-run`,
  inspect `linux-optimizer controllers` and `safety`, then enable `apply`.
- Phase 10 external-tool/package inventory is heuristic and depends on package database and systemd
  visibility. A user-level GameMode daemon may be visible only via `/proc`; sandboxed or containerized
  environments can report systemd/package state as unknown. This is non-fatal and falls back to process
  and executable evidence.
- fps / frame time / end-to-end interaction latency are not yet measured on all compositor/driver
  combinations; their reward terms are omitted where data is unavailable.

## Layout

```
include/{core,ipc,daemon,platform,safety} headers   src/{core,ipc,daemon,platform,safety} implementation
cli/main.cpp                CLI              systemd/*.service.in    unit template
config/*.conf.example       sample config    tests/*.cpp             8000+ checks (incl. model training, resume equivalence, real adapter threads)
third_party/tl/expected.hpp vendored (CC0)
```
