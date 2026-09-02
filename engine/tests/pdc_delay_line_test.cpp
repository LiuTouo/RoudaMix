#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

#include "pdc_delay_line.hpp"

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

int main() {
    rmx::PdcDelayLine delay;
    CHECK(delay.prepare(16, 8));
    CHECK(delay.set_delay(3, 0));

    const float in_l[4]{1.0F, 0.0F, 0.0F, 0.0F};
    const float in_r[4]{-1.0F, 0.0F, 0.0F, 0.0F};
    float out_l[4]{};
    float out_r[4]{};
    delay.process_add(in_l, in_r, out_l, out_r, 4);
    CHECK(out_l[0] == 0.0F && out_l[1] == 0.0F && out_l[2] == 0.0F);
    CHECK(out_l[3] == 1.0F);
    CHECK(out_r[3] == -1.0F);

    const float silence[4]{};
    float next_l[4]{};
    float next_r[4]{};
    delay.process_add(silence, silence, next_l, next_r, 4);
    for (float sample : next_l) CHECK(sample == 0.0F);

    // Runtime delay request 只在下一個 block boundary 生效，並在指定的 4
    // samples 內由舊 read 線性 crossfade 到新 read；transition 後恢復 exact。
    rmx::PdcDelayLine changing;
    CHECK(changing.prepare(8, 8));
    CHECK(changing.set_delay(2, 0));
    float history_l[8]{1, 2, 3, 4, 5, 6, 7, 8};
    float history_r[8]{-1, -2, -3, -4, -5, -6, -7, -8};
    float history_out_l[8]{};
    float history_out_r[8]{};
    changing.process_add(history_l, history_r, history_out_l, history_out_r, 8);
    CHECK(changing.set_delay(4, 4));
    float transition_in_l[4]{9, 10, 11, 12};
    float transition_in_r[4]{-9, -10, -11, -12};
    float transition_out_l[4]{};
    float transition_out_r[4]{};
    changing.process_add(transition_in_l, transition_in_r, transition_out_l,
                         transition_out_r, 4);
    const float expected[4]{7.0F, 7.5F, 8.0F, 8.5F};
    for (int i = 0; i < 4; ++i) {
        CHECK(std::isfinite(transition_out_l[i]));
        CHECK(transition_out_l[i] == expected[i]);
        CHECK(transition_out_r[i] == -expected[i]);
    }
    float settled_in_l[4]{13, 14, 15, 16};
    float settled_in_r[4]{-13, -14, -15, -16};
    float settled_out_l[4]{};
    float settled_out_r[4]{};
    changing.process_add(settled_in_l, settled_in_r, settled_out_l, settled_out_r, 4);
    CHECK(settled_out_l[0] == 9.0F && settled_out_l[3] == 12.0F);
    CHECK(settled_out_r[0] == -9.0F && settled_out_r[3] == -12.0F);

    // 規格矩陣：不同 rate/block 下的 0、block 邊界、multi-block 與 2 秒。
    for (const std::uint64_t rate : {44100u, 48000u, 96000u, 192000u}) {
        for (const std::uint32_t block : {64u, 128u, 512u, 2048u}) {
            const std::uint64_t values[]{0u, 1u, block - 1u, block, block + 1u,
                                         static_cast<std::uint64_t>(block) * 3u,
                                         rate * 2u};
            for (const auto latency : values) {
                rmx::PdcDelayLine matrix;
                CHECK(matrix.prepare(rate * 2u, block));
                CHECK(matrix.set_delay(latency, 0));
                std::vector<float> matrix_in_l(block, 0.0F), matrix_in_r(block, 0.0F);
                std::vector<float> got_l(block, 0.0F), got_r(block, 0.0F);
                std::uint64_t cursor = 0;
                bool impulse_seen = false;
                while (cursor <= latency) {
                    std::fill(matrix_in_l.begin(), matrix_in_l.end(), 0.0F);
                    std::fill(matrix_in_r.begin(), matrix_in_r.end(), 0.0F);
                    std::fill(got_l.begin(), got_l.end(), 0.0F);
                    std::fill(got_r.begin(), got_r.end(), 0.0F);
                    if (cursor == 0) matrix_in_l[0] = matrix_in_r[0] = 1.0F;
                    const auto frames = static_cast<std::uint32_t>(
                        (std::min)(static_cast<std::uint64_t>(block), latency + 1u - cursor));
                    matrix.process_add(matrix_in_l.data(), matrix_in_r.data(), got_l.data(),
                                       got_r.data(), frames);
                    for (std::uint32_t i = 0; i < frames; ++i) {
                        CHECK(std::isfinite(got_l[i]) && std::isfinite(got_r[i]));
                        const bool expected_impulse = cursor + i == latency;
                        CHECK(got_l[i] == (expected_impulse ? 1.0F : 0.0F));
                        CHECK(got_r[i] == (expected_impulse ? 1.0F : 0.0F));
                        impulse_seen = impulse_seen || expected_impulse;
                    }
                    cursor += frames;
                }
                CHECK(impulse_seen);
            }
        }
    }

    std::printf("pdc_delay_line_test PASSED\n");
    return 0;
}
