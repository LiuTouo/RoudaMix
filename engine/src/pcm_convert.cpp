#include "pcm_convert.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace rmx {

void wasapi_mix_to_stereo(const std::byte* data, std::uint32_t frames,
                          std::uint32_t channels, int format, float* out) noexcept {
    if (format == 0) {  // f32
        const float* src = reinterpret_cast<const float*>(data);
        if (channels == 1) {
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i * 2] = out[i * 2 + 1] = src[i];
            }
        } else {
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i * 2] = src[i * channels];
                out[i * 2 + 1] = src[i * channels + 1];
            }
        }
    } else {  // s16
        const std::int16_t* src = reinterpret_cast<const std::int16_t*>(data);
        const float k = 1.0F / 32768.0F;
        if (channels == 1) {
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i * 2] = out[i * 2 + 1] = src[i] * k;
            }
        } else {
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i * 2] = src[i * channels] * k;
                out[i * 2 + 1] = src[i * channels + 1] * k;
            }
        }
    }
}


// asio.h ASIOSampleType 常數(SDK asio.h L139-168 的數值,避免此檔依賴 SDK header)
namespace {
constexpr long kAsioStInt16Msb = 0;
constexpr long kAsioStInt24Msb = 1;
constexpr long kAsioStInt32Msb = 2;
constexpr long kAsioStFloat32Msb = 3;
constexpr long kAsioStFloat64Msb = 4;
constexpr long kAsioStInt32Msb16 = 8;
constexpr long kAsioStInt32Msb18 = 9;
constexpr long kAsioStInt32Msb20 = 10;
constexpr long kAsioStInt32Msb24 = 11;
constexpr long kAsioStInt16Lsb = 16;
constexpr long kAsioStInt24Lsb = 17;
constexpr long kAsioStInt32Lsb = 18;
constexpr long kAsioStFloat32Lsb = 19;
constexpr long kAsioStFloat64Lsb = 20;
constexpr long kAsioStInt32Lsb16 = 24;
constexpr long kAsioStInt32Lsb18 = 25;
constexpr long kAsioStInt32Lsb20 = 26;
constexpr long kAsioStInt32Lsb24 = 27;
constexpr long kAsioStDsdInt8Lsb1 = 32;
constexpr long kAsioStDsdInt8Msb1 = 33;
constexpr long kAsioStDsdInt8Ner8 = 40;

struct IntegerFormat {
    std::uint8_t storage_bytes;
    std::uint8_t valid_bits;
    bool big_endian;
    bool left_aligned;
};

bool integer_format(PcmType type, IntegerFormat& format) noexcept {
    switch (type) {
    case PcmType::Int16Lsb:   format = {2, 16, false, false}; return true;
    case PcmType::Int16Msb:   format = {2, 16, true,  false}; return true;
    case PcmType::Int24Lsb:   format = {3, 24, false, false}; return true;
    case PcmType::Int24Msb:   format = {3, 24, true,  false}; return true;
    case PcmType::Int32Lsb:   format = {4, 32, false, false}; return true;
    case PcmType::Int32Msb:   format = {4, 32, true,  false}; return true;
    case PcmType::Int32Lsb16: format = {4, 16, false, true};  return true;
    case PcmType::Int32Lsb18: format = {4, 18, false, true};  return true;
    case PcmType::Int32Lsb20: format = {4, 20, false, true};  return true;
    case PcmType::Int32Lsb24: format = {4, 24, false, true};  return true;
    case PcmType::Int32Msb16: format = {4, 16, true,  true};  return true;
    case PcmType::Int32Msb18: format = {4, 18, true,  true};  return true;
    case PcmType::Int32Msb20: format = {4, 20, true,  true};  return true;
    case PcmType::Int32Msb24: format = {4, 24, true,  true};  return true;
    default: return false;
    }
}

std::uint64_t read_unsigned(const std::byte* source, std::size_t size, bool big_endian) noexcept {
    std::uint64_t value = 0;
    if (big_endian) {
        for (std::size_t i = 0; i < size; ++i)
            value = (value << 8U) | static_cast<std::uint8_t>(source[i]);
    } else {
        for (std::size_t i = 0; i < size; ++i)
            value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(source[i])) << (i * 8U);
    }
    return value;
}

void write_unsigned(std::byte* destination, std::size_t size, bool big_endian,
                    std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        const std::size_t shift = big_endian ? (size - i - 1U) * 8U : i * 8U;
        destination[i] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

std::int64_t sign_extend(std::uint64_t value, std::uint8_t bits) noexcept {
    if (bits == 64) return std::bit_cast<std::int64_t>(value);
    const std::uint64_t sign_bit = std::uint64_t{1} << (bits - 1U);
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1U;
    return static_cast<std::int64_t>((value & sign_bit) != 0 ? value | ~mask : value & mask);
}

}  // namespace

