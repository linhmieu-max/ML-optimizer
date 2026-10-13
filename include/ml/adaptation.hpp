#pragma once
// Local adaptation: Base model -> local machine adaptation -> machine-specific model.
// A dedicated, lowest-priority thread fine-tunes the models on this machine's recorded samples, but ONLY while
// the TrainingGate says the machine is idle. When a workload appears it pauses within one batch, writes a
// checkpoint and releases the CPU; it resumes (from the checkpoint, also across daemon restarts) once the machine
// has been idle again for a sustained period. An adapted model replaces the current one only if it passes the
// promotion tests (no regression on the base prior, measurable improvement on this machine's own hold-out).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "database/telemetry.hpp"
#include "ml/predictor.hpp"
#include "ml/trainer.hpp"
#include "ml/training_gate.hpp"

namespace opt::ml {

struct AdaptationOptions {
  std::filesystem::path dir;                         // state_dir/ml
  GateConfig gate;
  bool enabled = true;
  int min_new_samples = 500;                         // new samples since the last finished job
  std::chrono::seconds interval{6 * 3600};           // minimum time between jobs
  int replay_per_class = 120;                        // base-prior replay (workload)
  int replay_profile = 3000;                         // base-prior replay (profile)
  int replicate = 5;                                 // weight of a labelled local profile sample
  int epochs = 8;
  float lr = 0.003f;
  int min_local_train = 50, min_local_val = 20;      // labelled local samples needed to train / to judge
  std::chrono::seconds checkpoint_every{30};
  std::chrono::milliseconds poll{500};
  int data_days = 30;
  int max_samples = 40000;
  int slice_batches = 30;                            // batches between gate checks (each batch ~ 1 ms)
};

enum class AdaptState { Disabled, Waiting, Training, Paused };
constexpr std::string_view to_string(AdaptState s) noexcept {
  switch (s) {
    case AdaptState::Disabled: return "disabled";
    case AdaptState::Waiting: return "waiting";
    case AdaptState::Training: return "training";
    case AdaptState::Paused: return "paused";
  }
  return "?";
}

struct AdaptStatus {
  AdaptState state = AdaptState::Waiting;
  std::string detail;                    // why waiting/paused
  GateDecision gate;
  std::string job_kind;
  double progress = 0;
  int epoch = 0, epochs = 0;
  std::int64_t samples_total = 0, samples_since_last = 0;
  std::int64_t last_finished_unix = 0;
  std::vector<std::string> last_results; // outcome of the last job, one line per model
  bool checkpoint_exists = false;
  std::int64_t jobs_finished = 0, models_accepted = 0, models_rejected = 0, pauses = 0, resumes = 0;
};

class Adapter {
 public:
  Adapter(AdaptationOptions options, db::TelemetryStore& store, Predictor& predictor);
  ~Adapter();
  Adapter(const Adapter&) = delete;
  Adapter& operator=(const Adapter&) = delete;

  void start();
  void stop();  // pauses the current job (checkpoint) and joins

  /// Cheap and thread-safe; call whenever the machine state changes (e.g. after every classification).
  void update_gate(const GateInput&, Clock::time_point now = Clock::now());
  void set_enabled(bool);
  void request_run();           // start a job even if the sample/interval thresholds are not met (gate still applies)
  void reset_local();           // delete local models + checkpoint, back to the base models
  AdaptStatus status() const;

 private:
  struct Job;
  void run(std::stop_token);
  void step(std::stop_token);
  bool due(std::int64_t now_unix);
  bool begin_job(std::int64_t now_unix);
  bool prepare_phase();            // builds the next runnable phase of the job; false when none is left
  void finish_phase();
  void end_job(bool completed);
  void save_checkpoint();
  void restore_job_from_disk();
  void perform_reset();
  void set_state(AdaptState, std::string detail);
  std::filesystem::path path(const char* name) const { return opts_.dir / name; }

  AdaptationOptions opts_;
  db::TelemetryStore& store_;
  Predictor& predictor_;

  mutable std::mutex mutex_;
  std::condition_variable_any cv_;
  std::mutex gate_mutex_;
  TrainingGate gate_logic_;
  GateDecision gate_;
  std::atomic<bool> blocked_{true};
  std::atomic<bool> enabled_{true}, force_{false}, reset_{false};
  AdaptStatus status_;
  std::unique_ptr<Job> job_;       // worker thread only
  Clock::time_point last_checkpoint_{};
  std::int64_t last_finished_ = 0;
  std::jthread thread_;
};

}  // namespace opt::ml
