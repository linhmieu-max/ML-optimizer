#include "ml/adaptation.hpp"

#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "core/atomic_file.hpp"
#include "core/logging.hpp"
#include "core/sysfs.hpp"
#include "ml/base_model.hpp"
#include "ml/dataset.hpp"
#include "ml/feature_vector.hpp"

namespace opt::ml {
namespace fs = std::filesystem;
namespace {
constexpr const char* kCkpt = "adapt-checkpoint.ckpt";
constexpr const char* kJobState = "adapt-job.state";
constexpr const char* kMeta = "adapt-meta.state";

std::int64_t unix_now() { return static_cast<std::int64_t>(std::time(nullptr)); }
std::string pct(double v) {
  char b[16];
  std::snprintf(b, sizeof b, "%.1f%%", 100.0 * v);
  return b;
}
}  // namespace

struct Adapter::Job {
  std::int64_t id = 0, ts_max = 0;
  std::vector<std::string> remaining;            // phases still to run, current first
  std::vector<db::MlSampleRow> rows;
  std::string kind;
  std::unique_ptr<Trainer> trainer;
  std::vector<Sample> val;
  std::shared_ptr<const Mlp> start_model;
  std::int64_t local_train = 0, local_val = 0;
  std::string fingerprint;
  FitConfig cfg;
  bool resumed = false;
  std::vector<std::string> results;
};

Adapter::~Adapter() { stop(); }

Adapter::Adapter(AdaptationOptions options, db::TelemetryStore& store, Predictor& predictor)
    : opts_(std::move(options)), store_(store), predictor_(predictor), gate_logic_(opts_.gate) {
  enabled_ = opts_.enabled;
  status_.state = opts_.enabled ? AdaptState::Waiting : AdaptState::Disabled;
  status_.detail = "no data yet";
  if (auto t = sysfs::read_text(path(kMeta), 256)) {
    for (auto line : sysfs::split_lines(*t))
      if (line.starts_with("last_finished=")) last_finished_ = static_cast<std::int64_t>(sysfs::parse_u64(line.substr(14)).value_or(0));
  }
  status_.last_finished_unix = last_finished_;
}

void Adapter::start() {
  if (thread_.joinable()) return;
  thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void Adapter::stop() {
  if (!thread_.joinable()) return;
  thread_.request_stop();
  thread_.join();
}

void Adapter::update_gate(const GateInput& in, Clock::time_point now) {
  GateDecision d;
  {
    std::lock_guard<std::mutex> lock(gate_mutex_);
    d = gate_logic_.evaluate(in, now);
  }
  blocked_.store(d.klass == TrainingClass::Blocked, std::memory_order_release);  // read by the trainer between batches
  std::lock_guard<std::mutex> lock(mutex_);
  gate_ = std::move(d);
}

void Adapter::set_enabled(bool on) {
  enabled_ = on;
  cv_.notify_all();
}
void Adapter::request_run() {
  force_ = true;
  cv_.notify_all();
}
void Adapter::reset_local() {
  reset_ = true;
  if (!thread_.joinable()) perform_reset();
  cv_.notify_all();
}

AdaptStatus Adapter::status() const {
  AdaptStatus s;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    s = status_;
    s.gate = gate_;
  }
  s.checkpoint_exists = fs::exists(path(kCkpt));
  if (auto n = store_.ml_sample_count(0)) s.samples_total = *n;
  if (auto n = store_.ml_sample_count(s.last_finished_unix)) s.samples_since_last = *n;
  return s;
}

void Adapter::set_state(AdaptState st, std::string detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_.state = st;
  status_.detail = std::move(detail);
}

void Adapter::run(std::stop_token st) {
  // Lowest possible priority: background learning must never make the machine lag.
  ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
  sched_param sp{};
  ::sched_setscheduler(static_cast<pid_t>(::syscall(SYS_gettid)), SCHED_IDLE, &sp);  // best effort
  restore_job_from_disk();
  std::mutex wait_mutex;
  std::unique_lock<std::mutex> lock(wait_mutex);
  while (!st.stop_requested()) {
    step(st);
    cv_.wait_for(lock, st, opts_.poll, [] { return false; });
  }
  if (job_) save_checkpoint();  // leave a resumable state behind
}

bool Adapter::due(std::int64_t now_unix) {
  if (!predictor_.ready()) {
    set_state(AdaptState::Waiting, "base models are not ready yet");
    return false;
  }
  if (force_.load()) return true;
  if (now_unix - last_finished_ < opts_.interval.count()) {
    set_state(AdaptState::Waiting, "next job after the minimum interval");
    return false;
  }
  auto n = store_.ml_sample_count(last_finished_);
  const std::int64_t have = n ? *n : 0;
  if (have < opts_.min_new_samples) {
    set_state(AdaptState::Waiting, std::to_string(have) + " of " + std::to_string(opts_.min_new_samples) + " new samples collected");
    return false;
  }
  return true;
}

void Adapter::step(std::stop_token st) {
  if (reset_.exchange(false)) perform_reset();
  if (!enabled_.load()) {
    if (job_) { save_checkpoint(); job_.reset(); }
    set_state(AdaptState::Disabled, "disabled");
    return;
  }
  GateDecision gate;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    gate = gate_;
  }
  const bool blocked = gate.klass == TrainingClass::Blocked;

