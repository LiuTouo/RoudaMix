// Plugin Delay Compensation planner:control thread 的純函式 seam。
// 輸入不可變 DAG、primary/monitor chain latency 與 output policy；輸出 full-PDC
// edge delay、每個 output 的 Total Plugin Delay，以及配置需求或明確錯誤。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rmx {

enum class OutputLatencyPolicy : std::uint8_t { kFullPdc, kLowLatency };

inline const char* output_latency_policy_str(OutputLatencyPolicy policy) noexcept {
    return policy == OutputLatencyPolicy::kLowLatency ? "lowLatency" : "fullPdc";
}

struct PdcNodeSpec {
    std::uint32_t track_id{};
    std::uint64_t primary_latency_samples{};
    std::uint64_t monitor_latency_samples{};
    std::vector<std::uint32_t> dests;
};

struct PdcOutputSpec {
    std::uint32_t track_id{};
    OutputLatencyPolicy policy{OutputLatencyPolicy::kFullPdc};
};

struct PdcLimits {
    std::uint64_t max_path_samples{};
    std::size_t max_buffer_bytes{};
    std::uint32_t channels{2};
    std::uint32_t bytes_per_sample{sizeof(float)};
    std::uint32_t max_block_frames{};
};

struct PdcEdgeDelay {
    std::uint32_t from_track_id{};
    std::uint32_t to_track_id{};
    std::uint64_t delay_samples{};
};

struct PdcOutputLatency {
    std::uint32_t track_id{};
    std::uint64_t total_plugin_delay_samples{};
    std::uint64_t compensation_delay_samples{};
    bool synchronized{};
};

enum class PdcPlanError : std::uint8_t {
    kNone,
    kDuplicateTrackId,
    kUnknownDestination,
    kUnknownOutput,
    kCycle,
    kPathLimitExceeded,
    kMemoryLimitExceeded,
    kArithmeticOverflow,
};

struct PdcPlanResult {
    PdcPlanError error{PdcPlanError::kNone};
    std::vector<PdcEdgeDelay> edge_delays;
    std::vector<PdcOutputLatency> outputs;
    std::size_t buffer_bytes{};

    [[nodiscard]] bool ok() const noexcept { return error == PdcPlanError::kNone; }
};

PdcPlanResult plan_plugin_delay(const std::vector<PdcNodeSpec>& nodes,
                                const std::vector<PdcOutputSpec>& outputs,
                                const PdcLimits& limits);

}  // namespace rmx
