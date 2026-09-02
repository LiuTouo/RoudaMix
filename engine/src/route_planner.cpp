#include "route_planner.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace rmx {

namespace {

bool primary_processes(const RouteSlotSpec& slot) {
    return !slot.bypassed && slot.primary_available &&
           slot.primary_state != RouteRuntimeState::kSuspended;
}

std::uint64_t primary_latency(const RouteSlotSpec& slot) {
    return primary_processes(slot) && slot.primary_latency_known
               ? slot.primary_latency_samples
               : 0u;
}

bool add_latency(std::uint64_t value, std::uint64_t& total) {
    if (value > (std::numeric_limits<std::uint64_t>::max)() - total) return false;
    total += value;
    return true;
}

}  // namespace

RoutePlan plan_routes(const std::vector<RouteTrackSpec>& tracks,
                      const PdcLimits& limits) {
    RoutePlan result;
    std::vector<PdcNodeSpec> latency_nodes;
    std::vector<PdcOutputSpec> latency_outputs;
    latency_nodes.reserve(tracks.size());
    for (const auto& track : tracks) {
        std::uint64_t latency = 0;
        for (const auto& slot : track.slots) {
            if (!add_latency(primary_latency(slot), latency)) {
                result.latency.error = PdcPlanError::kArithmeticOverflow;
                return result;
            }
        }
        // 第一階段只規劃 full-PDC edges；low-latency 的有效 latency 必須等
        // slot route/shadow 決策完成後，於第二階段填入並正式驗證。
        latency_nodes.push_back({track.track_id, latency, 0u, track.dests});
        if (track.is_output)
            latency_outputs.push_back({track.track_id, track.latency_policy});
    }
    const auto primary_plan = plan_plugin_delay(latency_nodes, latency_outputs, limits);
    result.latency = primary_plan;
    if (!result.ok()) return result;

    bool has_monitor_bypass = false;
    bool has_low_latency = false;
    bool has_full_pdc = false;
    for (const auto& track : tracks) {
        if (track.is_output) {
            has_low_latency = has_low_latency ||
                              track.latency_policy == OutputLatencyPolicy::kLowLatency;
            has_full_pdc = has_full_pdc ||
                           track.latency_policy == OutputLatencyPolicy::kFullPdc;
        }
        for (const auto& slot : track.slots)
            has_monitor_bypass = has_monitor_bypass || slot.monitor_bypassed;
    }
    const bool pdc_splits_buses = std::any_of(
        primary_plan.edge_delays.begin(), primary_plan.edge_delays.end(),
        [](const PdcEdgeDelay& edge) { return edge.delay_samples > 0; });
    result.monitor_variants_needed =
        has_monitor_bypass || (has_low_latency && has_full_pdc && pdc_splits_buses);

    std::unordered_map<std::uint32_t, std::size_t> index;
    index.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) index.emplace(tracks[i].track_id, i);
    std::vector<std::vector<std::size_t>> incoming(tracks.size());
    std::vector<std::size_t> indegree(tracks.size(), 0);
    for (std::size_t from = 0; from < tracks.size(); ++from)
        for (const auto dest_id : tracks[from].dests) {
            incoming[index.at(dest_id)].push_back(from);
            ++indegree[index.at(dest_id)];
        }

    std::vector<std::size_t> order;
    order.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (indegree[i] == 0) order.push_back(i);
    for (std::size_t cursor = 0; cursor < order.size(); ++cursor) {
        for (const auto dest_id : tracks[order[cursor]].dests) {
            const auto dest = index.at(dest_id);
            if (--indegree[dest] == 0) order.push_back(dest);
        }
    }

    std::vector<bool> reaches_low_latency(tracks.size(), false);
    std::vector<std::size_t> pending;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].is_output &&
            tracks[i].latency_policy == OutputLatencyPolicy::kLowLatency) {
            reaches_low_latency[i] = true;
            pending.push_back(i);
        }
    }
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        for (const auto from : incoming[pending[cursor]]) {
            if (reaches_low_latency[from]) continue;
            reaches_low_latency[from] = true;
            pending.push_back(from);
        }
    }

    result.tracks.resize(tracks.size());
    std::vector<std::uint64_t> monitor_chain_latency(tracks.size(), 0);
    const auto pdc_delay = [&](std::uint32_t from, std::uint32_t to) {
        const auto found = std::find_if(
            primary_plan.edge_delays.begin(), primary_plan.edge_delays.end(),
            [&](const PdcEdgeDelay& edge) {
                return edge.from_track_id == from && edge.to_track_id == to;
            });
        return found == primary_plan.edge_delays.end() ? 0u : found->delay_samples;
    };
    for (const auto i : order) {
        const auto& track = tracks[i];
        RouteTrackPlan planned;
        planned.track_id = track.track_id;
        planned.monitor_required = reaches_low_latency[i] || result.monitor_variants_needed;
        planned.muted = track.muted;
        planned.output_bus = track.latency_policy == OutputLatencyPolicy::kLowLatency
                                 ? RouteBus::kMonitor
                                 : RouteBus::kPrimary;
        const bool monitor_diverged = result.monitor_variants_needed;
        planned.slots.reserve(track.slots.size());
        for (const auto& slot : track.slots) {
            RouteSlotPlan slot_plan;
            slot_plan.instance_id = slot.instance_id;
            if (result.monitor_variants_needed && slot.primary_available)
                slot_plan.shadow = slot.shadow_available ? ShadowDisposition::kReuse
                                                        : ShadowDisposition::kCreate;
            else
                slot_plan.shadow = slot.shadow_available ? ShadowDisposition::kRelease
                                                        : ShadowDisposition::kNone;
            if (primary_processes(slot)) {
                slot_plan.primary = {RouteSlotAction::kProcess, RouteBus::kPrimary,
                                     slot.primary_latency_known
                                         ? slot.primary_latency_samples
                                         : 0u};
            } else if (!slot.bypassed && slot.primary_available &&
                       slot.primary_state == RouteRuntimeState::kSuspended) {
                slot_plan.primary = {RouteSlotAction::kDrainParameters,
                                     RouteBus::kPrimary, 0u};
            }
            const bool primary_is_dry =
                slot_plan.primary.action != RouteSlotAction::kProcess;
            if (!planned.monitor_required) {
                slot_plan.monitor = {RouteSlotAction::kReusePrimary, RouteBus::kPrimary, 0u};
            } else if (!monitor_diverged) {
                slot_plan.monitor = {RouteSlotAction::kReusePrimary, RouteBus::kPrimary, 0u};
                if (!primary_is_dry && slot.primary_latency_known &&
                    !add_latency(slot.primary_latency_samples, monitor_chain_latency[i])) {
                    result.latency.error = PdcPlanError::kArithmeticOverflow;
                    result.tracks.clear();
                    return result;
                }
            } else if (primary_is_dry) {
                slot_plan.monitor = {RouteSlotAction::kDry, RouteBus::kMonitor, 0u};
            } else if (slot.monitor_bypassed) {
                slot_plan.monitor = {RouteSlotAction::kDry, RouteBus::kMonitor, 0u};
            } else if (!slot.shadow_available) {
                slot_plan.monitor = {RouteSlotAction::kDry, RouteBus::kMonitor, 0u};
            } else if (slot.shadow_state == RouteRuntimeState::kActive) {
                slot_plan.monitor = {RouteSlotAction::kProcess, RouteBus::kMonitor,
                                     slot.shadow_latency_known
                                         ? slot.shadow_latency_samples
                                         : 0u};
                if (slot.shadow_latency_known &&
                    !add_latency(slot.shadow_latency_samples, monitor_chain_latency[i])) {
                    result.latency.error = PdcPlanError::kArithmeticOverflow;
                    result.tracks.clear();
                    return result;
                }
            } else {
                slot_plan.monitor = {RouteSlotAction::kDrainParameters,
                                     RouteBus::kMonitor, 0u};
            }
            planned.slots.push_back(slot_plan);
        }
        planned.monitor_diverged = monitor_diverged;
        planned.sends.reserve(track.dests.size());
        for (const auto dest_id : track.dests) {
            const auto delay = pdc_delay(track.track_id, dest_id);
            planned.sends.push_back({dest_id, RouteBus::kPrimary, RouteBus::kMonitor,
                                     delay});
        }
        result.tracks[i] = std::move(planned);
    }

    for (std::size_t i = 0; i < latency_nodes.size(); ++i)
        latency_nodes[i].monitor_latency_samples = monitor_chain_latency[i];
    result.latency = plan_plugin_delay(latency_nodes, latency_outputs, limits);
    return result;
}

}  // namespace rmx