  if (!job_) {
    if (blocked) {
      set_state(AdaptState::Waiting, "waiting for an idle machine: " + gate.reason);
      return;
    }
    if (!due(unix_now())) return;
    if (!begin_job(unix_now())) return;
  }
  if (blocked) {
    if (status_.state != AdaptState::Paused) {
      save_checkpoint();  // a workload appeared: keep the progress, free the CPU
      std::lock_guard<std::mutex> lock(mutex_);
      ++status_.pauses;
    }
    set_state(AdaptState::Paused, "paused: " + gate.reason);
    return;
  }
  if (status_.state == AdaptState::Paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++status_.resumes;
  }
  set_state(AdaptState::Training, "training " + job_->kind + " on idle time");

  const auto t0 = std::chrono::steady_clock::now();
  const auto result = job_->trainer->run([&] { return !st.stop_requested() && !blocked_.load(std::memory_order_acquire); }, opts_.slice_batches);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.job_kind = job_->kind;
    status_.progress = job_->trainer->progress();
    status_.epoch = job_->trainer->epoch();
    status_.epochs = job_->trainer->epochs();
  }
  if (result == Trainer::Status::Finished) {
    finish_phase();
    return;
  }
  if (std::chrono::steady_clock::now() - last_checkpoint_ >= opts_.checkpoint_every) save_checkpoint();
  if (gate.klass == TrainingClass::Limited) {  // reduced duty cycle: sleep ~3x the time just spent
    const auto spent = std::chrono::steady_clock::now() - t0;
    std::mutex m;
    std::unique_lock<std::mutex> lk(m);
    cv_.wait_for(lk, st, std::min<std::chrono::steady_clock::duration>(spent * 3, std::chrono::milliseconds(1500)), [] { return false; });
  }
}

bool Adapter::begin_job(std::int64_t now_unix) {
  job_ = std::make_unique<Job>();
  job_->id = now_unix;
  job_->ts_max = now_unix;
  job_->remaining = {"workload", "profile"};
  force_ = false;
  auto rows = store_.ml_samples(now_unix - static_cast<std::int64_t>(opts_.data_days) * 86400, job_->ts_max, opts_.max_samples);
  if (!rows) {
    log::warn("adapt", "cannot read training samples", {{"error", describe(rows.error())}});
    job_.reset();
    return false;
  }
  job_->rows = std::move(*rows);
  (void)write_file_atomic(path(kJobState), "job_id=" + std::to_string(job_->id) + "\nts_max=" + std::to_string(job_->ts_max) + "\n");
  log::info("adapt", "local adaptation job started", {{"samples", std::to_string(job_->rows.size())}});
  if (!prepare_phase()) {
    end_job(true);
    return false;
  }
  return true;
}

