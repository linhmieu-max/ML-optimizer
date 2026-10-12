#include "telemetry/collector.hpp"

#include <algorithm>

#include "policy/feedback.hpp"
#include "telemetry/reward.hpp"

namespace opt::telemetry {
namespace {

std::string params(const policy::PolicyDecision& d) {
  const auto& t = d.targets;
  return "cpu=" + std::string(to_string(t.cpu)) + ";boost=" + (t.allow_boost ? "1" : "0") + ";gpu=" + std::string(to_string(t.gpu)) +
         ";io=" + std::string(to_string(t.io)) + ";background=" + std::string(to_string(t.background)) +
         ";memory=" + std::string(to_string(t.memory)) + ";runtime_pm=" + std::string(to_string(t.runtime_pm)) +
         ";source=" + std::string(to_string(d.source)) + ";aggressiveness=" + std::to_string(d.aggressiveness);
}

}  // namespace

db::TelemetryRow RowBuilder::build(const SampleInputs& in) const {
  db::TelemetryRow r;
  r.ts = in.unix_time;
  r.session_id = session_;
  r.device_id_hash = device_;
  r.kernel_version = kernel_;
  r.interval_s = in.interval_s;

  r.workload = in.workload ? std::string(to_string(in.workload->cls)) : "UNKNOWN";
  if (in.workload) r.workload_confidence = in.workload->confidence;
  r.selected_policy = in.decision ? std::string(to_string(in.decision->profile)) : "UNKNOWN";
  if (in.decision) r.policy_parameters = params(*in.decision);

  RewardInput ri;
  if (const auto* s = in.sys) {
    if (s->cpu) {
      r.cpu_utilization = s->cpu->utilization_pct;
      r.cpu_frequency = s->cpu->freq_avg_mhz;
      ri.cpu_util_pct = s->cpu->utilization_pct;
      ri.cpu_freq_mhz = s->cpu->freq_avg_mhz;
      ri.cpu_max_mhz = cpu_max_mhz_;
    }
    if (s->gpu) {
      for (const auto& g : s->gpu->devices) {  // the busiest/fastest device represents the GPU
        if (g.busy_pct && (!r.gpu_utilization || *g.busy_pct > *r.gpu_utilization)) r.gpu_utilization = g.busy_pct;
        if (g.cur_mhz && (!r.gpu_frequency || *g.cur_mhz > *r.gpu_frequency)) r.gpu_frequency = static_cast<double>(*g.cur_mhz);
      }
    }
    if (s->memory) {
      r.ram_used = s->memory->used_pct();
      if (s->memory->swap_in_pages_s) r.swap_activity = *s->memory->swap_in_pages_s + s->memory->swap_out_pages_s.value_or(0);
    }
    if (s->psi) {
      auto some10 = [](const std::optional<monitor::PsiResource>& x) -> std::optional<double> {
        return x && x->some ? std::optional<double>(x->some->avg10) : std::nullopt;
      };
      r.psi_cpu = some10(s->psi->cpu);
      r.psi_memory = some10(s->psi->memory);
      r.psi_io = some10(s->psi->io);
      ri.psi_cpu10 = r.psi_cpu;
      ri.psi_io10 = r.psi_io;
      ri.psi_mem10 = r.psi_memory;
    }
    if (s->thermal) {
      if (const auto* h = s->thermal->hottest()) r.temperature = h->temp_c;
      ri.thermal_margin_c = policy::thermal_margin(*s->thermal);
    }
    if (s->power) {
      if (s->power->rapl_pkg_w) r.power = s->power->rapl_pkg_w;
      else if (s->power->on_ac == false)
        for (const auto& p : s->power->supplies)
          if (p.type == "Battery" && p.power_w) { r.power = p.power_w; break; }
    }
    if (r.power && in.interval_s > 0) r.energy_delta = *r.power * in.interval_s;
    ri.power_w = r.power;
    if (s->io && s->io->total_read_bps) r.io_throughput = *s->io->total_read_bps + s->io->total_write_bps.value_or(0);
  }

  std::string actions;
  auto add = [&](const std::string& a) { actions += (actions.empty() ? "" : ",") + a; };
  if (in.safety != SafetyState::Normal) add("thermal:" + std::string(to_string(in.safety)));
  if (in.blocked) add("gate-blocked:" + std::to_string(in.blocked));
  if (in.failed) add("change-failed:" + std::to_string(in.failed));
  r.safety_action = actions;
  r.rollback = static_cast<std::int64_t>(in.reverted);

  ri.profile_changes = in.profile_changes;
  ri.safety_event = in.safety != SafetyState::Normal || in.blocked > 0 || in.failed > 0;
  r.reward = compute_reward(ri).total;
  r.success = in.safety < SafetyState::Hot && in.failed == 0;
  return r;
}

}  // namespace opt::telemetry
