#include "daemon/scheduler.hpp"

#include <algorithm>

#include "core/logging.hpp"

namespace opt {

void Scheduler::add(std::string name, std::chrono::milliseconds interval, Task task) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.push_back({std::move(name), interval, std::move(task), Clock::now()});
  dirty_ = true;
  cv_.notify_all();
}

void Scheduler::set_interval(std::string_view name, std::chrono::milliseconds interval) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& e : entries_) {
    if (e.name != name) continue;
    e.interval = interval;
    e.next = std::min(e.next, Clock::now() + interval);
  }
  dirty_ = true;
  cv_.notify_all();
}

void Scheduler::run_soon(std::string_view name) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& e : entries_)
    if (e.name == name) e.next = Clock::now();
  dirty_ = true;
  cv_.notify_all();
}

void Scheduler::start() {
  if (thread_.joinable()) return;
  thread_ = std::jthread([this](std::stop_token token) { loop(token); });
}

void Scheduler::stop() {
  if (!thread_.joinable()) return;
  thread_.request_stop();
  thread_.join();
}

void Scheduler::loop(std::stop_token token) {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!token.stop_requested()) {
    if (entries_.empty()) {
      cv_.wait(lock, token, [&] { return dirty_; });
    } else {
      const auto earliest = std::min_element(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
                              return a.next < b.next;
                            })->next;
      cv_.wait_until(lock, token, earliest, [&] { return dirty_; });
    }
    if (token.stop_requested()) break;
    dirty_ = false;

    const auto now = Clock::now();
    for (std::size_t i = 0; i < entries_.size() && !token.stop_requested(); ++i) {
      if (entries_[i].next > now) continue;
      const Task fn = entries_[i].fn;
      const std::string name = entries_[i].name;
      lock.unlock();
      try {
        fn();
      } catch (const std::exception& e) {
        log::error("scheduler", "task threw", {{"task", name}, {"what", e.what()}});
      } catch (...) {
        log::error("scheduler", "task threw", {{"task", name}});
      }
      lock.lock();
      entries_[i].next = Clock::now() + entries_[i].interval;
    }
  }
}

}  // namespace opt