void Adapter::restore_job_from_disk() {
  auto text = sysfs::read_text(path(kJobState), 1024);
  if (!text || !fs::exists(path(kCkpt))) return;
  std::int64_t id = 0, ts_max = 0;
  for (auto line : sysfs::split_lines(*text)) {
    if (line.starts_with("job_id=")) id = static_cast<std::int64_t>(sysfs::parse_u64(line.substr(7)).value_or(0));
    if (line.starts_with("ts_max=")) ts_max = static_cast<std::int64_t>(sysfs::parse_u64(line.substr(7)).value_or(0));
  }
  if (id == 0 || ts_max == 0) return;
  auto rows = store_.ml_samples(ts_max - static_cast<std::int64_t>(opts_.data_days) * 86400, ts_max, opts_.max_samples);
  if (!rows) return;
  job_ = std::make_unique<Job>();
  job_->id = id;
  job_->ts_max = ts_max;
  job_->rows = std::move(*rows);
  job_->remaining = {"workload", "profile"};
  // The checkpoint names the phase it belongs to; try the phases in order until one matches its fingerprint.
  if (!prepare_phase() || !job_->resumed) {
    log::info("adapt", "stale checkpoint discarded");
    job_.reset();
    std::error_code ec;
    fs::remove(path(kCkpt), ec);
    fs::remove(path(kJobState), ec);
    return;
  }
  log::info("adapt", "resumed from checkpoint", {{"phase", job_->kind}, {"progress", pct(job_->trainer->progress())}});
  set_state(AdaptState::Paused, "resumed from checkpoint; waiting for an idle machine");
}

bool Adapter::prepare_phase() {
  Job& j = *job_;
  while (!j.remaining.empty()) {
    const std::string kind = j.remaining.front();
    const bool is_workload = kind == "workload";
    const std::uint64_t seed = static_cast<std::uint64_t>(j.id) * 31 + (is_workload ? 1 : 2);
    TrainingSets sets = is_workload ? build_workload_sets(j.rows, opts_.replay_per_class, seed)
                                    : build_profile_sets(j.rows, opts_.replay_profile, opts_.replicate, seed);
    auto cur = predictor_.model(kind);
    if (!cur) { j.results.push_back(kind + ": models not ready"); j.remaining.erase(j.remaining.begin()); continue; }
    if (sets.local_train < opts_.min_local_train || sets.local_val < opts_.min_local_val) {
      j.results.push_back(kind + ": skipped (" + std::to_string(sets.local_train) + " labelled training / " + std::to_string(sets.local_val) +
                          " validation samples, needs " + std::to_string(opts_.min_local_train) + " / " + std::to_string(opts_.min_local_val) + ")");
      j.remaining.erase(j.remaining.begin());
      continue;
    }
    j.kind = kind;
    j.local_train = sets.local_train;
    j.local_val = sets.local_val;
    j.val = std::move(sets.val);
    j.start_model = cur;
    j.cfg = FitConfig{};
    j.cfg.epochs = opts_.epochs;
    j.cfg.lr = opts_.lr;
    j.cfg.seed = seed;
    j.fingerprint = dataset_fingerprint(kind, sets.train, j.val);
    j.resumed = false;
    if (auto ck = sysfs::read_text(path(kCkpt), 64 * 1024 * 1024)) {
      auto restored = Trainer::restore(*ck + "\n", sets.train, j.val, j.cfg, kind, j.fingerprint);
      if (restored) {
        j.trainer = std::make_unique<Trainer>(std::move(*restored));
        j.resumed = true;
      }
    }
    if (!j.trainer) j.trainer = std::make_unique<Trainer>(*cur, std::move(sets.train), j.val, j.cfg);
    std::lock_guard<std::mutex> lock(mutex_);
    status_.job_kind = kind;
    status_.progress = j.trainer->progress();
    status_.epoch = j.trainer->epoch();
    status_.epochs = j.trainer->epochs();
    return true;
  }
  return false;
}

void Adapter::save_checkpoint() {
  if (!job_ || !job_->trainer) return;
  if (auto r = write_file_atomic(path(kCkpt), job_->trainer->checkpoint(job_->kind, job_->fingerprint)); !r)
    log::warn("adapt", "cannot write the checkpoint", {{"error", describe(r.error())}});
  last_checkpoint_ = std::chrono::steady_clock::now();
}

