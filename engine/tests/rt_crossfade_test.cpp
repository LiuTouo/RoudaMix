#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "rt_crossfade.hpp"

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                         \
        }                                                                         \
    } while (0)

int main() {
    rmx::RtCrossfade fade;
    fade.request(4);
    const float old_l[4]{1, 1, 1, 1};
    const float old_r[4]{-1, -1, -1, -1};
    float next_l[4]{5, 5, 5, 5};
    float next_r[4]{-5, -5, -5, -5};
    fade.blend(old_l, old_r, next_l, next_r, 4);
    const float expected[4]{1, 2, 3, 4};
    for (int i = 0; i < 4; ++i) {
        CHECK(std::isfinite(next_l[i]));
        CHECK(next_l[i] == expected[i]);
        CHECK(next_r[i] == -expected[i]);
    }
    CHECK(!fade.active());

    float settled_l[2]{5, 5};
    float settled_r[2]{-5, -5};
    const float old2_l[2]{1, 1};
    const float old2_r[2]{-1, -1};
    fade.blend(old2_l, old2_r, settled_l, settled_r, 2);
    CHECK(settled_l[0] == 5 && settled_l[1] == 5);
    CHECK(settled_r[0] == -5 && settled_r[1] == -5);

    std::printf("rt_crossfade_test PASSED\n");
    return 0;
}
