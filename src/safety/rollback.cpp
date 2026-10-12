#include "safety/rollback.hpp"

#include <algorithm>

#include "core/logging.hpp"

namespace opt::safety {

RollbackManager::RollbackManager(std::unique_ptr<RollbackJournal> journal, StateWriter& writer)
    : journal_(std::move(journal)), writer_(writer) {}

std::size_t RollbackManager::outstanding_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_->outstanding().size();
}
std::vector<ChangeRecord> RollbackManager::outstanding() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_->outstanding();
}
int RollbackManager::corrupt_lines() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_->corrupt_lines();
}

Result<std::uint64_t> RollbackManager::apply(const ChangeRequest& req) {
  std::lock_guard<std::mutex> lock(mutex_);
  // We must be able to read what we are about to overwrite, otherwise it cannot be restored.
  auto old = writer_.read(req.target);
  if (!old) return fail(make_error(old.error().code, "cannot read current value of " + req.target + ": refusing to change what cannot be restored (" + old.error().message + ")"));
  if (*old == req.new_value) return std::uint64_t{0};

  // If we already changed this target and nobody else touched it since, keep ONE record
  // holding the ORIGINAL value, so a single rollback restores what the system had before us.
  std::vector<std::uint64_t> superseded;
  std::string original = *old;
  {
    const ChangeRecord* latest = nullptr;
    const ChangeRecord* first = nullptr;
    for (const auto& r : journal_->outstanding()) {
      if (r.target != req.target) continue;
      if (!first || r.id < first->id) first = &r;
      if (!latest || r.id > latest->id) latest = &r;
      superseded.push_back(r.id);
    }
    if (latest && latest->applied && latest->new_value == *old) original = first->old_value;
    else superseded.clear();  // changed externally in between: keep the chain, do not collapse
  }

  ChangeRecord rec;
  rec.subsystem = req.subsystem;
  rec.target = req.target;
  rec.old_value = original;
  rec.new_value = req.new_value;
  rec.reason = req.reason;
  rec.policy = req.policy;
  auto id = journal_->begin(std::move(rec));  // durable BEFORE the write
  if (!id) return fail(make_error(id.error().code, "journal unavailable, change refused: " + id.error().message));

  if (auto w = writer_.write(req.target, req.new_value); !w) {
    (void)journal_->mark_aborted(*id);
    return fail(w.error());
  }
  // The kernel may normalise what we wrote; journal what is really there so rollback can recognise it.
  std::string actual = req.new_value;
  if (auto back = writer_.read(req.target); back) actual = *back;
  if (auto m = journal_->mark_applied(*id, actual != req.new_value ? &actual : nullptr); !m)
    log::warn("rollback", "journal could not record the applied state", {{"id", std::to_string(*id)}});
  for (const auto old_id : superseded) (void)journal_->mark_rolled_back(old_id);
  log::debug("rollback", "applied", {{"id", std::to_string(*id)}, {"target", req.target}});
  return *id;
}

RollbackReport RollbackManager::rollback_locked(const std::string* subsystem, const std::string* target, const std::string& why) {
  RollbackReport rep;
  auto pending = journal_->outstanding();
  std::sort(pending.begin(), pending.end(), [](const ChangeRecord& a, const ChangeRecord& b) { return a.id > b.id; });  // newest first
  for (const auto& r : pending) {
    if (subsystem && r.subsystem != *subsystem) continue;
    if (target && r.target != *target) continue;
    const std::string tag = "#" + std::to_string(r.id) + " " + r.target;
    auto cur = writer_.read(r.target);
    if (!cur) {
      ++rep.failed;
      rep.notes.push_back(tag + ": cannot read current value (" + cur.error().message + ")");
      continue;
    }
    if (*cur == r.old_value) {
      // Already back to the original (e.g. the write never happened before a crash).
      (void)journal_->mark_rolled_back(r.id);
      ++rep.skipped;
      rep.notes.push_back(tag + ": already at original value");
      continue;
    }
    if (*cur != r.new_value) {
      // Someone else changed it after us: do not fight them.
      (void)journal_->mark_rolled_back(r.id);
      ++rep.skipped;
      rep.notes.push_back(tag + ": changed by another tool, left untouched");
      log::safety("rollback", "target changed externally; not restoring", {{"target", r.target}});
      continue;
    }
    if (auto w = writer_.write(r.target, r.old_value); !w) {
      ++rep.failed;
      rep.notes.push_back(tag + ": restore failed (" + w.error().message + ")");
      log::safety("rollback", "RESTORE FAILED", {{"target", r.target}, {"error", describe(w.error())}});
      continue;
    }
    (void)journal_->mark_rolled_back(r.id);
    ++rep.restored;
    rep.notes.push_back(tag + ": restored");
  }
  if (rep.restored + rep.skipped + rep.failed > 0)
    log::safety("rollback", "rollback finished: " + why,
                {{"restored", std::to_string(rep.restored)}, {"skipped", std::to_string(rep.skipped)}, {"failed", std::to_string(rep.failed)}});
  return rep;
}

RollbackReport RollbackManager::rollback_all(const std::string& why) {
  std::lock_guard<std::mutex> lock(mutex_);
  return rollback_locked(nullptr, nullptr, why);
}
RollbackReport RollbackManager::rollback_subsystem(const std::string& subsystem, const std::string& why) {
  std::lock_guard<std::mutex> lock(mutex_);
  return rollback_locked(&subsystem, nullptr, why);
}
RollbackReport RollbackManager::rollback_target(const std::string& target, const std::string& why) {
  std::lock_guard<std::mutex> lock(mutex_);
  return rollback_locked(nullptr, &target, why);
}

}  // namespace opt::safety
