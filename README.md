# Linux Optimizer

An adaptive power and performance daemon for Linux, written in C++20.

`linux-optimizerd` runs as a systemd service. It watches the machine, works out what you are doing
(idle, browsing, gaming, compiling, ...), picks a profile, and can apply it through a small set of
gated, journaled controllers. Every change it makes can be undone, and thermal safety always wins.

**Version:** 0.10.2 &nbsp;|&nbsp; **Status:** pre-release. See [Status and limitations](#status-and-limitations).

> **Safe by default.** Controllers start in `dry-run`: they compute and log what *would* change and
> write nothing. Nothing touches `/sys` or `/proc/sys` until you opt in with `apply`.

## Features

- **Monitoring**: CPU, memory, PSI, thermal zones, power supply/RAPL, disk I/O and GPU, read from
  `/proc` and `/sys`. Missing interfaces are skipped, not treated as errors.
- **Workload classification**: IDLE, DESKTOP, BROWSER, VIDEO_PLAYBACK, GAME, COMPILE, RENDER, ENCODE,
  FILE_COPY, ARCHIVE, VM, CONTAINER, SERVER, BACKGROUND, with explainable scores and hysteresis.
  A game is never decided from a single signal.
- **Policy engine**: profiles `powersave`, `balanced`, `performance` and `auto`, plus an internal thermal
  emergency profile. Aggressiveness 1 to 4 (default 3).
- **Controllers**: cpufreq (EPP, boost), Intel i915 GPU limits, `vm.swappiness`, I/O scheduler for
  rotational disks, cgroup CPU weight, and opt-in PCI runtime PM.
- **Safety and rollback**: independent thermal guard, hardware limits, change allowlist, and a
  crash-safe rollback journal.
- **Coexistence**: power-profiles-daemon, TuneD, auto-cpufreq, system76-power and TLP make overlapping
  subsystems *observe-only*. Nothing is stopped, disabled or masked.
- **Local telemetry and optional ML**: SQLite on disk, never sent anywhere. The ML advisor is
  observe-only by default and has no write access to the system.

## Requirements

- Linux with systemd (the daemon also runs without it for testing)
- g++ 11+ or clang 14+, CMake 3.20+, Make or Ninja
- SQLite3 development files (`libsqlite3-dev`, `sqlite-devel`, or `sqlite` on Arch)

No other dependencies: no libsystemd, no OpenSSL, no test framework.

## Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests use fake `/proc` and `/sys` trees and never write to the real ones.

## Try it without installing (no root)

```bash
cat > /tmp/opt.conf <<'CONF'
[ipc]
socket_path = /tmp/opt/control.sock
CONF
./build/linux-optimizerd --config /tmp/opt.conf --check    # validate and print capabilities
./build/linux-optimizerd --config /tmp/opt.conf &
export LINUX_OPTIMIZER_SOCKET=/tmp/opt/control.sock
./build/linux-optimizer status
```

## Install as a service

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build
sudo cmake --install build
sudo cp /etc/linux-optimizer/linux-optimizer.conf.example /etc/linux-optimizer/linux-optimizer.conf
sudo groupadd -f linux-optimizer      # optional: members may run admin commands
sudo systemctl daemon-reload
sudo systemctl enable --now linux-optimizerd
linux-optimizer status
```

### First run on real hardware

1. Leave controllers in `dry-run`.
2. Run `linux-optimizer controllers` and `linux-optimizer safety` and read what it *would* do.
3. Only then set `[controllers] mode = apply` in the config, or run
   `sudo linux-optimizer controllers apply` for the current run only.
4. `sudo linux-optimizer reset` rolls back every change the daemon made.

## Commands

| Command | Who | Purpose |
|---|---|---|
| `status`, `detect`, `ping`, `version`, `help` | everyone | overview and live probe |
| `profile`, `policy [--watch]` | everyone | current profile, decision, per-subsystem control and reasons |
| `profile powersave\|balanced\|performance\|auto [--lock\|--unlock]` | root / group | set the mode; lock freezes adaptation |
| `workload`, `monitor` (`--watch`) | everyone | current class and scores, latest samples |
| `controllers [off\|dry-run\|apply]` | everyone / root to set | what controllers did or would do, outstanding changes |
| `safety` | everyone | thermal state, sensors, limits, rollback journal |
| `conflicts` | everyone | other tools and who owns which subsystem (read-only) |
| `reset` | root / group | profile back to auto, roll back every change |
| `model`, `training` | everyone / root to change | ML advisor and local adaptation |
| `history`, `telemetry [on\|off\|delete --yes]` | everyone / root to change | local history; pause or delete |
| `set-log-level`, `reload-config` | root / group | daemon-internal |
| `packages`, `install sensors\|thermald` | local CLI | read-only package report; allowlisted optional installs with typed confirmation |

Exit codes: `0` ok, `1` daemon returned an error, `2` usage/protocol, `3` daemon unreachable.

## How changes are made

```
monitor -> workload -> policy -> controllers (state INTENT only)
        -> ControlManager: ownership, rollback, gate -> journal -> write -> verify
```

- Controllers never write. Each one states what the current policy **wants** (its *desired* set).
  The `ControlManager` decides what actually needs writing (the *pending* changes).
- A change is read, journaled with its original value (fsynced), written, and read back. After a crash
  the journal can always be undone. One record per target keeps the **original** value.
- A target is rolled back only when policy genuinely lets go of it: the profile changes, the
  subsystem becomes observe-only, or a needed capability disappears. A value that already meets the
  goal is simply kept; it is not written again and not reverted.
- If another tool changes a value the daemon set, the daemon leaves it alone, backs off for 5 minutes,
  and never overwrites it on rollback.
- If a restore fails, it is retried with a backoff instead of on every cycle.
- All writes pass one gate: target allowlist, policy, capability, conflict, thermal state, hardware
  limits. Only `FileStateWriter` touches `/sys` and `/proc/sys`.

### Controllers

| Backend | Behaviour |
|---|---|
| cpufreq | EPP `power`/`performance`; boost off when not allowed; never forces the `powersave` governor |
| gpu | Intel i915 only; values validated against the hardware range |
| memory | Responsiveness: `vm.swappiness` at most 30, only if swap exists. It never raises a lower value, and under memory pressure it keeps what it already set but starts nothing new |
| io | rotational disks only (`bfq` / `mq-deadline`); NVMe, SSD, dm and md are untouched |
| cgroup | `system.slice/cpu.weight` 50 or 20 for background restriction; nothing is stopped |
| runtime-pm | **opt-in**; battery plus powersave only; PCI display, bridge, system and signal classes |

`balanced` is hands-off: it proposes nothing and restores whatever was changed before.

## Thermal safety

A dedicated thread reads its own sensors, independent of the monitor, policy and ML.

- States: NORMAL, WARM, HOT, EMERGENCY, with hysteresis. Thresholds are deliberately **not configurable**.
- WARM blocks any performance increase. HOT also caps the profile at `balanced` and disables boost.
  EMERGENCY immediately applies protective throttling, even on a subsystem another tool owns, and these
  actions can only lower performance.
- If sensors disappear after working, the state moves to at least WARM (fail-safe). No temperatures are invented.
- The state survives restarts for 10 minutes.
- Numeric requests outside the hardware range, or without a known limit, are refused. There is no overclocking.

## Privacy and security

- **Local only.** The service runs with `PrivateNetwork=yes`; nothing in the code sends data anywhere.
- Telemetry stores numbers, workload class names and profile names. It never stores process names,
  command lines, paths, user names or hostnames. `linux-optimizer telemetry delete --yes` removes
  everything and rotates the device-id salt.
- Only the daemon runs as root. The CLI is an ordinary program talking to a Unix socket; the daemon
  authorizes **each command** by the peer's uid (`SO_PEERCRED`). Requests are strictly validated and
  capped at 1 KiB, and clients cannot name arbitrary files to write.
- To report a vulnerability, please contact the maintainers privately instead of opening a public issue.

## Configuration

Copy `config/linux-optimizer.conf.example` to `/etc/linux-optimizer/linux-optimizer.conf`. The file is
strict INI: unknown keys stop the daemon at startup, or keep the previous configuration on reload.
Reload with `linux-optimizer reload-config` or `SIGHUP`.

## Status and limitations

This project is **pre-release**. Please read this before enabling `apply`.

- Thermal safety and all controllers were verified against fake `/sys` trees, **not** over days of
  real use. Thresholds are initial values.
- Tested design target: Intel CPUs with `intel_pstate` and i915 graphics. Other drivers are
  detected and skipped rather than guessed.
- Classification is heuristic. There is no real foreground-window detection on Wayland.
- fps, frame time and latency are not measured, so the ML reward uses CPU frequency, PSI, power and thermal margin only.
- The IPC server handles one connection at a time; a silent client delays others for up to 2 s.
- The systemd unit passed `systemd-analyze verify` but has not been exercised in every environment.

## Repository layout

```
include/ src/      core, ipc, daemon, monitor, workload, policy, safety, control, database, telemetry, ml
cli/main.cpp       command-line client
systemd/           service unit template
config/            example configuration
tests/             unit tests (fake /proc and /sys, no real writes)
third_party/tl/    vendored tl::expected (CC0)
```

## Contributing

Bug reports and patches are welcome. For changes to controllers or safety code, please include a test
that runs against a fake root and does not write to the real `/sys` or `/proc`.

## License

See [LICENSE](LICENSE). The vendored `third_party/tl/expected.hpp` keeps its own CC0 dedication.
