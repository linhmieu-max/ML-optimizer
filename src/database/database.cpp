#include "database/database.hpp"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cmath>

namespace opt::db {
namespace {
Error sqlite_error(sqlite3* db, const std::string& what) {
  return make_error(Errc::Io, "sqlite: " + what + ": " + (db ? sqlite3_errmsg(db) : "no connection"));
}
}  // namespace

Statement::~Statement() {
  if (st_) sqlite3_finalize(st_);
}
Statement& Statement::bind(int i, std::int64_t v) { sqlite3_bind_int64(st_, i, v); return *this; }
Statement& Statement::bind(int i, double v) {
  if (std::isfinite(v)) sqlite3_bind_double(st_, i, v);
  else sqlite3_bind_null(st_, i);
  return *this;
}
Statement& Statement::bind(int i, std::string_view v) {
  sqlite3_bind_text(st_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  return *this;
}
Statement& Statement::bind_blob(int i, std::string_view v) {
  sqlite3_bind_blob(st_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
  return *this;
}
Statement& Statement::bind(int i, const std::optional<double>& v) { return v ? bind(i, *v) : bind_null(i); }
Statement& Statement::bind_null(int i) { sqlite3_bind_null(st_, i); return *this; }
void Statement::reset() {
  sqlite3_reset(st_);
  sqlite3_clear_bindings(st_);
}
Result<bool> Statement::step() {
  const int rc = sqlite3_step(st_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  return fail(sqlite_error(sqlite3_db_handle(st_), "step"));
}
std::int64_t Statement::col_int(int i) const { return sqlite3_column_int64(st_, i); }
double Statement::col_double(int i) const { return sqlite3_column_double(st_, i); }
bool Statement::col_null(int i) const { return sqlite3_column_type(st_, i) == SQLITE_NULL; }
std::string Statement::col_text(int i) const {
  const auto* t = sqlite3_column_text(st_, i);
  return t ? reinterpret_cast<const char*>(t) : "";
}

std::string Statement::col_text_blob(int i) const {
  const void* p = sqlite3_column_blob(st_, i);
  const int n = sqlite3_column_bytes(st_, i);
  return p && n > 0 ? std::string(static_cast<const char*>(p), static_cast<std::size_t>(n)) : std::string();
}

Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& file) {
  std::unique_ptr<Database> d(new Database());
  d->path_ = file;
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
  const bool memory = file == ":memory:";
  if (!memory) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
  }
  const mode_t old_umask = ::umask(0027);  // db, -wal and -shm are created 0640 at most
  const int rc = sqlite3_open_v2(file.c_str(), &d->db_, flags, nullptr);
  ::umask(old_umask);
  if (rc != SQLITE_OK) {
    auto e = sqlite_error(d->db_, "cannot open " + file.string());
    if (d->db_) sqlite3_close(d->db_);
    d->db_ = nullptr;
    return fail(std::move(e));
  }
  sqlite3_busy_timeout(d->db_, 2000);
  // auto_vacuum only takes effect on a brand-new (still empty) database.
  if (auto n = d->scalar_int("PRAGMA page_count"); n && *n == 0) {
    if (auto r = d->exec("PRAGMA auto_vacuum=INCREMENTAL"); !r) return fail(r.error());
  }
  for (const char* p : {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL", "PRAGMA trusted_schema=OFF", "PRAGMA secure_delete=ON"})
    if (auto r = d->scalar_text(p); !r) return fail(r.error());
  auto check = d->scalar_text("PRAGMA quick_check");
  if (!check) return fail(check.error());
  if (*check != "ok") return fail(make_error(Errc::Io, "sqlite: database is corrupt: " + *check));
  return d;
}

Database::~Database() {
  if (db_) sqlite3_close(db_);
}

Result<void> Database::exec(const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string m = err ? err : "?";
    sqlite3_free(err);
    return fail(make_error(Errc::Io, "sqlite: " + m + " [" + sql.substr(0, 60) + "]"));
  }
  return {};
}

Result<Statement> Database::prepare(const std::string& sql) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return fail(sqlite_error(db_, "prepare [" + sql.substr(0, 60) + "]"));
  return Statement(st);
}

Result<std::int64_t> Database::scalar_int(const std::string& sql) {
  auto st = prepare(sql);
  if (!st) return fail(st.error());
  auto row = st->step();
  if (!row) return fail(row.error());
  return *row ? st->col_int(0) : 0;
}

Result<std::string> Database::scalar_text(const std::string& sql) {
  auto st = prepare(sql);
  if (!st) return fail(st.error());
  auto row = st->step();
  if (!row) return fail(row.error());
  return *row ? st->col_text(0) : std::string();
}

std::int64_t Database::changes() const { return sqlite3_changes(db_); }

}  // namespace opt::db
