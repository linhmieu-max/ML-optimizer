#include "core/config.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/un.h>

namespace opt {
namespace {

constexpr std::size_t kMaxConfigBytes = 64 * 1024;

std::string_view trim(std::string_view s) noexcept {
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
  while (!s.empty() && ws(s.front())) s.remove_prefix(1);
  while (!s.empty() && ws(s.back())) s.remove_suffix(1);
  return s;
}

Error line_error(int line, const std::string& msg) {
  return make_error(Errc::Parse, "config line " + std::to_string(line) + ": " + msg);
}

bool valid_group_name(std::string_view g) noexcept {
  if (g.empty() || g.size() > 32) return false;
  for (char c : g) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

Result<void> apply(Config& c, std::string_view section, std::string_view key, std::string_view value,
                   int line) {
  const std::string where = std::string(section) + "." + std::string(key);
  if (section == "daemon") {
    if (key == "log_level") {
      auto lvl = log::parse_level(value);
      if (!lvl) return fail(line_error(line, "invalid log level '" + std::string(value) + "'"));
      c.log_level = *lvl;
      return {};
    }
    if (key == "state_dir") {
      c.state_dir = std::string(value);
      return {};
    }
  } else if (section == "ipc") {
    if (key == "socket_path") {
      c.socket_path = std::string(value);
      return {};
    }
    if (key == "admin_group") {
      c.admin_group = std::string(value);
      return {};
    }
  } else if (section == "monitor") {
    if (key == "interval_ms") {
      int n = 0;
      const auto* end = value.data() + value.size();
      auto [p, ec] = std::from_chars(value.data(), end, n);
      if (ec != std::errc{} || p != end) return fail(line_error(line, "interval_ms must be an integer"));
      c.monitor_interval_ms = n;
      return {};
    }
  } else if (section == "workload") {
    if (key == "interval_ms") {
      int n = 0;
      const auto* end = value.data() + value.size();
      auto [p, ec] = std::from_chars(value.data(), end, n);
      if (ec != std::errc{} || p != end) return fail(line_error(line, "interval_ms must be an integer"));
      c.workload_interval_ms = n;
      return {};
    }
  } else if (section == "controllers") {
    if (key == "mode") {
      if (value != "off" && value != "dry-run" && value != "apply")
        return fail(line_error(line, "controllers.mode must be off, dry-run or apply"));
      c.controllers_mode = std::string(value);
      return {};
    }
    if (key == "runtime_pm") {
      if (value != "on" && value != "off") return fail(line_error(line, "controllers.runtime_pm must be on or off"));
      c.runtime_pm = value == "on";
      return {};
    }
  } else if (section == "telemetry") {
    auto num = [&](int& out) -> Result<void> {
      int n = 0;
      const auto* end = value.data() + value.size();
      auto [p, ec] = std::from_chars(value.data(), end, n);
      if (ec != std::errc{} || p != end) return fail(line_error(line, std::string(key) + " must be an integer"));
      out = n;
      return {};
    };
    if (key == "enabled") {
      if (value != "on" && value != "off") return fail(line_error(line, "telemetry.enabled must be on or off"));
      c.telemetry_enabled = value == "on";
      return {};
    }
    if (key == "interval_ms") return num(c.telemetry_interval_ms);
    if (key == "detail_retention_days") return num(c.telemetry_detail_days);
    if (key == "summary_retention_days") return num(c.telemetry_summary_days);
    if (key == "max_db_mb") return num(c.telemetry_max_db_mb);
  } else if (section == "ml") {
    if (key == "enabled") {
      if (value != "on" && value != "off") return fail(line_error(line, "ml.enabled must be on or off"));
      c.ml_enabled = value == "on";
      return {};
    }
    if (key == "mode") {
      if (value != "observe" && value != "suggest") return fail(line_error(line, "ml.mode must be observe or suggest"));
      c.ml_mode = std::string(value);
      return {};
    }
    if (key == "training" || key == "training_on_battery") {
      if (value != "on" && value != "off") return fail(line_error(line, "ml." + std::string(key) + " must be on or off"));
      (key == "training" ? c.ml_training : c.ml_training_on_battery) = value == "on";
      return {};
    }
    if (key == "training_min_samples" || key == "training_interval_hours" || key == "training_idle_seconds") {
      int n = 0;
      const auto* end = value.data() + value.size();
      auto [p, ec] = std::from_chars(value.data(), end, n);
      if (ec != std::errc{} || p != end) return fail(line_error(line, "ml." + std::string(key) + " must be an integer"));
      (key == "training_min_samples" ? c.ml_training_min_samples : (key == "training_interval_hours" ? c.ml_training_interval_hours : c.ml_training_idle_seconds)) = n;
      return {};
    }
    if (key == "min_confidence") {
      const std::string t(value);
      char* end = nullptr;
      const double v = std::strtod(t.c_str(), &end);
      if (t.empty() || *end != '\0') return fail(line_error(line, "min_confidence must be a number"));
      c.ml_min_confidence = v;
      return {};
    }
  } else if (section == "safety") {
    if (key == "interval_ms") {
      int n = 0;
      const auto* end = value.data() + value.size();
      auto [p, ec] = std::from_chars(value.data(), end, n);
      if (ec != std::errc{} || p != end) return fail(line_error(line, "interval_ms must be an integer"));
      c.safety_interval_ms = n;
      return {};
    }
  } else if (section == "policy") {
    if (key == "aggressiveness") {
      int n = 0;
      const auto* end = value.data() + value.size();
      auto [p, ec] = std::from_chars(value.data(), end, n);
      if (ec != std::errc{} || p != end) return fail(line_error(line, "aggressiveness must be an integer"));
      c.aggressiveness = n;
      return {};
    }
    if (key == "interval_ms") {
      int n = 0;
      const auto* end = value.data() + value.size();
      auto [p, ec] = std::from_chars(value.data(), end, n);
      if (ec != std::errc{} || p != end) return fail(line_error(line, "interval_ms must be an integer"));
      c.policy_interval_ms = n;
      return {};
    }
  }
  return fail(line_error(line, "unknown setting '" + where + "'"));
}

}  // namespace

Result<void> validate_config(const Config& c) {
  if (!c.socket_path.is_absolute() || c.socket_path.filename().empty())
    return fail(make_error(Errc::InvalidArgument, "ipc.socket_path must be an absolute file path"));
  if (c.socket_path.string().size() >= sizeof(sockaddr_un::sun_path))
    return fail(make_error(Errc::InvalidArgument, "ipc.socket_path is too long for a Unix socket"));
  if (!c.state_dir.is_absolute())
    return fail(make_error(Errc::InvalidArgument, "daemon.state_dir must be an absolute path"));
  if (c.aggressiveness < kAggressivenessMin || c.aggressiveness > kAggressivenessMax)
    return fail(make_error(Errc::InvalidArgument, "policy.aggressiveness must be between 1 and 4"));
  if (c.monitor_interval_ms < 250 || c.monitor_interval_ms > 60000)
    return fail(make_error(Errc::InvalidArgument, "monitor.interval_ms must be between 250 and 60000"));
  if (c.workload_interval_ms < 500 || c.workload_interval_ms > 60000)
    return fail(make_error(Errc::InvalidArgument, "workload.interval_ms must be between 500 and 60000"));
  if (c.policy_interval_ms < 500 || c.policy_interval_ms > 60000)
    return fail(make_error(Errc::InvalidArgument, "policy.interval_ms must be between 500 and 60000"));
  if (c.safety_interval_ms < 250 || c.safety_interval_ms > 10000)
    return fail(make_error(Errc::InvalidArgument, "safety.interval_ms must be between 250 and 10000"));
  if (c.telemetry_interval_ms < 5000 || c.telemetry_interval_ms > 300000)
    return fail(make_error(Errc::InvalidArgument, "telemetry.interval_ms must be between 5000 and 300000"));
  if (c.telemetry_detail_days < 1 || c.telemetry_detail_days > 90)
    return fail(make_error(Errc::InvalidArgument, "telemetry.detail_retention_days must be between 1 and 90"));
  if (c.telemetry_summary_days < 30 || c.telemetry_summary_days > 3650)
    return fail(make_error(Errc::InvalidArgument, "telemetry.summary_retention_days must be between 30 and 3650"));
  if (c.telemetry_max_db_mb < 4 || c.telemetry_max_db_mb > 1024)
    return fail(make_error(Errc::InvalidArgument, "telemetry.max_db_mb must be between 4 and 1024"));
  if (!(c.ml_min_confidence >= 0.5 && c.ml_min_confidence <= 0.99))
    return fail(make_error(Errc::InvalidArgument, "ml.min_confidence must be between 0.5 and 0.99"));
  if (c.ml_training_min_samples < 100 || c.ml_training_min_samples > 100000)
    return fail(make_error(Errc::InvalidArgument, "ml.training_min_samples must be between 100 and 100000"));
  if (c.ml_training_interval_hours < 1 || c.ml_training_interval_hours > 720)
    return fail(make_error(Errc::InvalidArgument, "ml.training_interval_hours must be between 1 and 720"));
  if (c.ml_training_idle_seconds < 10 || c.ml_training_idle_seconds > 3600)
    return fail(make_error(Errc::InvalidArgument, "ml.training_idle_seconds must be between 10 and 3600"));
  if (!valid_group_name(c.admin_group))
    return fail(make_error(Errc::InvalidArgument, "ipc.admin_group is not a valid group name"));
  return {};
}

Result<Config> parse_config(std::string_view text, Config base) {
  std::string section;
  std::istringstream in{std::string(text)};
  std::string raw;
  int line = 0;
  while (std::getline(in, raw)) {
    ++line;
    std::string_view s = trim(raw);
    if (s.empty() || s.front() == '#' || s.front() == ';') continue;
    if (s.front() == '[') {
      if (s.back() != ']') return fail(line_error(line, "unterminated section header"));
      section = std::string(trim(s.substr(1, s.size() - 2)));
      if (section != "daemon" && section != "ipc" && section != "monitor" && section != "workload" && section != "controllers" && section != "telemetry" && section != "ml" && section != "safety" && section != "policy")
        return fail(line_error(line, "unknown section [" + section + "]"));
      continue;
    }
    const auto eq = s.find('=');
    if (eq == std::string_view::npos) return fail(line_error(line, "expected key = value"));
    if (section.empty()) return fail(line_error(line, "setting outside of a section"));
    auto r = apply(base, section, trim(s.substr(0, eq)), trim(s.substr(eq + 1)), line);
    if (!r) return fail(r.error());
  }
  auto v = validate_config(base);
  if (!v) return fail(v.error());
  return base;
}

Result<Config> load_config(const std::filesystem::path& file, bool* found) {
  if (found) *found = false;
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    Config def;
    auto v = validate_config(def);
    if (!v) return fail(v.error());
    return def;
  }
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return fail(make_error(Errc::Io, "cannot stat " + file.string(), ec.value()));
  if (size > kMaxConfigBytes) return fail(make_error(Errc::InvalidArgument, "config file too large"));
  std::ifstream f(file);
  if (!f) return fail(errno_error(Errc::Io, "cannot open " + file.string()));
  std::ostringstream buf;
  buf << f.rdbuf();
  if (found) *found = true;
  auto cfg = parse_config(buf.str());
  if (!cfg) return fail(make_error(cfg.error().code, file.string() + ": " + cfg.error().message));
  return cfg;
}

}  // namespace opt