void Adapter::finish_phase() {
  Job& j = *job_;
  Mlp candidate = j.trainer->model();
  const double new_acc = accuracy(candidate, j.val), old_acc = accuracy(*j.start_model, j.val);
  const bool is_workload = j.kind == "workload";
  Rng rng(777);  // fixed hold-out of the BASE prior: the adapted model must not forget it
  const double sanity = is_workload ? accuracy(candidate, make_workload_dataset(60, rng)) : accuracy(candidate, make_profile_dataset(1500, rng));
  bool finite = true;
  for (const auto* v : {&candidate.w1, &candidate.b1, &candidate.w2, &candidate.b2})
    for (float x : *v) finite &= std::isfinite(x);

  std::string verdict;
  bool accept = false;
  if (!finite) verdict = "rejected (non-finite weights)";
  else if (sanity < 0.80) verdict = "rejected (forgot the base prior: " + pct(sanity) + " on the base hold-out)";
  else if (new_acc < old_acc + 0.005) verdict = "rejected (no measurable improvement: " + pct(new_acc) + " vs " + pct(old_acc) + " before on this machine's hold-out)";
  else accept = true;

  if (accept) {
    candidate.meta.kind = j.kind;
    candidate.meta.feature_version = kFeatureVersion;
    candidate.meta.trained_samples = static_cast<std::int64_t>(j.local_train);
    candidate.meta.trained_at = unix_now();
    candidate.meta.source = "local";
    candidate.meta.val_accuracy = new_acc;
    const fs::path file = path(is_workload ? "local-workload-v1.model" : "local-profile-v1.model");
    std::error_code ec;
    if (fs::exists(file, ec)) fs::rename(file, file.string() + ".prev", ec);
    if (auto r = save_model(file, candidate); !r) {
      verdict = "rejected (cannot save: " + r.error().message + ")";
      accept = false;
    } else if (auto r2 = predictor_.install_local(candidate); !r2) {
      verdict = "rejected (" + r2.error().message + ")";
      accept = false;
    } else {
      verdict = "ACCEPTED (" + pct(new_acc) + " vs " + pct(old_acc) + " before on this machine's hold-out, " + pct(sanity) + " on the base prior)";
    }
  }
  j.results.push_back(j.kind + ": " + verdict);
  log::info("adapt", j.kind + " model " + (accept ? "adapted" : "unchanged"), {{"verdict", verdict}});
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accept ? ++status_.models_accepted : ++status_.models_rejected;
  }
  std::error_code ec;
  fs::remove(path(kCkpt), ec);
  j.trainer.reset();
  j.remaining.erase(j.remaining.begin());
  if (!prepare_phase()) end_job(true);
}

void Adapter::end_job(bool completed) {
  std::vector<std::string> results;
  if (job_) results = job_->results;
  job_.reset();
  std::error_code ec;
  fs::remove(path(kCkpt), ec);
  fs::remove(path(kJobState), ec);
  if (!completed) return;
  last_finished_ = unix_now();
  (void)write_file_atomic(path(kMeta), "last_finished=" + std::to_string(last_finished_) + "\n");
  std::lock_guard<std::mutex> lock(mutex_);
  ++status_.jobs_finished;
  status_.last_finished_unix = last_finished_;
  status_.last_results = std::move(results);
  status_.progress = 0;
  status_.job_kind.clear();
  status_.state = AdaptState::Waiting;
  status_.detail = "job finished";
}

void Adapter::perform_reset() {
  job_.reset();
  std::error_code ec;
  for (const char* f : {"local-workload-v1.model", "local-workload-v1.model.prev", "local-profile-v1.model", "local-profile-v1.model.prev", kCkpt, kJobState, kMeta})
    fs::remove(path(f), ec);
  last_finished_ = 0;
  predictor_.reset_local();
  std::lock_guard<std::mutex> lock(mutex_);
  status_.last_results = {"local models deleted: back to the base models"};
  status_.last_finished_unix = 0;
  status_.progress = 0;
  status_.job_kind.clear();
  status_.state = AdaptState::Waiting;
  status_.detail = "reset";
  log::info("adapt", "local models deleted; base models restored");
}

}  // namespace opt::ml
