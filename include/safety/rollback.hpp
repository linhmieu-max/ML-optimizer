#pragma once
// Rollback: every system change is journaled (old state, new state, time, reason,
// policy) and fsync'ed BEFORE it is applied, so a crash or restart can always
// restore the previous state. The journal lives in the daemon's own state dir.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/result.hpp"

namespace opt::safety {

struct ChangeRecord {
  std::uint64_t id = 0;
  std::int64_t unix_time = 0;
  std::string subsystem, target, old_value, new_value, reason, policy;
  bool applied = false;  // false = journaled but the write may not have happened (crash window)
};

/// Append-only, crash-tolerant journal (torn last line tolerated, compacted when large).
class RollbackJournal {
 public:
  static Result<std::unique_ptr<RollbackJournal>> open(const std::filesystem::path& file);
  ~RollbackJournal();
  RollbackJournal(const RollbackJournal&) = delete;
  RollbackJournal& operator=(const RollbackJournal&) = delete;

  Result<std::uint64_t> begin(ChangeRecord record);  // assigns the id; durable on return
  /// `actual` = the value read back after the write (the kernel may normalise it); it replaces new_value.
  Result<void> mark_applied(std::uint64_t id, const std::string* actual = nullptr);
  Result<void> mark_aborted(std::uint64_t id);       // the write failed: nothing to undo
  Result<void> mark_rolled_back(std::uint64_t id);
  const std::vector<ChangeRecord>& outstanding() const { return outstanding_; }
  int corrupt_lines() const { return corrupt_; }

 private:
  RollbackJournal() = default;
  Result<void> append_line(const std::string& line);
  Result<void> rewrite_compacted();
  void apply_event(char kind, std::uint64_t id, const std::string* actual = nullptr);

  std::filesystem::path file_;
  int fd_ = -1;
  std::vector<ChangeRecord> outstanding_;  // ascending id
  std::uint64_t next_id_ = 1;
  int corrupt_ = 0;
};

/// Reads and writes the value behind a target (a sysfs file, a sysctl...). Phase 6
/// provides the real implementation; tests use a fake.
class StateWriter {
 public:
  virtual ~StateWriter() = default;
  virtual Result<std::string> read(const std::string& target) = 0;
  virtual Result<void> write(const std::string& target, const std::string& value) = 0;
};

/// Placeholder until controllers exist: cannot read or write anything.
class NullWriter final : public StateWriter {
 public:
  Result<std::string> read(const std::string&) override { return fail(make_error(Errc::Unsupported, "no state writer in this build")); }
  Result<void> write(const std::string&, const std::string&) override { return fail(make_error(Errc::Unsupported, "no state writer in this build")); }
};

struct ChangeRequest {
  std::string subsystem, target, new_value, reason, policy;
};

struct RollbackReport {
  int restored = 0, skipped = 0, failed = 0;
  std::vector<std::string> notes;
  bool ok() const { return failed == 0; }
};

/// Thread-safe. apply() = read old value -> journal -> write -> mark applied.
class RollbackManager {
 public:
  RollbackManager(std::unique_ptr<RollbackJournal> journal, StateWriter& writer);

  /// Returns the change id (0 when the value already equals the requested one).
  Result<std::uint64_t> apply(const ChangeRequest&);
  /// Undo outstanding changes newest-first. A target that someone else changed in the
  /// meantime is NOT overwritten (we do not fight other tools).
  RollbackReport rollback_all(const std::string& why);
  RollbackReport rollback_subsystem(const std::string& subsystem, const std::string& why);
  RollbackReport rollback_target(const std::string& target, const std::string& why);
  std::size_t outstanding_count() const;
  std::vector<ChangeRecord> outstanding() const;
  int corrupt_lines() const;

 private:
  RollbackReport rollback_locked(const std::string* subsystem, const std::string* target, const std::string& why);

  mutable std::mutex mutex_;
  std::unique_ptr<RollbackJournal> journal_;
  StateWriter& writer_;
};

}  // namespace opt::safety
