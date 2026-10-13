#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace opt::log {

enum class Level { Trace, Debug, Info, Warn, Error, Safety };

std::string_view to_string(Level) noexcept;
std::optional<Level> parse_level(std::string_view name) noexcept;  // case-insensitive

struct Field {
  std::string_view key;
  std::string value;
};

void set_level(Level) noexcept;
Level level() noexcept;
bool enabled(Level) noexcept;

/// In journal mode lines carry a "<prio>" prefix (sd-daemon(3)) and no timestamp,
/// because journald adds its own. Otherwise lines get an ISO-8601 UTC timestamp.
void set_journal_mode(bool) noexcept;

/// Thread-safe. Control characters in message/values are neutralised so callers
/// cannot forge extra log lines.
void write(Level, std::string_view component, std::string_view message,
           std::initializer_list<Field> fields = {});

inline void trace(std::string_view c, std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Trace, c, m, f); }
inline void debug(std::string_view c, std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Debug, c, m, f); }
inline void info(std::string_view c, std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Info, c, m, f); }
inline void warn(std::string_view c, std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Warn, c, m, f); }
inline void error(std::string_view c, std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Error, c, m, f); }
inline void safety(std::string_view c, std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Safety, c, m, f); }

}  // namespace opt::log
