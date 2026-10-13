#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opt {

/// Runs named tasks periodically on one background thread. Tasks run
/// sequentially (never concurrently with each other); an overrunning task
/// delays the next run instead of piling up. Exceptions are logged and swallowed.
class Scheduler {
 public:
  using Task = std::function<void()>;
  using Clock = std::chrono::steady_clock;

  Scheduler() = default;
  ~Scheduler() { stop(); }
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  /// First run happens as soon as the thread is running.
  void add(std::string name, std::chrono::milliseconds interval, Task task);
  void set_interval(std::string_view name, std::chrono::milliseconds interval);
  /// Run the named task as soon as possible (e.g. after the user changed the profile).
  void run_soon(std::string_view name);
  void start();
  void stop();  // idempotent, waits for the running task to finish

 private:
  struct Entry {
    std::string name;
    std::chrono::milliseconds interval;
    Task fn;
    Clock::time_point next;
  };
  void loop(std::stop_token);

  std::mutex mutex_;
  std::condition_variable_any cv_;
  std::vector<Entry> entries_;
  bool dirty_ = false;
  std::jthread thread_;
};

}  // namespace opt
