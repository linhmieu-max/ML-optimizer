#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>

#include "core/atomic_file.hpp"
#include "core/sysfs.hpp"
#include "safety/rollback.hpp"

namespace opt::safety {
namespace fs = std::filesystem;
namespace {

constexpr std::size_t kCompactAbove = 256 * 1024;

// Fields are percent-encoded so a record is one line of space-separated tokens.
// An empty string is written as "_" (a literal "_" is encoded).
std::string enc(const std::string& s) {
  if (s.empty()) return "_";
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '/' ||
                      c == '-' || c == ':' || c == '=' || c == ',' || c == '+' || c == '@';
    if (safe) out.push_back(static_cast<char>(c));
    else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
  }
  return out;
}

std::optional<std::string> dec(std::string_view s) {
  if (s == "_") return std::string();
  auto hv = [](char c) -> int {
    return c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1));
  };
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '%') {
      out.push_back(s[i]);
      continue;
    }
    if (i + 2 >= s.size()) return std::nullopt;  // truncated escape
    const int a = hv(s[i + 1]), b = hv(s[i + 2]);
    if (a < 0 || b < 0) return std::nullopt;
    out.push_back(static_cast<char>(a * 16 + b));
    i += 2;
  }
  return out;
}

std::string begin_line(const ChangeRecord& r) {
  return "B " + std::to_string(r.id) + " " + std::to_string(r.unix_time) + " " + enc(r.subsystem) + " " + enc(r.target) + " " +
         enc(r.old_value) + " " + enc(r.new_value) + " " + enc(r.reason) + " " + enc(r.policy);
}

}  // namespace

void RollbackJournal::apply_event(char kind, std::uint64_t id, const std::string* actual) {
  auto it = std::find_if(outstanding_.begin(), outstanding_.end(), [&](const ChangeRecord& r) { return r.id == id; });
  if (it == outstanding_.end()) return;
  if (kind == 'A') {
    it->applied = true;
    if (actual) it->new_value = *actual;
  }
  else outstanding_.erase(it);  // 'X' aborted or 'R' rolled back
}

Result<std::unique_ptr<RollbackJournal>> RollbackJournal::open(const fs::path& file) {
  std::unique_ptr<RollbackJournal> j(new RollbackJournal());
  j->file_ = file;
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);

  bool needs_newline = false;
  std::size_t size = 0;
  if (fs::exists(file, ec)) {
    auto text = sysfs::read_text(file, 8 * 1024 * 1024);
    if (!text) return fail(errno_error(Errc::Io, "cannot read journal " + file.string()));
    size = text->size();
    for (auto line : sysfs::split_lines(*text)) {
      auto f = sysfs::split_ws(line);
      if (f.size() >= 2 && f[0].size() == 1 && sysfs::parse_u64(f[1])) {
        const auto id = *sysfs::parse_u64(f[1]);
        j->next_id_ = std::max(j->next_id_, id + 1);
        if (f[0] == "B" && f.size() == 9) {
          ChangeRecord r;
          r.id = id;
          auto t = sysfs::parse_u64(f[2]);
          auto sub = dec(f[3]), tgt = dec(f[4]), ov = dec(f[5]), nv = dec(f[6]), rs = dec(f[7]), po = dec(f[8]);
          if (t && sub && tgt && ov && nv && rs && po) {
            r.unix_time = static_cast<std::int64_t>(*t);
            r.subsystem = *sub; r.target = *tgt; r.old_value = *ov; r.new_value = *nv; r.reason = *rs; r.policy = *po;
            j->outstanding_.push_back(std::move(r));
            continue;
          }
        } else if (f[0] == "A" && (f.size() == 2 || f.size() == 3)) {
          std::optional<std::string> actual;
          if (f.size() == 3) actual = dec(f[2]);
          if (f.size() == 2 || actual) {
            j->apply_event('A', id, actual ? &*actual : nullptr);
            continue;
          }
        } else if ((f[0] == "X" || f[0] == "R") && f.size() == 2) {
          j->apply_event(f[0][0], id);
          continue;
        }
      }
      ++j->corrupt_;  // torn or damaged line: skipped, never fatal
    }
    needs_newline = !text->empty() && text->back() != '\n';
  }

  j->fd_ = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
  if (j->fd_ < 0) return fail(errno_error(Errc::Io, "cannot open journal " + file.string()));
  if (needs_newline) {
    if (auto r = j->append_line(""); !r) return fail(r.error());  // isolate a torn last line
  }
  if (size > kCompactAbove) {
    if (auto r = j->rewrite_compacted(); !r) return fail(r.error());
  }
  return j;
}

RollbackJournal::~RollbackJournal() {
  if (fd_ >= 0) ::close(fd_);
}

Result<void> RollbackJournal::append_line(const std::string& line) {
  const std::string data = line + "\n";
  const char* p = data.data();
  std::size_t left = data.size();
  while (left > 0) {
    const ssize_t w = ::write(fd_, p, left);
    if (w < 0) {
      if (errno == EINTR) continue;
      return fail(errno_error(Errc::Io, "journal write failed"));
    }
    p += w;
    left -= static_cast<std::size_t>(w);
  }
  if (::fsync(fd_) != 0) return fail(errno_error(Errc::Io, "journal fsync failed"));
  return {};
}

Result<void> RollbackJournal::rewrite_compacted() {
  std::string content;
  for (const auto& r : outstanding_) {
    content += begin_line(r) + "\n";
    if (r.applied) content += "A " + std::to_string(r.id) + " " + enc(r.new_value) + "\n";
  }
  if (auto r = write_file_atomic(file_, content); !r) return fail(r.error());
  ::close(fd_);
  fd_ = ::open(file_.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd_ < 0) return fail(errno_error(Errc::Io, "cannot reopen journal"));
  return {};
}

Result<std::uint64_t> RollbackJournal::begin(ChangeRecord record) {
  record.id = next_id_;
  if (record.unix_time == 0) record.unix_time = static_cast<std::int64_t>(std::time(nullptr));
  record.applied = false;
  if (auto r = append_line(begin_line(record)); !r) return fail(r.error());
  ++next_id_;
  const auto id = record.id;
  outstanding_.push_back(std::move(record));
  return id;
}

Result<void> RollbackJournal::mark_applied(std::uint64_t id, const std::string* actual) {
  if (auto r = append_line("A " + std::to_string(id) + (actual ? " " + enc(*actual) : std::string())); !r) return r;
  apply_event('A', id, actual);
  return {};
}
Result<void> RollbackJournal::mark_aborted(std::uint64_t id) {
  if (auto r = append_line("X " + std::to_string(id)); !r) return r;
  apply_event('X', id);
  return {};
}
Result<void> RollbackJournal::mark_rolled_back(std::uint64_t id) {
  if (auto r = append_line("R " + std::to_string(id)); !r) return r;
  apply_event('R', id);
  return {};
}

}  // namespace opt::safety
