// 轉換表 round-trip + 邊界測試 — int16/24/32、LSB16/18/20/24、float32/64、大小端
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pcm_convert.hpp"

using rmx::PcmType;

static int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void roundtrip(PcmType type, const char* name) {
    const std::size_t n = 257;
    std::vector<float> src(n);
    for (std::size_t i = 0; i < n; ++i)
        src[i] = -0.9F + 1.8F * static_cast<float>(i) / static_cast<float>(n - 1);

    const std::size_t bytes = rmx::pcm_bytes_per_sample(type);
    check(bytes > 0, name);
    std::vector<std::byte> encoded(n * bytes);
    check(rmx::float32_to_pcm(type, src, encoded), name);

    std::vector<float> back(n);
    check(rmx::pcm_to_float32(type, encoded, back), name);

    // 量化誤差 ≤ 1 LSB
    const float tol = type == PcmType::Float32Lsb || type == PcmType::Float32Msb ||
                              type == PcmType::Float64Lsb || type == PcmType::Float64Msb
                          ? 1e-6F
                          : 2.0F / 32768.0F;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::abs(back[i] - src[i]) > tol) {
            std::printf("FAIL: %s sample %zu: %f -> %f\n", name, i, src[i], back[i]);
            ++g_failures;
            break;
        }
    }
}

void known_values() {
    // int16 LSB:+1.0 → 0x7FFF(clamp)、-1.0 → 0x8000
    float one = 1.0F, neg = -1.0F;
    std::byte b16[2];
    rmx::float32_to_pcm(PcmType::Int16Lsb, {&one, 1}, {b16, 2});
    check(static_cast<uint8_t>(b16[0]) == 0xFF && static_cast<uint8_t>(b16[1]) == 0x7F,
          "int16 +1.0 = 0x7FFF");
    rmx::float32_to_pcm(PcmType::Int16Lsb, {&neg, 1}, {b16, 2});
    check(static_cast<uint8_t>(b16[0]) == 0x00 && static_cast<uint8_t>(b16[1]) == 0x80,
          "int16 -1.0 = 0x8000");

    // Int32Lsb24(left-aligned):+1.0 → 0x7FFFFF00
    float v = 1.0F;
    std::byte b32[4];
    rmx::float32_to_pcm(PcmType::Int32Lsb24, {&v, 1}, {b32, 4});
    check(static_cast<uint8_t>(b32[0]) == 0x00 && static_cast<uint8_t>(b32[1]) == 0xFF &&
              static_cast<uint8_t>(b32[2]) == 0xFF && static_cast<uint8_t>(b32[3]) == 0x7F,
          "int32 lsb24 +1.0 left-aligned = 0x7FFFFF00");

    // 大端 int16:+1.0 → bytes 7F FF
    rmx::float32_to_pcm(PcmType::Int16Msb, {&one, 1}, {b16, 2});
    check(static_cast<uint8_t>(b16[0]) == 0x7F && static_cast<uint8_t>(b16[1]) == 0xFF,
          "int16 msb +1.0");

    // float32 bit-exact
    float f = -0.5F;
    std::byte bf[4];
    rmx::float32_to_pcm(PcmType::Float32Lsb, {&f, 1}, {bf, 4});
    float back = 0.0F;
    rmx::pcm_to_float32(PcmType::Float32Lsb, {bf, 4}, {&back, 1});
    check(std::memcmp(&f, &back, 4) == 0, "float32 bit-exact");

    // NaN/inf → 0(int 路徑)
    float nan_v = std::nanf("");
    std::byte bn[4];
    rmx::float32_to_pcm(PcmType::Int32Lsb, {&nan_v, 1}, {bn, 4});
    uint32_t raw = 0;
    std::memcpy(&raw, bn, 4);
    check(raw == 0, "nan → 0 (int32)");
}

int main() {
    roundtrip(PcmType::Int16Lsb, "Int16Lsb");
    roundtrip(PcmType::Int16Msb, "Int16Msb");
    roundtrip(PcmType::Int24Lsb, "Int24Lsb");
    roundtrip(PcmType::Int24Msb, "Int24Msb");
    roundtrip(PcmType::Int32Lsb, "Int32Lsb");
    roundtrip(PcmType::Int32Msb, "Int32Msb");
    roundtrip(PcmType::Int32Lsb16, "Int32Lsb16");
    roundtrip(PcmType::Int32Lsb18, "Int32Lsb18");
    roundtrip(PcmType::Int32Lsb20, "Int32Lsb20");
    roundtrip(PcmType::Int32Lsb24, "Int32Lsb24");
    roundtrip(PcmType::Int32Msb16, "Int32Msb16");
    roundtrip(PcmType::Int32Msb18, "Int32Msb18");
    roundtrip(PcmType::Int32Msb20, "Int32Msb20");
    roundtrip(PcmType::Int32Msb24, "Int32Msb24");
    roundtrip(PcmType::Float32Lsb, "Float32Lsb");
    roundtrip(PcmType::Float32Msb, "Float32Msb");
    roundtrip(PcmType::Float64Lsb, "Float64Lsb");
    roundtrip(PcmType::Float64Msb, "Float64Msb");
    known_values();

    if (g_failures != 0) {
        std::printf("%d failures\n", g_failures);
        return 1;
    }
    std::printf("audio_test: all passed\n");
    return 0;
}
