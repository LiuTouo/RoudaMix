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
          rmx::RouteSlotAction::kProcess);

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

    std::printf("pdc_audio_path_test PASSED\n");
    return 0;
}
