#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "pdc_delay_line.hpp"
#include "route_planner.hpp"

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                         \
        }                                                                         \
    } while (0)

int main() {
    constexpr std::uint32_t block = 64;
    constexpr std::uint64_t slow_latency = 128;
    rmx::RouteSlotSpec slow_slot;
    slow_slot.instance_id = 1;
    slow_slot.primary_available = true;
    slow_slot.primary_latency_known = true;
    slow_slot.primary_latency_samples = slow_latency;
    slow_slot.shadow_available = true;
    slow_slot.shadow_latency_known = true;
    slow_slot.shadow_latency_samples = slow_latency;
    const auto route_plan = rmx::plan_routes(
        {{1, false, false, rmx::OutputLatencyPolicy::kFullPdc, {}, {3, 4}},
         {2, false, false, rmx::OutputLatencyPolicy::kFullPdc,
          {slow_slot}, {3, 4}},
         {3, false, true, rmx::OutputLatencyPolicy::kFullPdc, {}, {}},
         {4, false, true, rmx::OutputLatencyPolicy::kLowLatency, {}, {}}},
        {96000, 256u * 1024u * 1024u, 2u, sizeof(float), block});
    CHECK(route_plan.ok());
    CHECK(route_plan.latency.edge_delays.size() == 2);
    CHECK(route_plan.tracks[0].sends[0].primary_delay_samples == slow_latency);
    CHECK(route_plan.tracks[0].sends[1].primary_delay_samples == 0);
    CHECK(route_plan.tracks[1].slots[0].monitor.action ==
          rmx::RouteSlotAction::kReusePrimary);

    std::vector<std::unique_ptr<rmx::PdcDelayLine>> lines;
    for (const auto& edge : route_plan.latency.edge_delays) {
        auto line = std::make_unique<rmx::PdcDelayLine>();
        CHECK(line->prepare(96000, block));
        CHECK(line->set_delay(edge.delay_samples, 0));
        lines.push_back(std::move(line));
    }

    std::vector<float> full_output(256, 0.0F);
    std::vector<float> low_output(256, 0.0F);
    for (std::uint32_t base = 0; base < full_output.size(); base += block) {
        float fast_l[block]{}, fast_r[block]{}, slow_l[block]{}, slow_r[block]{};
        float sum_l[block]{}, sum_r[block]{};
        if (base == 0) fast_l[0] = fast_r[0] = 1.0F;
        if (base <= slow_latency && slow_latency < base + block)
            slow_l[slow_latency - base] = slow_r[slow_latency - base] = 1.0F;
        for (std::size_t i = 0; i < route_plan.latency.edge_delays.size(); ++i) {
            const auto from = route_plan.latency.edge_delays[i].from_track_id;
            lines[i]->process_add(from == 1 ? fast_l : slow_l,
                                  from == 1 ? fast_r : slow_r, sum_l, sum_r, block);
        }
        for (std::uint32_t i = 0; i < block; ++i) {
            full_output[base + i] = sum_l[i];
            low_output[base + i] = fast_l[i] + slow_l[i];
            CHECK(std::isfinite(sum_l[i]) && std::isfinite(sum_r[i]));
        }
    }
    for (std::size_t i = 0; i < full_output.size(); ++i)
        CHECK(full_output[i] == (i == slow_latency ? 2.0F : 0.0F));
    CHECK(low_output[0] == 1.0F);
    CHECK(low_output[slow_latency] == 1.0F);

    // Shadow 準備用 safety plan 不呼叫 plugin，但以已預熱的 per-plugin dry
    // delay 保留原 Plugin Latency；與既有 PDC edge 組合後 fan-in 仍 sample-exact。
    const auto safety_plan = rmx::plan_route_suspension(route_plan);
    const auto& safety_slot = safety_plan.tracks[1].slots[0].primary;
    CHECK(safety_slot.action == rmx::RouteSlotAction::kDelayDry);
    CHECK(safety_slot.dry_delay_samples == slow_latency);
    rmx::PdcDelayLine safety_dry;
    CHECK(safety_dry.prepare(96000, block));
    CHECK(safety_dry.set_delay(safety_slot.dry_delay_samples, 0));
    std::vector<std::unique_ptr<rmx::PdcDelayLine>> safety_edges;
    for (const auto& edge : safety_plan.latency.edge_delays) {
        auto line = std::make_unique<rmx::PdcDelayLine>();
        CHECK(line->prepare(96000, block));
        CHECK(line->set_delay(edge.delay_samples, 0));
        safety_edges.push_back(std::move(line));
    }
    std::vector<float> safety_output(256, 0.0F);
    for (std::uint32_t base = 0; base < safety_output.size(); base += block) {
        float fast_l[block]{}, fast_r[block]{}, slow_l[block]{}, slow_r[block]{};
        float delayed_slow_l[block]{}, delayed_slow_r[block]{};
        float sum_l[block]{}, sum_r[block]{};
        if (base == 0) {
            fast_l[0] = fast_r[0] = 1.0F;
            slow_l[0] = slow_r[0] = 1.0F;
        }
        safety_dry.process_add(slow_l, slow_r, delayed_slow_l, delayed_slow_r, block);
        for (std::size_t i = 0; i < safety_plan.latency.edge_delays.size(); ++i) {
            const auto from = safety_plan.latency.edge_delays[i].from_track_id;
            safety_edges[i]->process_add(from == 1 ? fast_l : delayed_slow_l,
                                         from == 1 ? fast_r : delayed_slow_r,
                                         sum_l, sum_r, block);
        }
        for (std::uint32_t i = 0; i < block; ++i) {
            CHECK(std::isfinite(sum_l[i]) && std::isfinite(sum_r[i]));
            safety_output[base + i] = sum_l[i];
        }
    }
    for (std::size_t i = 0; i < safety_output.size(); ++i)
        CHECK(safety_output[i] == (i == slow_latency ? 2.0F : 0.0F));

    // RoutePlan 驅動的 20ms delay transition：穩態訊號不得出現 NaN/靜音
    // gap；transition 完成並清空 history 後，impulse 恢復 sample-exact。
    auto changed_slot = slow_slot;
    changed_slot.primary_latency_samples = 64;
    changed_slot.shadow_latency_samples = 64;
    const auto changed_plan = rmx::plan_routes(
        {{1, false, false, rmx::OutputLatencyPolicy::kFullPdc, {}, {3, 4}},
         {2, false, false, rmx::OutputLatencyPolicy::kFullPdc,
          {changed_slot}, {3, 4}},
         {3, false, true, rmx::OutputLatencyPolicy::kFullPdc, {}, {}},
         {4, false, true, rmx::OutputLatencyPolicy::kLowLatency, {}, {}}},
        {96000, 256u * 1024u * 1024u, 2u, sizeof(float), block});
    CHECK(changed_plan.ok());
    const auto old_delay = route_plan.tracks[0].sends[0].primary_delay_samples;
    const auto new_delay = changed_plan.tracks[0].sends[0].primary_delay_samples;
    CHECK(old_delay == 128 && new_delay == 64);

    rmx::PdcDelayLine transition;
    CHECK(transition.prepare(96000, block));
    CHECK(transition.set_delay(old_delay, 0));
    float ones[block], zeros[block]{};
    std::fill_n(ones, block, 1.0F);
    for (int warmup = 0; warmup < 4; ++warmup) {
        float out_l[block]{}, out_r[block]{};
        transition.process_add(ones, ones, out_l, out_r, block);
    }
    constexpr std::uint32_t transition_samples = 48000 / 50;
    CHECK(transition.set_delay(new_delay, transition_samples));
    for (std::uint32_t base = 0; base < transition_samples; base += block) {
        float out_l[block]{}, out_r[block]{};
        transition.process_add(ones, ones, out_l, out_r, block);
        for (std::uint32_t i = 0; i < block; ++i) {
            CHECK(std::isfinite(out_l[i]) && std::isfinite(out_r[i]));
            CHECK(out_l[i] == 1.0F && out_r[i] == 1.0F);
        }
    }
    for (int flush = 0; flush < 3; ++flush) {
        float out_l[block]{}, out_r[block]{};
        transition.process_add(zeros, zeros, out_l, out_r, block);
    }
    float impulse_l[block]{}, impulse_r[block]{};
    impulse_l[0] = impulse_r[0] = 1.0F;
    float first_l[block]{}, first_r[block]{};
    float second_l[block]{}, second_r[block]{};
    transition.process_add(impulse_l, impulse_r, first_l, first_r, block);
    transition.process_add(zeros, zeros, second_l, second_r, block);
    for (std::uint32_t i = 0; i < block; ++i) {
        CHECK(first_l[i] == 0.0F && first_r[i] == 0.0F);
        CHECK(second_l[i] == (i == 0 ? 1.0F : 0.0F));
        CHECK(second_r[i] == (i == 0 ? 1.0F : 0.0F));
    }

    std::printf("pdc_audio_path_test PASSED\n");
    return 0;
}
