#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

#include "core/version.hpp"
#include "daemon/daemon.hpp"

namespace {
void usage(FILE* out) {
  std::fprintf(out,
               "linux-optimizerd %s (Phase %d)\n"
               "usage: linux-optimizerd [--config FILE] [--check] [--version] [--help]\n"
               "  --config FILE  configuration file (default %s)\n"
               "  --check        validate configuration, print detected capabilities, exit\n",
               opt::kVersion, opt::kCurrentPhase, opt::kDefaultConfigPath);
}
}  // namespace

int main(int argc, char** argv) {
  opt::DaemonOptions options;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!std::strcmp(a, "--config") && i + 1 < argc) options.config_path = argv[++i];
    else if (!std::strcmp(a, "--check")) options.check_only = true;
    else if (!std::strcmp(a, "--version")) { std::printf("linux-optimizerd %s\n", opt::kVersion); return 0; }
    else if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) { usage(stdout); return 0; }
    else { usage(stderr); return 2; }
  }

  // Under systemd, stderr is the journal: use priority prefixes instead of timestamps.
  opt::log::set_journal_mode(std::getenv("JOURNAL_STREAM") != nullptr);

  // Block termination signals in every thread before any thread exists;
  // Daemon::run() consumes them synchronously with sigtimedwait().
  std::signal(SIGPIPE, SIG_IGN);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  opt::Daemon daemon(std::move(options));
  return daemon.run();
}
