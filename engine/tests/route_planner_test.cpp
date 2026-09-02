// Route planner 的 public seam 測試：只觀察純 RoutePlan value。
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "route_planner.hpp"

namespace {

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
            std::exit(1);                                                         \
        }                                                                         \
    } while (0)

rmx::PdcLimits limits() {
    return {96000, 256u * 1024u * 1024u, 2, sizeof(float), 512};
}

rmx::RouteSlotSpec active_slot(std::uint32_t id, std::uint64_t latency) {
    rmx::RouteSlotSpec slot;
    slot.instance_id = id;
    slot.primary_available = true;
    slot.primary_latency_known = true;
    slot.primary_latency_samples = latency;
    return slot;
}

const rmx::PdcOutputLatency& output_latency(const rmx::RoutePlan& plan,
                                            std::uint32_t track_id) {
    for (const auto& output : plan.latency.outputs)
        if (output.track_id == track_id) return output;
    std::fprintf(stderr, "missing output latency for track %u\n", track_id);
    std::exit(1);
}

const rmx::RouteTrackPlan& find_track(const rmx::RoutePlan& plan, std::uint32_t track_id) {
    for (const auto& track : plan.tracks)
        if (track.track_id == track_id) return track;
    std::fprintf(stderr, "missing track plan for track %u\n", track_id);
    std::exit(1);
}

}  // namespace

