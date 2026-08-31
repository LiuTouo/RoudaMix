#include "telemetry.hpp"

#include <intrin.h>  // __rdtsc(publish thread 量 load 用;RT 端在 audio_engine)

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

void MeterAccumulator::append_spectrum(const float* l, const float* r,
                                       std::uint32_t frames) noexcept {
    std::size_t w = ring_write_.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < frames; ++i) {
        spectrum_ring_[w] = 0.5F * ((l != nullptr ? l[i] : 0.0F) + (r != nullptr ? r[i] : 0.0F));
        w = (w + 1) % kSpectrumRingSize;
    }
    ring_write_.store(static_cast<std::uint32_t>(w), std::memory_order_release);
}

// publish thread 專屬(單一 consumer):取 ring 末 2048 點,Hann + radix-2 FFT,
// 256 線性 bins(每 bin = 4 個 FFT bins 跨度,涵蓋到 Nyquist),dB 滿幅 sine ≈ 0。
void MeterAccumulator::compute_spectrum(float* out_db) noexcept {
    constexpr std::size_t N = kSpectrumFft;
    static float window[N];
    static bool window_init = false;
    if (!window_init) {
        for (std::size_t n = 0; n < N; ++n)
            window[n] = 0.5F * (1.0F - std::cos(2.0F * 3.14159265358979323846F *
                                                static_cast<float>(n) / static_cast<float>(N - 1)));
        window_init = true;
    }

    const std::uint32_t w = ring_write_.load(std::memory_order_acquire);
    // 16KB stack(publish thread,非 RT):float re/im 各 8KB
    alignas(16) float re[N];
    alignas(16) float im[N];
    for (std::size_t n = 0; n < N; ++n) {
        const std::size_t idx =
            (static_cast<std::size_t>(w) + kSpectrumRingSize - N + n) % kSpectrumRingSize;
        re[n] = spectrum_ring_[idx] * window[n];
        im[n] = 0.0F;
    }

    for (std::size_t i = 1, j = 0; i < N; ++i) {  // bit-reversal 重排
        std::size_t bit = N >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            // ponytail:std::swap 在此 TU 觸發 LNK2019(工具鏈怪癖),手寫交換繞過
            const float tr = re[i]; re[i] = re[j]; re[j] = tr;
            const float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (std::size_t len = 2; len <= N; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (std::size_t i = 0; i < N; i += len) {
            double cr = 1.0, ci = 0.0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = static_cast<float>(ur + vr);
                im[i + k] = static_cast<float>(ui + vi);
                re[i + k + len / 2] = static_cast<float>(ur - vr);
                im[i + k + len / 2] = static_cast<float>(ui - vi);
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }

    // Hann 相干增益 0.5:滿幅 sine 的 bin 島 ≈ A*N/2*0.5,除以 N/4 補償 → ≈ 0 dB。
    // 每 display bin = FFT bins [j*4+1, j*4+4] 取 max(tone 落 span 內任一點都看得到,
    // 點採樣會因頻率對不齊 sampled bin 而顯示過低)
    for (std::size_t j = 0; j < kTelemetrySpectrumBins; ++j) {
        float mag = 0.0F;
        for (std::size_t k = j * 4 + 1; k <= j * 4 + 4; ++k) {
            const float m = std::sqrt(re[k] * re[k] + im[k] * im[k]);
            if (m > mag) mag = m;
        }
        mag /= static_cast<float>(N / 4);
        float db = 20.0F * std::log10(mag > 1e-6F ? mag : 1e-6F);
        if (db < -120.0F) db = -120.0F;
        if (db > 0.0F) db = 0.0F;
        out_db[j] = db;
    }
}

void MeterAccumulator::publish(TelemetryBlockShm& block, std::uint64_t xruns_total,
                                const std::uint32_t* instance_ids, const std::uint8_t* kinds,
                                std::size_t id_count, bool running) noexcept {
    TelemetryBlockShm next{};
    next.magic = kTelemetryMagic;
    next.abi_version = kTelemetryAbiVersion;
    next.xruns = xruns_total;
    next.sample_rate = bits_f32(sample_rate_.load(std::memory_order_relaxed));
    next.buffer_size = buffer_size_.load(std::memory_order_relaxed);
    next.input_latency = in_lat_.load(std::memory_order_relaxed);
    next.output_latency = out_lat_.load(std::memory_order_relaxed);
    // callback load(P1-J):RT 端 __rdtsc 差累積(RT 開銷 = 一次 relaxed add),
    // 這裡以兩次 publish 之間的 TSC 差為分母 = audio thread 平均 CPU 佔比。
    // invariant TSC(現代 Windows x86 保證);比值不需絕對頻率校準。
    {
        const std::uint64_t tsc_now = __rdtsc();
        const std::uint64_t busy_now = busy_cycles_.load(std::memory_order_relaxed);
        float load = 0.0F;
        if (last_tsc_ != 0 && tsc_now > last_tsc_) {
            load = static_cast<float>(
                static_cast<double>(busy_now - last_busy_snapshot_) /
                static_cast<double>(tsc_now - last_tsc_));
            if (load < 0.0F) load = 0.0F;
            if (load > 2.0F) load = 2.0F;  // 量測雜訊夾限
        }
        next.callback_load = load;
        last_tsc_ = tsc_now;
        last_busy_snapshot_ = busy_now;
    }
    next.strip_count = static_cast<std::uint32_t>(id_count < kTelemetryStrips
                                                      ? id_count
                                                      : kTelemetryStrips);
    for (std::size_t s = 0; s < kTelemetryStrips; ++s) {
        next.strips[s].instance_id = s < id_count ? instance_ids[s] : 0u;
        next.strips[s].kind = s < id_count ? kinds[s] : 0u;
    }

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

    if (running) {
        next.spectrum_count = kTelemetrySpectrumBins;
        compute_spectrum(next.spectrum_db);
    } else {
        next.spectrum_count = 0;
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
    std::memcpy(&block.spectrum_count, &next.spectrum_count,
                sizeof(next.spectrum_count) + sizeof(next.spectrum_db));
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
