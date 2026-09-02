#include "route_planner.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "graph_topo.hpp"

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

    const auto build = graph::build_topo(tracks);
    const auto& g = build.graph;
    // 拓撲已在第一階段由 plan_plugin_delay 驗證;此防線只在未知缺陷時 fail closed。
    if (build.duplicate_id || build.unknown_dest || !g.complete()) {
        result.latency.error = PdcPlanError::kCycle;
        result.tracks.clear();
        return result;
    }

    std::vector<std::size_t> low_latency_seeds;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].is_output &&
            tracks[i].latency_policy == OutputLatencyPolicy::kLowLatency)
            low_latency_seeds.push_back(i);
    const std::vector<bool> reaches_low_latency = graph::reverse_reach(g, low_latency_seeds);

    result.tracks.resize(tracks.size());
    std::vector<std::uint64_t> monitor_chain_latency(tracks.size(), 0);
    std::vector<bool> incoming_monitor_diverged(tracks.size(), false);
    const auto pdc_delay = [&](std::uint32_t from, std::uint32_t to) {
        const auto found = std::find_if(
            primary_plan.edge_delays.begin(), primary_plan.edge_delays.end(),
            [&](const PdcEdgeDelay& edge) {
                return edge.from_track_id == from && edge.to_track_id == to;
            });
        return found == primary_plan.edge_delays.end() ? 0u : found->delay_samples;
    };
    for (const auto i : g.order) {
        const auto& track = tracks[i];
        RouteTrackPlan planned;
        planned.track_id = track.track_id;
        planned.monitor_required = reaches_low_latency[i];
        planned.muted = track.muted;
        planned.output_bus = track.latency_policy == OutputLatencyPolicy::kLowLatency
                                 ? RouteBus::kMonitor
                                 : RouteBus::kPrimary;
        bool monitor_diverged = planned.monitor_required && track.uses_input_bus &&
                                incoming_monitor_diverged[i];
        planned.slots.reserve(track.slots.size());
        for (const auto& slot : track.slots) {
            RouteSlotPlan slot_plan;
            slot_plan.instance_id = slot.instance_id;
            // 預設 shadow 處置:有 shadow 即釋放;分歧/重用分支僅在需要時覆寫。
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
                slot_plan.monitor = {RouteSlotAction::kReusePrimary, RouteBus::kPrimary, 0u};            } else if (primary_is_dry) {
                slot_plan.monitor = monitor_diverged
                                        ? RoutePathPlan{RouteSlotAction::kDry,
                                                        RouteBus::kMonitor, 0u}
                                        : RoutePathPlan{RouteSlotAction::kReusePrimary,
                                                        RouteBus::kPrimary, 0u};            } else if (!monitor_diverged && !slot.monitor_bypassed) {
                slot_plan.monitor = {RouteSlotAction::kReusePrimary, RouteBus::kPrimary, 0u};                if (slot.primary_latency_known &&
                    !add_latency(slot.primary_latency_samples, monitor_chain_latency[i])) {
                    result.latency.error = PdcPlanError::kArithmeticOverflow;
                    result.tracks.clear();
                    return result;
                }
            } else if (slot.monitor_bypassed) {
                slot_plan.monitor = {RouteSlotAction::kDry, RouteBus::kMonitor, 0u};                monitor_diverged = true;
            } else if (!slot.shadow_available) {
                slot_plan.monitor = {RouteSlotAction::kDry, RouteBus::kMonitor, 0u};
                slot_plan.shadow = ShadowDisposition::kCreate;
                monitor_diverged = true;
            } else if (slot.shadow_state == RouteRuntimeState::kActive) {
                slot_plan.monitor = {RouteSlotAction::kProcess, RouteBus::kMonitor,
                                     slot.shadow_latency_known
                                         ? slot.shadow_latency_samples
                                         : 0u};
                slot_plan.shadow = ShadowDisposition::kReuse;
                if (slot.shadow_latency_known &&
                    !add_latency(slot.shadow_latency_samples, monitor_chain_latency[i])) {
                    result.latency.error = PdcPlanError::kArithmeticOverflow;
                    result.tracks.clear();
                    return result;
                }
                monitor_diverged = true;
            } else {
                slot_plan.monitor = {RouteSlotAction::kDrainParameters,
                                     RouteBus::kMonitor, 0u};
                slot_plan.shadow = ShadowDisposition::kReuse;
                monitor_diverged = true;
            }
            planned.slots.push_back(slot_plan);
        }
        planned.monitor_diverged = monitor_diverged;
        result.monitor_variants_needed =
            result.monitor_variants_needed || monitor_diverged;
        planned.sends.reserve(track.dests.size());
        for (const auto dest_id : track.dests) {
            const auto delay = pdc_delay(track.track_id, dest_id);
            planned.sends.push_back({dest_id, RouteBus::kPrimary, RouteBus::kMonitor,
                                     delay});
            const auto dest = g.index.at(dest_id);
            if (reaches_low_latency[dest] && tracks[dest].uses_input_bus &&
                (monitor_diverged || delay > 0))
                incoming_monitor_diverged[dest] = true;
        }
        result.tracks[i] = std::move(planned);
    }

    for (std::size_t i = 0; i < latency_nodes.size(); ++i)
        latency_nodes[i].monitor_latency_samples = monitor_chain_latency[i];
    result.latency = plan_plugin_delay(latency_nodes, latency_outputs, limits);
    return result;
}

RoutePlan plan_route_suspension(RoutePlan active) {
    for (auto& track : active.tracks) {
        for (auto& slot : track.slots) {
            const auto suspend = [](RoutePathPlan path) {
                if (path.action == RouteSlotAction::kProcess)
                    path.action = RouteSlotAction::kDelayDry;
                else if (path.action != RouteSlotAction::kReusePrimary)
                    path = {RouteSlotAction::kDry, path.bus, 0u};
                return path;
            };
            slot.primary = suspend(slot.primary);
            slot.monitor = suspend(slot.monitor);
        }
    }
    return active;
}

}  // namespace rmx
