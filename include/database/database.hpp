#pragma once
// Thin RAII wrapper over SQLite. Every failure is returned as opt::Error; nothing throws.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace opt::db {

class Statement {
 public:
  Statement(Statement&& o) noexcept : st_(o.st_) { o.st_ = nullptr; }
  Statement& operator=(Statement&&) = delete;
  ~Statement();
  Statement(const Statement&) = delete;

  Statement& bind(int idx, std::int64_t v);
  Statement& bind(int idx, double v);                       // NaN/inf are stored as NULL
  Statement& bind(int idx, std::string_view v);              // text
  Statement& bind_blob(int idx, std::string_view v);
  Statement& bind(int idx, const std::optional<double>& v);  // nullopt -> NULL
  Statement& bind_null(int idx);

  Result<bool> step();  // true = a row is available, false = done
  void reset();
  std::int64_t col_int(int i) const;
  double col_double(int i) const;
  std::string col_text(int i) const;
  std::string col_text_blob(int i) const;  // raw bytes (BLOB columns)
  bool col_null(int i) const;

 private:
  friend class Database;
  explicit Statement(sqlite3_stmt* st) : st_(st) {}
  sqlite3_stmt* st_;
};

class Database {
 public:
  /// ":memory:" is allowed. Opens read/write, creates the file (mode 0640), WAL, busy timeout,
  /// incremental auto-vacuum on new databases, and runs `PRAGMA quick_check` (corrupt => error).
  static Result<std::unique_ptr<Database>> open(const std::filesystem::path& file);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  Result<void> exec(const std::string& sql);
  Result<Statement> prepare(const std::string& sql);
  Result<std::int64_t> scalar_int(const std::string& sql);
  Result<std::string> scalar_text(const std::string& sql);
  std::int64_t changes() const;
  const std::filesystem::path& path() const { return path_; }

 private:
  Database() = default;
  sqlite3* db_ = nullptr;
  std::filesystem::path path_;
};

}  // namespace opt::db
