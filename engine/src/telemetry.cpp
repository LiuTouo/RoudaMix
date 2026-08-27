#include "telemetry.hpp"

#include <cmath>
#include <cstring>

namespace rmx {

namespace {
// f32 位元模式存 atomic u32(avoid ABA on float):直接 bit_cast,decay 由讀端處理
inline std::uint32_t f32_bits(float v) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}
inline float bits_f32(std::uint32_t b) noexcept {
    float v;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}
}  // namespace

void MeterAccumulator::atomic_max(std::atomic<std::uint32_t>& t, float v) noexcept {
    std::uint32_t cur = t.load(std::memory_order_relaxed);
    std::uint32_t next = f32_bits(v);
    while (next > cur && !t.compare_exchange_weak(cur, next, std::memory_order_relaxed)) {
    }
}

void MeterAccumulator::atomic_add(std::atomic<std::uint32_t>& t, float v) noexcept {
    // ponytail:relaxed 加總,float 精度用 double 暫存不在乎 — 純位元加會爛,改存量化值
    t.fetch_add(static_cast<std::uint32_t>(v * 65536.0F + 0.5F), std::memory_order_relaxed);
}

void MeterAccumulator::accumulate(std::size_t strip, float peak_l, float peak_r,
                                  float squared_sum_l, float squared_sum_r,
                                  std::uint32_t samples) noexcept {
    if (strip >= kTelemetryStrips) return;
    atomic_max(peak_l_bits_[strip], peak_l);
    atomic_max(peak_r_bits_[strip], peak_r);
    atomic_add(sq_l_bits_[strip], squared_sum_l);
    atomic_add(sq_r_bits_[strip], squared_sum_r);
    sample_counts_[strip].fetch_add(samples, std::memory_order_relaxed);
}

void MeterAccumulator::set_runtime(float sample_rate, std::uint32_t buffer_size,
                                   std::uint32_t in_lat, std::uint32_t out_lat) noexcept {
    sample_rate_.store(f32_bits(sample_rate), std::memory_order_relaxed);
    buffer_size_.store(buffer_size, std::memory_order_relaxed);
    in_lat_.store(in_lat, std::memory_order_relaxed);
    out_lat_.store(out_lat, std::memory_order_relaxed);
}

void MeterAccumulator::publish(TelemetryBlockShm& block, std::uint64_t xruns_total,
                                const std::uint32_t* instance_ids,
                                std::size_t id_count) noexcept {
    TelemetryBlockShm next{};
    next.magic = kTelemetryMagic;
    next.abi_version = kTelemetryAbiVersion;
    next.xruns = xruns_total;
    next.sample_rate = bits_f32(sample_rate_.load(std::memory_order_relaxed));
    next.buffer_size = buffer_size_.load(std::memory_order_relaxed);
    next.input_latency = in_lat_.load(std::memory_order_relaxed);
    next.output_latency = out_lat_.load(std::memory_order_relaxed);
    next.callback_load = 0.0F;  // M1 之後量測
    next.strip_count = static_cast<std::uint32_t>(id_count < kTelemetryStrips
                                                      ? id_count
                                                      : kTelemetryStrips);
    for (std::size_t s = 0; s < kTelemetryStrips; ++s)
        next.strips[s].instance_id = s < id_count ? instance_ids[s] : 0u;

    for (std::size_t s = 0; s < kTelemetryStrips; ++s) {
        const std::uint32_t n = sample_counts_[s].exchange(0, std::memory_order_relaxed);
        if (n == 0) continue;
        const float peak_l = bits_f32(peak_l_bits_[s].exchange(0, std::memory_order_relaxed));
        const float peak_r = bits_f32(peak_r_bits_[s].exchange(0, std::memory_order_relaxed));
        const float sq_l = static_cast<float>(sq_l_bits_[s].exchange(0, std::memory_order_relaxed)) /
                           65536.0F;
        const float sq_r = static_cast<float>(sq_r_bits_[s].exchange(0, std::memory_order_relaxed)) /
                           65536.0F;
        next.strips[s].peak_l = peak_l;
        next.strips[s].peak_r = peak_r;
        next.strips[s].rms_l = std::sqrt(sq_l / static_cast<float>(n));
        next.strips[s].rms_r = std::sqrt(sq_r / static_cast<float>(n));
    }

    // seqlock write:odd → payload → even(release)
    // 注意:sequence 欄位不得被 header memcpy 蓋掉(next 未設 = 0 會把序號歸零)
    const std::uint32_t seq = block.sequence;
    next.sequence = seq + 1;  // odd
    block.sequence = next.sequence;
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&block.magic, &next.magic,
                offsetof(TelemetryBlockShm, strips));
    std::memcpy(block.strips, next.strips, sizeof(block.strips));
    std::atomic_thread_fence(std::memory_order_release);
    block.sequence = seq + 2;  // even
}

HANDLE telemetry_create(TelemetryBlockShm** out_block) noexcept {
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(kTelemetryBytes),
                                        L"Local\\roudamix-telemetry");
    if (mapping == nullptr) return nullptr;
    auto* block = static_cast<TelemetryBlockShm*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kTelemetryBytes));
    if (block == nullptr) {
        CloseHandle(mapping);
        return nullptr;
    }
    std::memset(block, 0, kTelemetryBytes);
    block->magic = kTelemetryMagic;
    block->abi_version = kTelemetryAbiVersion;
    block->sequence = 0;
    block->strip_count = 0;
    *out_block = block;
    return mapping;  // mapping handle 保留(SHM 存活);view 不 unmap(engine 常駐)
}

}  // namespace rmx
