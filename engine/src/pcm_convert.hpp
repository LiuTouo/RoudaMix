// ASIO sample format 轉換 — 移植自 ProMixArea device_capability.cpp(f32 planar 雙向)
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace rmx {

enum class PcmType : std::uint8_t {
    Int16Lsb, Int16Msb,
    Int24Lsb, Int24Msb,
    Int32Lsb, Int32Msb,
    Float32Lsb, Float32Msb,
    Float64Lsb, Float64Msb,
    Int32Lsb16, Int32Lsb18, Int32Lsb20, Int32Lsb24,
    Int32Msb16, Int32Msb18, Int32Msb20, Int32Msb24,
    UnsupportedDsd,
    Unknown
};

// ASIOSampleType → PcmType
PcmType map_asio_sample_type(long asio_type) noexcept;

// WASAPI 交錯 buffer 轉 stereo(交錯 L/R 對):取 ch0/ch1(>2ch 其餘忽略;
// 1ch 複製)。format 0 = f32、1 = s16。app_capture(process loopback,可能
// s16 mix)與 mic_capture(一般 capture)共用。
void wasapi_mix_to_stereo(const std::byte* data, std::uint32_t frames,
                          std::uint32_t channels, int format, float* out) noexcept;

std::size_t pcm_bytes_per_sample(PcmType type) noexcept;
bool pcm_to_float32(PcmType type, std::span<const std::byte> source,
                    std::span<float> destination) noexcept;
bool float32_to_pcm(PcmType type, std::span<const float> source,
                    std::span<std::byte> destination) noexcept;

}  // namespace rmx