PcmType map_asio_sample_type(long t) noexcept {
    switch (t) {
    case kAsioStInt16Lsb:   return PcmType::Int16Lsb;
    case kAsioStInt16Msb:   return PcmType::Int16Msb;
    case kAsioStInt24Lsb:   return PcmType::Int24Lsb;
    case kAsioStInt24Msb:   return PcmType::Int24Msb;
    case kAsioStInt32Lsb:   return PcmType::Int32Lsb;
    case kAsioStInt32Msb:   return PcmType::Int32Msb;
    case kAsioStFloat32Lsb: return PcmType::Float32Lsb;
    case kAsioStFloat32Msb: return PcmType::Float32Msb;
    case kAsioStFloat64Lsb: return PcmType::Float64Lsb;
    case kAsioStFloat64Msb: return PcmType::Float64Msb;
    case kAsioStInt32Lsb16: return PcmType::Int32Lsb16;
    case kAsioStInt32Lsb18: return PcmType::Int32Lsb18;
    case kAsioStInt32Lsb20: return PcmType::Int32Lsb20;
    case kAsioStInt32Lsb24: return PcmType::Int32Lsb24;
    case kAsioStInt32Msb16: return PcmType::Int32Msb16;
    case kAsioStInt32Msb18: return PcmType::Int32Msb18;
    case kAsioStInt32Msb20: return PcmType::Int32Msb20;
    case kAsioStInt32Msb24: return PcmType::Int32Msb24;
    case kAsioStDsdInt8Lsb1:
    case kAsioStDsdInt8Msb1:
    case kAsioStDsdInt8Ner8: return PcmType::UnsupportedDsd;
    default: return PcmType::Unknown;
    }
}

std::size_t pcm_bytes_per_sample(PcmType type) noexcept {
    IntegerFormat format;
    if (integer_format(type, format)) return format.storage_bytes;
    switch (type) {
    case PcmType::Float32Lsb:
    case PcmType::Float32Msb: return 4;
    case PcmType::Float64Lsb:
    case PcmType::Float64Msb: return 8;
    default: return 0;
    }
}

bool pcm_to_float32(PcmType type, std::span<const std::byte> source,
                    std::span<float> destination) noexcept {
    const std::size_t bytes = pcm_bytes_per_sample(type);
    if (bytes == 0 || source.size() != destination.size() * bytes) return false;
    IntegerFormat integer;
    if (integer_format(type, integer)) {
        const double scale = std::ldexp(1.0, integer.valid_bits - 1U);
        for (std::size_t i = 0; i < destination.size(); ++i) {
            auto raw = read_unsigned(source.data() + i * bytes, bytes, integer.big_endian);
            auto value = sign_extend(raw, static_cast<std::uint8_t>(bytes * 8U));
            if (integer.left_aligned)
                value >>= static_cast<std::uint8_t>(32U - integer.valid_bits);
            destination[i] = static_cast<float>(static_cast<double>(value) / scale);
        }
        return true;
    }
    for (std::size_t i = 0; i < destination.size(); ++i) {
        const bool big = type == PcmType::Float32Msb || type == PcmType::Float64Msb;
        const auto bits = read_unsigned(source.data() + i * bytes, bytes, big);
        if (bytes == 4)
            destination[i] = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
        else
            destination[i] = static_cast<float>(std::bit_cast<double>(bits));
    }
    return true;
}

bool float32_to_pcm(PcmType type, std::span<const float> source,
                    std::span<std::byte> destination) noexcept {
    const std::size_t bytes = pcm_bytes_per_sample(type);
    if (bytes == 0 || destination.size() != source.size() * bytes) return false;
    IntegerFormat integer;
    if (integer_format(type, integer)) {
        const auto scale = std::uint64_t{1} << (integer.valid_bits - 1U);
        const auto minimum = -static_cast<std::int64_t>(scale);
        const auto maximum = static_cast<std::int64_t>(scale - 1U);
        for (std::size_t i = 0; i < source.size(); ++i) {
            const float finite = std::isfinite(source[i]) ? source[i] : 0.0F;
            auto value = static_cast<std::int64_t>(
                std::llround(static_cast<double>(finite) * static_cast<double>(scale)));
            value = std::clamp(value, minimum, maximum);
            if (integer.left_aligned)
                value <<= static_cast<std::uint8_t>(32U - integer.valid_bits);
            write_unsigned(destination.data() + i * bytes, bytes, integer.big_endian,
                           static_cast<std::uint64_t>(value));
        }
        return true;
    }
    for (std::size_t i = 0; i < source.size(); ++i) {
        const bool big = type == PcmType::Float32Msb || type == PcmType::Float64Msb;
        if (bytes == 4)
            write_unsigned(destination.data() + i * bytes, bytes, big,
                           std::bit_cast<std::uint32_t>(source[i]));
        else
            write_unsigned(destination.data() + i * bytes, bytes, big,
                           std::bit_cast<std::uint64_t>(static_cast<double>(source[i])));
    }
    return true;
}

}  // namespace rmx
