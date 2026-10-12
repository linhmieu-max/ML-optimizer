#include "core/logging.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace opt::log {
namespace {

std::atomic<Level> g_level{Level::Info};
std::atomic<bool> g_journal{false};
std::mutex g_write_mutex;

int syslog_priority(Level l) noexcept {
  switch (l) {
    case Level::Trace:
    case Level::Debug: return 7;
    case Level::Info: return 6;
    case Level::Warn: return 4;
    case Level::Error: return 3;
    case Level::Safety: return 2;
  }
  return 6;
}

bool is_control(char c) noexcept { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; }

void append_clean(std::string& out, std::string_view s) {
  for (char c : s) out.push_back(is_control(c) ? ' ' : c);
}

void append_value(std::string& out, std::string_view v) {
  bool quote = v.empty();
  for (char c : v) {
    if (c == ' ' || c == '"' || c == '=' || c == '\\' || is_control(c)) {
      quote = true;
      break;
    }
  }
  if (!quote) {
    out.append(v);
    return;
  }
  out.push_back('"');
  for (char c : v) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else {
      out.push_back(is_control(c) ? ' ' : c);
    }
  }
  out.push_back('"');
}

}  // namespace

std::string_view to_string(Level l) noexcept {
  switch (l) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Safety: return "SAFETY";
  }
  return "INFO";
}

std::optional<Level> parse_level(std::string_view name) noexcept {
  std::string lower;
  for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (lower == "trace") return Level::Trace;
  if (lower == "debug") return Level::Debug;
  if (lower == "info") return Level::Info;
  if (lower == "warn" || lower == "warning") return Level::Warn;
  if (lower == "error") return Level::Error;
  if (lower == "safety") return Level::Safety;
  return std::nullopt;
}

void set_level(Level l) noexcept { g_level.store(l, std::memory_order_relaxed); }
Level level() noexcept { return g_level.load(std::memory_order_relaxed); }
bool enabled(Level l) noexcept { return static_cast<int>(l) >= static_cast<int>(level()); }
void set_journal_mode(bool on) noexcept { g_journal.store(on, std::memory_order_relaxed); }

void write(Level lvl, std::string_view component, std::string_view message,
           std::initializer_list<Field> fields) {
  if (!enabled(lvl)) return;

  std::string line;
  line.reserve(128);
  if (g_journal.load(std::memory_order_relaxed)) {
    line += '<';
    line += std::to_string(syslog_priority(lvl));
    line += '>';
  } else {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    line += buf;
    line += ' ';
  }
  line += '[';
  line += to_string(lvl);
  line += "] ";
  append_clean(line, component);
  line += ": ";
  append_clean(line, message);
  for (const Field& f : fields) {
    line += ' ';
    append_clean(line, f.key);
    line += '=';
    append_value(line, f.value);
  }
  line += '\n';

  std::lock_guard<std::mutex> lock(g_write_mutex);
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
}

}  // namespace opt::log
