#include "workload/classifier.hpp"

#include <algorithm>
#include <cstdio>

#include "core/logging.hpp"
#include "workload/browser_detector.hpp"
#include "workload/compile_detector.hpp"
#include "workload/container_detector.hpp"
#include "workload/game_detector.hpp"
#include "workload/simple_rules.hpp"

namespace opt::workload {
namespace {

constexpr std::size_t idx(WorkloadClass c) { return static_cast<std::size_t>(c); }

// Order = tie-break priority among specific workloads (earlier wins ties).
constexpr WorkloadClass kSpecific[] = {WorkloadClass::Game,   WorkloadClass::Render,     WorkloadClass::Encode,
                                       WorkloadClass::Compile, WorkloadClass::Vm,         WorkloadClass::Container,
                                       WorkloadClass::Archive, WorkloadClass::FileCopy,   WorkloadClass::VideoPlayback,
                                       WorkloadClass::Server,  WorkloadClass::Browser};
constexpr WorkloadClass kBaseline[] = {WorkloadClass::Desktop, WorkloadClass::Background, WorkloadClass::Idle};

}  // namespace

const WorkloadResult& Classifier::evaluate(const DetectContext& ctx, Clock::time_point now) {
  std::array<Evidence, kClassCount> ev;
  ev[idx(WorkloadClass::Game)] = detect_game(ctx);
  ev[idx(WorkloadClass::Render)] = detect_render(ctx);
  ev[idx(WorkloadClass::Encode)] = detect_encode(ctx);
  ev[idx(WorkloadClass::Compile)] = detect_compile(ctx);
  ev[idx(WorkloadClass::Vm)] = detect_vm(ctx);
  ev[idx(WorkloadClass::Container)] = detect_container(ctx);
  ev[idx(WorkloadClass::Archive)] = detect_archive(ctx);
  ev[idx(WorkloadClass::FileCopy)] = detect_file_copy(ctx);
  ev[idx(WorkloadClass::VideoPlayback)] = detect_video(ctx);
  ev[idx(WorkloadClass::Server)] = detect_server(ctx);
  ev[idx(WorkloadClass::Browser)] = detect_browser(ctx);
  ev[idx(WorkloadClass::Desktop)] = detect_desktop(ctx);
  ev[idx(WorkloadClass::Background)] = detect_background(ctx);
  ev[idx(WorkloadClass::Idle)] = detect_idle(ctx);

  // 1. Raw winner: best specific class above threshold, else best baseline.
  WorkloadClass best = WorkloadClass::Idle;
  double best_score = -1;
  for (WorkloadClass c : kSpecific) {
    const double s = ev[idx(c)].score;
    if (s >= cfg_.min_specific_score && s > best_score + 1e-9) { best = c; best_score = s; }
  }
  if (best_score < 0) {
    // Idle is the default when nothing positively indicates Desktop/Background.
    best = WorkloadClass::Idle;
    best_score = ev[idx(WorkloadClass::Idle)].score;
    for (WorkloadClass c : {WorkloadClass::Desktop, WorkloadClass::Background}) {
      const double s = ev[idx(c)].score;
      if (s > best_score + 1e-9) { best = c; best_score = s; }
    }
  }

  // 2. Hysteresis: a challenger must clearly beat the current class several times in a row.
  if (!initialized_) {
    result_.cls = best;
    result_.since = now;
    initialized_ = true;
    log::info("workload", "initial workload: " + std::string(to_string(best)));
  } else if (best != result_.cls) {
    // A specific class whose own evidence has dropped below the threshold no longer
    // qualifies, so it can be replaced without beating it by the margin.
    const bool cur_baseline_class = result_.cls == WorkloadClass::Desktop || result_.cls == WorkloadClass::Background ||
                                    result_.cls == WorkloadClass::Idle;
    const double cur_score = ev[idx(result_.cls)].score;
    const bool cur_qualifies = cur_baseline_class || cur_score >= cfg_.min_specific_score;
    if (!cur_qualifies || best_score > cur_score + cfg_.switch_margin) {
      if (result_.pending == best) ++result_.pending_streak;
      else { result_.pending = best; result_.pending_streak = 1; }
      if (result_.pending_streak >= cfg_.confirm_evaluations) {
        log::info("workload", "Workload changed: " + std::string(to_string(result_.cls)) + " -> " + std::string(to_string(best)),
                  {{"score", std::to_string(best_score).substr(0, 4)}});
        result_.cls = best;
        result_.since = now;
        result_.pending.reset();
        result_.pending_streak = 0;
      }
    } else {
      result_.pending.reset();
      result_.pending_streak = 0;
    }
  } else {
    result_.pending.reset();
    result_.pending_streak = 0;
  }

  // 3. Publish scores/explanation for the (possibly unchanged) current class.
  for (std::size_t i = 0; i < kClassCount; ++i) result_.scores[i] = ev[i].score;
  const double cur = ev[idx(result_.cls)].score;
  // Confidence = how clearly the class beats its rivals. Desktop/Background/Idle are
  // ever-present context, so they are not rivals of a specific workload.
  const bool cur_baseline = result_.cls == WorkloadClass::Desktop || result_.cls == WorkloadClass::Background ||
                            result_.cls == WorkloadClass::Idle;
  double second = 0;
  for (std::size_t i = 0; i < kClassCount; ++i) {
    const auto c = static_cast<WorkloadClass>(i);
    const bool baseline = c == WorkloadClass::Desktop || c == WorkloadClass::Background || c == WorkloadClass::Idle;
    if (i != idx(result_.cls) && (cur_baseline || !baseline)) second = std::max(second, ev[i].score);
  }
  result_.confidence = std::clamp(cur - 0.5 * second, 0.0, 1.0);
  result_.reasons = ev[idx(result_.cls)].reasons;
  ++result_.evaluations;

  // 4. Side features for the policy engine.
  unsigned f = 0;
  if (ctx.cpu_util() >= 70.0 || ctx.psi_cpu10() >= 25.0 || ctx.gpu_busy() >= 85.0) f |= kHighLoad;
  const bool big_proc = ctx.count([](const ProcessInfo& p) { return p.cpu_pct && *p.cpu_pct >= 20.0 && p.rss_kb >= 2ull * 1024 * 1024; }) > 0;
  if (ctx.mem_used_pct() >= 85.0 || ctx.swap_pages_s() >= 100.0 || big_proc) f |= kResourceHeavy;
  const bool interactive = result_.cls == WorkloadClass::Desktop || result_.cls == WorkloadClass::Browser;
  if (ev[idx(WorkloadClass::Game)].score >= 0.25 || ev[idx(WorkloadClass::VideoPlayback)].score >= 0.35 ||
      (ctx.audio_running() && interactive))
    f |= kLatencySensitive;
  result_.flags = f;

  result_.top.clear();
  std::vector<const ProcessInfo*> busy;
  for (const auto& p : ctx.procs.procs)
    if (p.cpu_pct && *p.cpu_pct >= 1.0) busy.push_back(&p);
  std::sort(busy.begin(), busy.end(), [](auto* a, auto* b) { return *a->cpu_pct > *b->cpu_pct; });
  for (std::size_t i = 0; i < busy.size() && i < 5; ++i) result_.top.push_back({busy[i]->pid, busy[i]->comm, *busy[i]->cpu_pct});
  return result_;
}

std::string format_workload(const WorkloadResult& r, Clock::time_point now) {
  char buf[96];
  std::string s = "Workload:   " + std::string(to_string(r.cls));
  std::snprintf(buf, sizeof buf, "  (confidence %.2f, for %llds, %llu evaluations)\n", r.confidence,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - r.since).count()),
                static_cast<unsigned long long>(r.evaluations));
  s += buf;
  s += "Features:   " + flags_to_string(r.flags) + "\n";
  if (r.pending) s += "Pending:    " + std::string(to_string(*r.pending)) + " (" + std::to_string(r.pending_streak) + " confirmations)\n";
  if (!r.reasons.empty()) {
    s += "Because:\n";
    for (const auto& x : r.reasons) s += "  - " + x + "\n";
  }
  std::vector<std::pair<double, WorkloadClass>> ranked;
  for (std::size_t i = 0; i < kClassCount; ++i)
    if (r.scores[i] > 0.0) ranked.emplace_back(r.scores[i], static_cast<WorkloadClass>(i));
  std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
  s += "Scores:    ";
  for (std::size_t i = 0; i < ranked.size() && i < 5; ++i) {
    std::snprintf(buf, sizeof buf, " %s=%.2f", std::string(to_string(ranked[i].second)).c_str(), ranked[i].first);
    s += buf;
  }
  s += "\n";
  if (!r.top.empty()) {
    s += "Busiest processes:\n";
    for (const auto& p : r.top) {
      std::snprintf(buf, sizeof buf, "  %-18s pid %-7d %6.1f%% of a core\n", p.name.c_str(), p.pid, p.cpu_pct);
      s += buf;
    }
  }
  return s;
}

}  // namespace opt::workload
