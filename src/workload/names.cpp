#include "workload/names.hpp"

namespace opt::workload {
namespace {
using SV = std::string_view;

template <std::size_t N>
bool exact(const ProcessInfo& p, const SV (&names)[N]) {
  for (SV n : names)
    if (p.comm == n || p.exe == n) return true;
  return false;
}
template <std::size_t N>
bool starts(const ProcessInfo& p, const SV (&names)[N]) {
  for (SV n : names)
    if (p.comm.starts_with(n) || p.exe.starts_with(n)) return true;
  return false;
}
bool contains(const std::string& s, SV needle) { return s.find(needle) != std::string::npos; }

constexpr SV kCompositor[] = {"gnome-shell", "kwin_wayland", "kwin_x11", "kwin", "mutter", "xorg", "xwayland", "sway",
                              "hyprland", "weston", "picom", "wayfire", "labwc", "cosmic-comp", "river", "niri"};
constexpr SV kCompositorPrefix[] = {"mutter-x11"};
constexpr SV kBrowser[] = {"firefox", "firefox-esr", "web content", "webextensions", "isolated web co", "privileged cont",
                           "rdd process", "socket process", "gpu process", "utility process", "librewolf", "epiphany",
                           "msedge", "opera", "zen"};
constexpr SV kBrowserPrefix[] = {"chrome", "google-chrome", "chromium", "brave", "vivaldi", "webkit", "firefox"};
constexpr SV kCompileWorker[] = {"cc1", "cc1plus", "cc1obj", "gcc", "g++", "cc", "c++", "rustc", "javac", "ld", "ld.lld",
                                 "ld.gold", "ld.bfd", "lld", "collect2", "as", "nvcc", "swiftc", "ptxas", "cicc", "tsc", "zig"};
constexpr SV kCompileWorkerPrefix[] = {"clang", "x86_64-linux-gnu-", "aarch64-linux-gnu-", "cc1"};
constexpr SV kCompileOrch[] = {"ninja", "make", "gmake", "cmake", "cargo", "gradle", "mvn", "bazel", "meson", "scons",
                               "msbuild", "ccache", "sccache", "distcc"};
constexpr SV kRender[] = {"blender", "povray", "luxcoreconsole", "mitsuba", "appleseed.cli"};
constexpr SV kRenderPrefix[] = {"blender"};
constexpr SV kEncoder[] = {"ffmpeg", "handbrake", "handbrakecli", "x264", "x265", "svtav1encapp", "aomenc", "rav1e",
                           "gst-launch-1.0", "gst-launch", "mencoder"};
constexpr SV kArchiver[] = {"tar", "zstd", "zstdmt", "xz", "unxz", "gzip", "gunzip", "bzip2", "bunzip2", "7z", "7za", "7zr",
                            "zip", "unzip", "pigz", "pbzip2", "lz4", "rar", "unrar", "bsdtar", "zpaq"};
constexpr SV kCopy[] = {"cp", "rsync", "mv", "dd", "rclone", "pv"};
constexpr SV kVm[] = {"kvm", "virtualboxvm", "vboxheadless", "vmware-vmx", "firecracker", "crosvm"};
constexpr SV kVmPrefix[] = {"qemu", "cloud-hyperviso"};
constexpr SV kPlayer[] = {"mpv", "vlc", "totem", "showtime", "celluloid", "smplayer", "mplayer", "haruna", "kodi", "kodi.bin",
                          "parole", "gst-play-1.0", "ffplay", "stremio"};
constexpr SV kServer[] = {"nginx", "apache2", "httpd", "caddy", "haproxy", "postgres", "mysqld", "mariadbd", "redis-server",
                          "mongod", "memcached", "gunicorn", "uwsgi", "named", "dovecot", "smbd", "nmbd", "jellyfin",
                          "plexmediaserver", "etcd", "prometheus", "grafana", "grafana-server", "influxd", "vsftpd",
                          "proftpd", "lighttpd", "traefik", "envoy"};
constexpr SV kServerPrefix[] = {"php-fpm"};
constexpr SV kLauncher[] = {"steam", "reaper", "lutris", "heroic", "bottles"};
constexpr SV kLauncherPrefix[] = {"gamescope", "pressure-vessel"};
constexpr SV kEmulator[] = {"retroarch", "dolphin-emu", "yuzu", "ryujinx", "pcsx2", "pcsx2-qt", "rpcs3", "ppsspp", "ppssppsdl",
                            "duckstation", "duckstation-qt", "cemu", "melonds", "mgba", "citra", "xemu", "flycast"};
constexpr SV kWineHelpers[] = {"services.exe", "winedevice.exe", "explorer.exe", "rpcss.exe", "svchost.exe", "plugplay.exe",
                               "tabtip.exe", "conhost.exe", "start.exe", "wineboot.exe", "winemenubuilder.exe"};
constexpr SV kContainerRuntime[] = {"dockerd", "containerd", "podman", "conmon", "crio", "runc", "docker-proxy"};
constexpr SV kContainerRuntimePrefix[] = {"containerd-shim"};
}  // namespace

bool is_compositor(const ProcessInfo& p) { return exact(p, kCompositor) || starts(p, kCompositorPrefix); }
bool is_browser(const ProcessInfo& p) { return exact(p, kBrowser) || starts(p, kBrowserPrefix); }
bool is_compile_worker(const ProcessInfo& p) { return exact(p, kCompileWorker) || starts(p, kCompileWorkerPrefix); }
bool is_compile_orchestrator(const ProcessInfo& p) { return exact(p, kCompileOrch); }
bool is_render(const ProcessInfo& p) { return exact(p, kRender) || starts(p, kRenderPrefix); }
bool is_encoder(const ProcessInfo& p) { return exact(p, kEncoder); }
bool is_archiver(const ProcessInfo& p) { return exact(p, kArchiver); }
bool is_copy_tool(const ProcessInfo& p) { return exact(p, kCopy); }
bool is_vm(const ProcessInfo& p) { return exact(p, kVm) || starts(p, kVmPrefix); }
bool is_media_player(const ProcessInfo& p) { return exact(p, kPlayer); }
bool is_server_daemon(const ProcessInfo& p) { return exact(p, kServer) || starts(p, kServerPrefix); }
bool is_launcher(const ProcessInfo& p) { return exact(p, kLauncher) || starts(p, kLauncherPrefix); }
bool is_emulator(const ProcessInfo& p) { return exact(p, kEmulator); }
bool is_container_runtime(const ProcessInfo& p) { return exact(p, kContainerRuntime) || starts(p, kContainerRuntimePrefix); }

bool is_wine_game_candidate(const ProcessInfo& p) {
  if (!p.comm.ends_with(".exe")) return false;
  for (SV h : kWineHelpers)
    if (p.comm == h) return false;
  return true;
}

bool in_container_cgroup(const ProcessInfo& p) {
  const std::string& c = p.cgroup;
  return contains(c, "/docker/") || contains(c, "docker-") || contains(c, "libpod-") || contains(c, "crio-") ||
         contains(c, "cri-containerd") || contains(c, "/kubepods") || contains(c, "machine.slice");
}
bool in_system_slice(const ProcessInfo& p) { return contains(p.cgroup, "system.slice"); }
bool in_user_slice(const ProcessInfo& p) { return contains(p.cgroup, "user.slice"); }

}  // namespace opt::workload
