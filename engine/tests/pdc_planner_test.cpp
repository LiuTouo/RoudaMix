// PDC planner 的 public seam 測試：只觀察 edge delay、output total 與錯誤。
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "pdc_planner.hpp"

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

int main() {
    const rmx::PdcLimits limits{96000, 256u * 1024u * 1024u};

    // 1. full-PDC fan-in:較快分支等待最慢分支，output total 包含自身 chain。
    {
        const std::vector<rmx::PdcNodeSpec> nodes{
            {1, 0, 0, {3}},
            {2, 128, 128, {3}},
            {3, 32, 32, {}},
        };
        const std::vector<rmx::PdcOutputSpec> outputs{
            {3, rmx::OutputLatencyPolicy::kFullPdc},
        };
        const auto plan = rmx::plan_plugin_delay(nodes, outputs, limits);
        CHECK(plan.ok());
        CHECK(plan.outputs.size() == 1);
        CHECK(plan.outputs[0].track_id == 3);
        CHECK(plan.outputs[0].total_plugin_delay_samples == 160);
        CHECK(plan.outputs[0].compensation_delay_samples == 128);
        CHECK(plan.outputs[0].synchronized);
        CHECK(plan.edge_delays.size() == 2);
        CHECK(plan.edge_delays[0].from_track_id == 1);
        CHECK(plan.edge_delays[0].to_track_id == 3);
        CHECK(plan.edge_delays[0].delay_samples == 128);
        CHECK(plan.edge_delays[1].from_track_id == 2);
        CHECK(plan.edge_delays[1].delay_samples == 0);
        CHECK(plan.buffer_bytes == 2u * (96000u + 1u) * 2u * sizeof(float));
    }

    // 2. low-latency fan-in:使用 monitor latency，只報最長 path，不加入補償。
    {
        const std::vector<rmx::PdcNodeSpec> nodes{
            {1, 400, 0, {3}},
            {2, 512, 64, {3}},
            {3, 128, 16, {}},
        };
        const std::vector<rmx::PdcOutputSpec> outputs{
            {3, rmx::OutputLatencyPolicy::kLowLatency},
        };
        const auto plan = rmx::plan_plugin_delay(nodes, outputs, limits);
        CHECK(plan.ok());
        CHECK(plan.outputs.size() == 1);
        CHECK(plan.outputs[0].total_plugin_delay_samples == 80);
        CHECK(plan.outputs[0].compensation_delay_samples == 0);
        CHECK(!plan.outputs[0].synchronized);
        CHECK(plan.edge_delays.empty());
        CHECK(plan.buffer_bytes == 0);
    }

    // 3. 混合政策:full-PDC 只規劃其祖先；不碰 low-latency-only 匯流。
    {
        const std::vector<rmx::PdcNodeSpec> nodes{
            {1, 0, 0, {3}},
            {2, 200, 20, {3}},
            {3, 0, 0, {}},       // low-latency output
            {10, 0, 0, {12}},
            {11, 100, 10, {12}},
            {12, 0, 0, {}},      // full-PDC output
        };
        const std::vector<rmx::PdcOutputSpec> outputs{
            {3, rmx::OutputLatencyPolicy::kLowLatency},
            {12, rmx::OutputLatencyPolicy::kFullPdc},
        };
        const auto plan = rmx::plan_plugin_delay(nodes, outputs, limits);
        CHECK(plan.ok());
        CHECK(plan.edge_delays.size() == 2);
        CHECK(plan.edge_delays[0].from_track_id == 10);
        CHECK(plan.edge_delays[0].to_track_id == 12);
        CHECK(plan.edge_delays[0].delay_samples == 100);
        CHECK(plan.edge_delays[1].delay_samples == 0);
        CHECK(plan.buffer_bytes == 2u * (96000u + 1u) * 2u * sizeof(float));
    }

    // 4. 重複 track ID 不得靜默覆寫 graph lookup。
    {
        const std::vector<rmx::PdcNodeSpec> nodes{{1, 0, 0, {}}, {1, 10, 10, {}}};
        const auto plan = rmx::plan_plugin_delay(
            nodes, {{1, rmx::OutputLatencyPolicy::kFullPdc}}, limits);
        CHECK(!plan.ok());
        CHECK(plan.error == rmx::PdcPlanError::kDuplicateTrackId);
    }

    // 5. 不完整或循環 graph 必須拒絕，不得產生 partial plan。
    {
        const auto unknown_dest = rmx::plan_plugin_delay(
            {{1, 0, 0, {99}}}, {{1, rmx::OutputLatencyPolicy::kFullPdc}}, limits);
        CHECK(unknown_dest.error == rmx::PdcPlanError::kUnknownDestination);

        const auto unknown_output = rmx::plan_plugin_delay(
            {{1, 0, 0, {}}}, {{99, rmx::OutputLatencyPolicy::kFullPdc}}, limits);
        CHECK(unknown_output.error == rmx::PdcPlanError::kUnknownOutput);

        const auto cycle = rmx::plan_plugin_delay(
            {{1, 0, 0, {2}}, {2, 0, 0, {1}}},
            {{2, rmx::OutputLatencyPolicy::kFullPdc}}, limits);
        CHECK(cycle.error == rmx::PdcPlanError::kCycle);
    }

    // 6. Path、buffer 與算術安全界線皆 fail closed。
    {
        const rmx::PdcLimits path_limited{100, 256u * 1024u * 1024u};
        const auto full_path = rmx::plan_plugin_delay(
            {{1, 101, 0, {}}}, {{1, rmx::OutputLatencyPolicy::kFullPdc}}, path_limited);
        CHECK(full_path.error == rmx::PdcPlanError::kPathLimitExceeded);
        const auto monitor_path = rmx::plan_plugin_delay(
            {{1, 0, 101, {}}}, {{1, rmx::OutputLatencyPolicy::kLowLatency}}, path_limited);
        CHECK(monitor_path.error == rmx::PdcPlanError::kPathLimitExceeded);

        const rmx::PdcLimits memory_limited{1000, 799};
        const auto memory = rmx::plan_plugin_delay(
            {{1, 0, 0, {3}}, {2, 100, 100, {3}}, {3, 0, 0, {}}},
            {{3, rmx::OutputLatencyPolicy::kFullPdc}}, memory_limited);
        CHECK(memory.error == rmx::PdcPlanError::kMemoryLimitExceeded);

        const auto overflow = rmx::plan_plugin_delay(
            {{1, std::numeric_limits<std::uint64_t>::max(), 0, {2}},
             {2, 1, 0, {}}},
            {{2, rmx::OutputLatencyPolicy::kFullPdc}},
            {std::numeric_limits<std::uint64_t>::max(),
             std::numeric_limits<std::size_t>::max()});
        CHECK(overflow.error == rmx::PdcPlanError::kArithmeticOverflow);
    }

    // 7. 規格矩陣：四種 rate／block 與跨 block latency 邊界。
    for (const std::uint64_t rate : {44100u, 48000u, 96000u, 192000u}) {
        for (const std::uint32_t block : {64u, 128u, 512u, 2048u}) {
            const std::uint64_t values[]{0u, 1u, block - 1u, block, block + 1u,
                                         static_cast<std::uint64_t>(block) * 3u,
                                         rate * 2u};
            for (const auto latency : values) {
                const auto matrix = rmx::plan_plugin_delay(
                    {{1, 0, 0, {3}}, {2, latency, latency, {3}}, {3, 0, 0, {}}},
                    {{3, rmx::OutputLatencyPolicy::kFullPdc}},
                    {rate * 2u, 256u * 1024u * 1024u, 2u, sizeof(float), block});
                CHECK(matrix.ok());
                CHECK(matrix.outputs.size() == 1);
                CHECK(matrix.outputs[0].total_plugin_delay_samples == latency);
                CHECK(matrix.outputs[0].compensation_delay_samples == latency);
            }
            const auto over_boundary = rmx::plan_plugin_delay(
                {{1, rate * 2u + 1u, 0, {}}},
                {{1, rmx::OutputLatencyPolicy::kFullPdc}},
                {rate * 2u, 256u * 1024u * 1024u, 2u, sizeof(float), block});
            CHECK(over_boundary.error == rmx::PdcPlanError::kPathLimitExceeded);
        }
    }

    std::printf("pdc_planner_test PASSED\n");
    return 0;
}
