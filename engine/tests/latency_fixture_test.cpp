#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "vst3_host.hpp"

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                         \
        }                                                                         \
    } while (0)

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::filesystem::path module = argv[1];
    std::string error;
    const auto classes = rmx::scan_vst3_module(module, error);
    CHECK(classes.size() == 1);
    rmx::Vst3Plugin plugin(module, classes[0].uid);
    CHECK(plugin.loaded());
    CHECK(plugin.initialize(48000.0, 64));

    std::atomic<bool> latency_changed{};
    plugin.set_latency_changed_callback([&] { latency_changed.store(true); });
    constexpr std::uint32_t latency_id = 100;
    constexpr std::uint32_t cpu_id = 101;
    constexpr std::uint32_t fail_id = 102;
    const rmx::Vst3ParamEdit latency_edit{latency_id, 128.0 / 192000.0};
    float in_l[64]{}, in_r[64]{}, out_l[64]{}, out_r[64]{};
    in_l[0] = in_r[0] = 1.0F;
    CHECK(plugin.process(in_l, in_r, out_l, out_r, 64, &latency_edit, 1));
    CHECK(latency_changed.load());
    CHECK(plugin.latency_samples() == 128);
    for (float sample : out_l) CHECK(sample == 0.0F);
    bool impulse_seen = false;
    for (int block = 1; block < 3; ++block) {
        std::fill_n(in_l, 64, 0.0F);
        std::fill_n(in_r, 64, 0.0F);
        CHECK(plugin.process(in_l, in_r, out_l, out_r, 64, nullptr, 0));
        if (block == 2) impulse_seen = out_l[0] == 1.0F && out_r[0] == 1.0F;
    }
    CHECK(impulse_seen);

    const rmx::Vst3ParamEdit cpu_edit{cpu_id, 0.25};
    CHECK(plugin.process(in_l, in_r, out_l, out_r, 64, &cpu_edit, 1));
    for (float sample : out_l) CHECK(std::isfinite(sample));

    rmx::Vst3RuntimeState state;
    CHECK(plugin.capture_runtime_state(state, error));
    const rmx::Vst3ParamEdit changed_latency{latency_id, 64.0 / 192000.0};
    CHECK(plugin.process(in_l, in_r, out_l, out_r, 64, &changed_latency, 1));
    CHECK(plugin.latency_samples() == 64);
    CHECK(plugin.restore_runtime_state(state, error));
    CHECK(plugin.latency_samples() == 128);

    const rmx::Vst3ParamEdit fail_edit{fail_id, 1.0};
    CHECK(!plugin.process(in_l, in_r, out_l, out_r, 64, &fail_edit, 1));
    std::printf("latency_fixture_test PASSED\n");
    return 0;
}