int main() {
    {
        const auto plan = rmx::plan_routes(
            {
                {1, false, false, rmx::OutputLatencyPolicy::kFullPdc,
                 {active_slot(10, 128)}, {2}},
                {2, false, true, rmx::OutputLatencyPolicy::kLowLatency, {}, {}},
            },
            limits());

        CHECK(plan.ok());
        CHECK(plan.tracks.size() == 2);
        const auto& input = plan.tracks[0];
        CHECK(input.monitor_required);
        CHECK(!input.monitor_diverged);
        CHECK(input.slots.size() == 1);
        CHECK(input.slots[0].primary.action == rmx::RouteSlotAction::kProcess);
        CHECK(input.slots[0].primary.bus == rmx::RouteBus::kPrimary);
        CHECK(input.slots[0].primary.dry_delay_samples == 128);
        CHECK(input.slots[0].monitor.action == rmx::RouteSlotAction::kReusePrimary);
        CHECK(input.slots[0].monitor.bus == rmx::RouteBus::kPrimary);
        CHECK(input.slots[0].shadow == rmx::ShadowDisposition::kNone);
        CHECK(plan.tracks[1].output_bus == rmx::RouteBus::kMonitor);
        CHECK(plan.latency.outputs.size() == 1);
        CHECK(plan.latency.outputs[0].total_plugin_delay_samples == 128);
    }

    {
        auto bypassed_on_monitor = active_slot(20, 128);
        bypassed_on_monitor.monitor_bypassed = true;
        const auto plan = rmx::plan_routes(
            {
                {1, false, false, rmx::OutputLatencyPolicy::kFullPdc,
                 {bypassed_on_monitor, active_slot(21, 64)}, {2}},
                {2, false, true, rmx::OutputLatencyPolicy::kLowLatency, {}, {}},
            },
            limits());

        CHECK(plan.ok());
        const auto& input = plan.tracks[0];
        CHECK(input.monitor_diverged);
        CHECK(input.slots[0].monitor.action == rmx::RouteSlotAction::kDry);
        CHECK(input.slots[0].monitor.bus == rmx::RouteBus::kMonitor);
        CHECK(input.slots[0].shadow == rmx::ShadowDisposition::kNone);
        CHECK(input.slots[1].monitor.action == rmx::RouteSlotAction::kDry);
        CHECK(input.slots[1].shadow == rmx::ShadowDisposition::kCreate);
        CHECK(plan.latency.outputs[0].total_plugin_delay_samples == 0);
    }

    {
        auto divergence = active_slot(30, 16);
        divergence.monitor_bypassed = true;
        auto suspended = active_slot(31, 64);
        suspended.primary_state = rmx::RouteRuntimeState::kSuspended;
        suspended.shadow_available = true;
        suspended.shadow_latency_known = true;
        suspended.shadow_latency_samples = 64;
        const auto plan = rmx::plan_routes(
            {
                {1, false, false, rmx::OutputLatencyPolicy::kFullPdc,
                 {divergence, suspended}, {2}},
                {2, false, true, rmx::OutputLatencyPolicy::kLowLatency, {}, {}},
            },
            limits());

        CHECK(plan.ok());
        const auto& suspended_plan = plan.tracks[0].slots[1];
        CHECK(suspended_plan.primary.action == rmx::RouteSlotAction::kDrainParameters);
        CHECK(suspended_plan.monitor.action == rmx::RouteSlotAction::kDry);
        CHECK(suspended_plan.monitor.bus == rmx::RouteBus::kMonitor);
        CHECK(suspended_plan.shadow == rmx::ShadowDisposition::kRelease);
    }

    {
        const auto plan = rmx::plan_routes(
            {{1, false, true, rmx::OutputLatencyPolicy::kFullPdc,
              {active_slot(40, (std::numeric_limits<std::uint64_t>::max)()),
               active_slot(41, 1)},
              {}}},
            limits());
        CHECK(plan.latency.error == rmx::PdcPlanError::kArithmeticOverflow);
    }

    {
        constexpr std::uint64_t samples[] = {0, 1, 511, 512, 513, 2048, 96000};
        for (const auto sample_count : samples) {
            const auto plan = rmx::plan_routes(
                {{1, false, true, rmx::OutputLatencyPolicy::kFullPdc,
                  {active_slot(50, sample_count)}, {}}},
                limits());
            CHECK(plan.ok());
            CHECK(plan.tracks[0].slots[0].primary.dry_delay_samples == sample_count);
            CHECK(output_latency(plan, 1).total_plugin_delay_samples == sample_count);
        }
        const auto over_limit = rmx::plan_routes(
            {{1, false, true, rmx::OutputLatencyPolicy::kFullPdc,
              {active_slot(50, 96001)}, {}}},
            limits());
        CHECK(over_limit.latency.error == rmx::PdcPlanError::kPathLimitExceeded);

        auto bypassed_over_limit = active_slot(51, 96001);
        bypassed_over_limit.monitor_bypassed = true;
        const auto low_latency_bypass = rmx::plan_routes(
            {{1, false, true, rmx::OutputLatencyPolicy::kLowLatency,
              {bypassed_over_limit}, {}}},
            limits());
        CHECK(low_latency_bypass.ok());
        CHECK(output_latency(low_latency_bypass, 1).total_plugin_delay_samples == 0);
    }

    {
        auto globally_bypassed = active_slot(60, 64);
        globally_bypassed.bypassed = true;
        auto placeholder = active_slot(61, 128);
        placeholder.primary_available = false;
        placeholder.primary_latency_known = false;
        const auto plan = rmx::plan_routes(
            {{1, false, true, rmx::OutputLatencyPolicy::kFullPdc,
              {globally_bypassed, placeholder}, {}}},
            limits());
        CHECK(plan.ok());
        CHECK(plan.tracks[0].slots[0].primary.action == rmx::RouteSlotAction::kDry);
        CHECK(plan.tracks[0].slots[1].primary.action == rmx::RouteSlotAction::kDry);
        CHECK(output_latency(plan, 1).total_plugin_delay_samples == 0);
    }

    {
        auto divergence = active_slot(70, 16);
        divergence.monitor_bypassed = true;
        auto shadowed = active_slot(71, 64);
        shadowed.shadow_available = true;
        shadowed.shadow_latency_known = true;
        shadowed.shadow_latency_samples = 32;
        const auto plan = rmx::plan_routes(
            {
                {1, false, false, rmx::OutputLatencyPolicy::kFullPdc,
                 {divergence, shadowed}, {2}},
                {2, false, true, rmx::OutputLatencyPolicy::kLowLatency, {}, {}},
            },
            limits());
        const auto& shadow_plan = plan.tracks[0].slots[1];
        CHECK(shadow_plan.monitor.action == rmx::RouteSlotAction::kProcess);
        CHECK(shadow_plan.monitor.dry_delay_samples == 32);
        CHECK(shadow_plan.shadow == rmx::ShadowDisposition::kReuse);
        CHECK(output_latency(plan, 2).total_plugin_delay_samples == 32);

        shadowed.shadow_state = rmx::RouteRuntimeState::kDegraded;
        const auto degraded = rmx::plan_routes(
            {
                {1, false, false, rmx::OutputLatencyPolicy::kFullPdc,
                 {divergence, shadowed}, {2}},
                {2, false, true, rmx::OutputLatencyPolicy::kLowLatency, {}, {}},
            },
            limits());
        CHECK(degraded.tracks[0].slots[1].monitor.action ==
              rmx::RouteSlotAction::kDrainParameters);
        CHECK(output_latency(degraded, 2).total_plugin_delay_samples == 0);
    }

    {
        auto fast = active_slot(81, 0);
        fast.shadow_available = true;
        fast.shadow_latency_known = true;
        auto slow = active_slot(82, 64);
        slow.shadow_available = true;
        slow.shadow_latency_known = true;
        slow.shadow_latency_samples = 64;
        auto downstream = active_slot(80, 32);
        downstream.shadow_available = true;
        downstream.shadow_latency_known = true;
        downstream.shadow_latency_samples = 24;
        const std::vector<rmx::RouteTrackSpec> graph{
            {1, false, false, rmx::OutputLatencyPolicy::kFullPdc,
             {fast}, {3}},
            {2, false, false, rmx::OutputLatencyPolicy::kFullPdc,
             {slow}, {3}},
            {3, false, false, rmx::OutputLatencyPolicy::kFullPdc,
             {downstream}, {4, 5}},
            {4, false, true, rmx::OutputLatencyPolicy::kFullPdc, {}, {}},
            {5, false, true, rmx::OutputLatencyPolicy::kLowLatency, {}, {}},
        };
        const auto plan = rmx::plan_routes(graph, limits());
        CHECK(plan.ok());
        CHECK(plan.tracks[0].sends[0].primary_delay_samples == 64);
        CHECK(plan.tracks[1].sends[0].primary_delay_samples == 0);
        CHECK(plan.tracks[2].monitor_diverged);
        CHECK(plan.monitor_variants_needed);
        CHECK(plan.tracks[2].slots[0].monitor.action == rmx::RouteSlotAction::kProcess);
        CHECK(output_latency(plan, 4).total_plugin_delay_samples == 96);
        CHECK(output_latency(plan, 5).total_plugin_delay_samples == 88);

        auto suspended_shadow_graph = graph;
        suspended_shadow_graph[2].slots[0].shadow_state =
            rmx::RouteRuntimeState::kSuspended;
        const auto suspended_shadow = rmx::plan_routes(suspended_shadow_graph, limits());
        CHECK(output_latency(suspended_shadow, 4).total_plugin_delay_samples == 96);
        CHECK(output_latency(suspended_shadow, 5).total_plugin_delay_samples == 64);
        CHECK(suspended_shadow.tracks[2].slots[0].monitor.action ==
              rmx::RouteSlotAction::kDrainParameters);
    }

    {
        const std::vector<rmx::RouteTrackSpec> audible{
            {1, false, false, rmx::OutputLatencyPolicy::kFullPdc,
             {active_slot(90, 64)}, {2}},
            {2, false, true, rmx::OutputLatencyPolicy::kFullPdc, {}, {}},
        };
        auto muted = audible;
        muted[0].muted = true;
        const auto audible_plan = rmx::plan_routes(audible, limits());
        const auto muted_plan = rmx::plan_routes(muted, limits());
        CHECK(!audible_plan.tracks[0].muted);
        CHECK(muted_plan.tracks[0].muted);
        CHECK(output_latency(audible_plan, 2).total_plugin_delay_samples ==
              output_latency(muted_plan, 2).total_plugin_delay_samples);
        CHECK(audible_plan.latency.edge_delays.size() ==
              muted_plan.latency.edge_delays.size());
        for (std::size_t i = 0; i < audible_plan.latency.edge_delays.size(); ++i) {
            CHECK(audible_plan.latency.edge_delays[i].from_track_id ==
                  muted_plan.latency.edge_delays[i].from_track_id);
            CHECK(audible_plan.latency.edge_delays[i].to_track_id ==
                  muted_plan.latency.edge_delays[i].to_track_id);
            CHECK(audible_plan.latency.edge_delays[i].delay_samples ==
                  muted_plan.latency.edge_delays[i].delay_samples);
        }
    }

    {
        auto unused_shadow = active_slot(100, 64);
        unused_shadow.shadow_available = true;
        const auto plan = rmx::plan_routes(
            {{1, false, true, rmx::OutputLatencyPolicy::kFullPdc,
              {unused_shadow}, {}}},
            limits());
        CHECK(plan.tracks[0].slots[0].shadow == rmx::ShadowDisposition::kRelease);
    }

    {
        auto preconfigured = active_slot(110, 64);
        preconfigured.monitor_bypassed = true;
        const auto plan = rmx::plan_routes(
            {{1, false, true, rmx::OutputLatencyPolicy::kFullPdc,
              {preconfigured}, {}}},
            limits());
        CHECK(!plan.monitor_variants_needed);
        CHECK(!plan.tracks[0].monitor_diverged);
        CHECK(plan.tracks[0].slots[0].shadow == rmx::ShadowDisposition::kNone);
        CHECK(plan.tracks[0].slots[0].monitor.action ==
              rmx::RouteSlotAction::kReusePrimary);
    }

    {
        auto bypass = active_slot(120, 16);
        bypass.monitor_bypassed = true;
        auto shadowed = active_slot(121, 32);
        shadowed.shadow_available = true;
        shadowed.shadow_latency_known = true;
        shadowed.shadow_latency_samples = 32;
        const auto active = rmx::plan_routes(
            {{1, false, false, rmx::OutputLatencyPolicy::kFullPdc,
              {bypass, shadowed}, {2}},
             {2, false, true, rmx::OutputLatencyPolicy::kLowLatency, {}, {}}},
            limits());
        const auto suspended = rmx::plan_route_suspension(active);
        CHECK(suspended.latency.edge_delays.size() == active.latency.edge_delays.size());
        CHECK(suspended.tracks[0].sends == active.tracks[0].sends);
        CHECK(suspended.tracks[0].slots[0].primary.action ==
              rmx::RouteSlotAction::kDelayDry);
        CHECK(suspended.tracks[0].slots[1].primary.action ==
              rmx::RouteSlotAction::kDelayDry);
        CHECK(suspended.tracks[0].slots[0].monitor.action ==
              rmx::RouteSlotAction::kDry);
        CHECK(suspended.tracks[0].slots[1].monitor.action ==
              rmx::RouteSlotAction::kDelayDry);
        CHECK(suspended.tracks[0].slots[1].shadow ==
              active.tracks[0].slots[1].shadow);
    }

    {
        // 決策矩陣:slot 狀態軸 × Output Latency Policy → primary action 與 Total Plugin Delay。
        struct Case {
            const char* name;
            bool bypassed;
            bool monitor_bypassed;
            bool primary_available;
            rmx::RouteRuntimeState primary_state;
            rmx::OutputLatencyPolicy policy;
            std::uint64_t latency;
            rmx::RouteSlotAction expected_primary;
            std::uint64_t expected_total;
        };
        const Case cases[] = {
            {"baseline",             false, false, true,  rmx::RouteRuntimeState::kActive,
             rmx::OutputLatencyPolicy::kFullPdc, 64, rmx::RouteSlotAction::kProcess, 64},
            {"low-latency output",   false, false, true,  rmx::RouteRuntimeState::kActive,
             rmx::OutputLatencyPolicy::kLowLatency, 64, rmx::RouteSlotAction::kProcess, 64},
            {"global bypass",        true,  false, true,  rmx::RouteRuntimeState::kActive,
             rmx::OutputLatencyPolicy::kFullPdc, 64, rmx::RouteSlotAction::kDry, 0},
            {"placeholder",          false, false, false, rmx::RouteRuntimeState::kActive,
             rmx::OutputLatencyPolicy::kFullPdc, 64, rmx::RouteSlotAction::kDry, 0},
            {"monitor bypass, full", false, true,  true,  rmx::RouteRuntimeState::kActive,
             rmx::OutputLatencyPolicy::kFullPdc, 64, rmx::RouteSlotAction::kProcess, 64},
        };
        for (const auto& c : cases) {
            rmx::RouteSlotSpec slot = active_slot(200, c.latency);
            slot.bypassed = c.bypassed;
            slot.monitor_bypassed = c.monitor_bypassed;
            slot.primary_available = c.primary_available;
            slot.primary_latency_known = c.primary_available;
            slot.primary_state = c.primary_state;
            const auto plan = rmx::plan_routes(
                {{1, false, true, c.policy, {slot}, {}}}, limits());
            CHECK(plan.ok());
            CHECK(plan.tracks[0].slots[0].primary.action == c.expected_primary);
            CHECK(output_latency(plan, 1).total_plugin_delay_samples == c.expected_total);
        }
    }

    {
        // 嵌套匯流:bus 2→{3,4} 的成員 4 自己再匯流(收 2 與 6)。
        // 路徑 1→2→4 = 64+16 = 80;路徑 6→4 = 32 → 匯流點 4 給 6 補 48。
        const std::vector<rmx::RouteTrackSpec> graph{
            {1, false, false, rmx::OutputLatencyPolicy::kFullPdc,
             {active_slot(300, 64)}, {2}},
            {2, false, false, rmx::OutputLatencyPolicy::kFullPdc,
             {active_slot(301, 16)}, {3, 4}},
            {3, false, true, rmx::OutputLatencyPolicy::kFullPdc, {}, {}},
            {4, false, false, rmx::OutputLatencyPolicy::kFullPdc, {}, {5}},
            {5, false, true, rmx::OutputLatencyPolicy::kFullPdc, {}, {}},
            {6, false, false, rmx::OutputLatencyPolicy::kFullPdc,
             {active_slot(302, 32)}, {4}},
        };
        const auto plan = rmx::plan_routes(graph, limits());
        CHECK(plan.ok());
        CHECK(find_track(plan, 6).sends[0].primary_delay_samples == 48);
        CHECK(output_latency(plan, 3).total_plugin_delay_samples == 80);
        CHECK(output_latency(plan, 5).total_plugin_delay_samples == 80);
    }

    std::printf("route_planner_test PASSED\n");
    return 0;
}
