// Latency route planner:control thread 的純函式 seam。
// 輸入不可變 track/slot 狀態；輸出 RT 可直接執行的 RoutePlan value。
#pragma once

#include <cstdint>
#include <vector>

#include "pdc_planner.hpp"

namespace rmx {

enum class RouteRuntimeState : std::uint8_t {
    kActive,
    kDegraded,
    kSuspended,
};

enum class RouteBus : std::uint8_t { kPrimary, kMonitor };

enum class RouteSlotAction : std::uint8_t {
    kDry,
    kDelayDry,
    kProcess,
    kDrainParameters,
    kReusePrimary,
};

enum class ShadowDisposition : std::uint8_t {
    kNone,
    kCreate,
    kReuse,
    kRelease,
};

struct RouteSlotSpec {
    std::uint32_t instance_id{};
    bool bypassed{};
    bool primary_available{};
    bool primary_latency_known{};
    std::uint64_t primary_latency_samples{};
    RouteRuntimeState primary_state{RouteRuntimeState::kActive};
    bool monitor_bypassed{};
    bool shadow_available{};
    bool shadow_latency_known{};
    std::uint64_t shadow_latency_samples{};
    RouteRuntimeState shadow_state{RouteRuntimeState::kActive};
};

struct RouteTrackSpec {
    std::uint32_t track_id{};
    bool muted{};
    bool is_output{};
    OutputLatencyPolicy latency_policy{OutputLatencyPolicy::kFullPdc};
    std::vector<RouteSlotSpec> slots;
    std::vector<std::uint32_t> dests;
    bool uses_input_bus{true};
};

struct RoutePathPlan {
    RouteSlotAction action{RouteSlotAction::kDry};
    RouteBus bus{RouteBus::kPrimary};
    std::uint64_t dry_delay_samples{};

    bool operator==(const RoutePathPlan&) const = default;
};

struct RouteSlotPlan {
    std::uint32_t instance_id{};
    RoutePathPlan primary;
    RoutePathPlan monitor;
    ShadowDisposition shadow{ShadowDisposition::kNone};

    bool operator==(const RouteSlotPlan&) const = default;
};

struct RouteSendPlan {
    std::uint32_t to_track_id{};
    RouteBus primary_bus{RouteBus::kPrimary};
    RouteBus monitor_bus{RouteBus::kMonitor};
    std::uint64_t primary_delay_samples{};

    bool operator==(const RouteSendPlan&) const = default;
};

struct RouteTrackPlan {
    std::uint32_t track_id{};
    bool monitor_required{};
    bool monitor_diverged{};
    bool muted{};
    std::vector<RouteSlotPlan> slots;
    std::vector<RouteSendPlan> sends;
    RouteBus output_bus{RouteBus::kPrimary};

    bool operator==(const RouteTrackPlan&) const = default;
};

struct RoutePlan {
    PdcPlanResult latency;
    std::vector<RouteTrackPlan> tracks;
    bool monitor_variants_needed{};

    [[nodiscard]] bool ok() const noexcept { return latency.ok(); }
};

RoutePlan plan_routes(const std::vector<RouteTrackSpec>& tracks,
                      const PdcLimits& limits);

// Shadow 準備期間的暫態 plan：保留現行 PDC/bus routing，只停用所有 plugin process。
RoutePlan plan_route_suspension(RoutePlan active);

}  // namespace rmx
