// Telemetry SHM(`Local\roudamix-telemetry`)— 契約:contracts/telemetry_abi.md(v3)
// RT 端 accumulate(僅 atomic)+ mono 頻譜 ring;30Hz publish thread 寫 seqlock block。
#pragma once

#include <atomic>
#include <cstdint>

#include <windows.h>

namespace rmx {

constexpr std::uint32_t kTelemetryMagic = 0x524D5854;  // "RMXT"
constexpr std::uint32_t kTelemetryAbiVersion = 3;
constexpr std::size_t kTelemetryStrips = 64;
constexpr std::size_t kTelemetrySpectrumBins = 256;
constexpr std::size_t kTelemetryBytes = 4096;

#pragma pack(push, 8)
struct TelemetryStripShm {
    std::uint32_t instance_id;
    std::uint32_t kind;  // v3(原 pad0):0 = plugin instanceId、1 = trackId、2 = engine 輸出
    float peak_l;
    float peak_r;
    float rms_l;
    float rms_r;
    std::uint32_t reserved[2];
};
static_assert(sizeof(TelemetryStripShm) == 32);

// v3:strips 16 → 64(4096 page 內,v2 block 只用 1592);v1/v2 欄位 offset/語意不變
struct TelemetryBlockShm {
    std::uint32_t magic;
    std::uint32_t abi_version;
    std::uint32_t sequence;  // seqlock:寫前 odd、寫完 even
    std::uint32_t strip_count;
    std::uint64_t xruns;
    float callback_load;
    float sample_rate;
    std::uint32_t buffer_size;
    std::uint32_t input_latency;
    std::uint32_t output_latency;
    std::uint32_t reserved0;
    TelemetryStripShm strips[kTelemetryStrips];
    std::uint32_t spectrum_count;  // v2:有效 bin 數(未啟動 = 0)
    float spectrum_db[kTelemetrySpectrumBins];  // 線性 0..Nyquist,dB,-120 floor
};
// 48 header + 64×32 strips + 4 + 1024 spectrum = 3124;pack(8) 補齊 → 3128
static_assert(sizeof(TelemetryBlockShm) == 3128);
static_assert(sizeof(TelemetryBlockShm) <= kTelemetryBytes);
#pragma pack(pop)

// RT 安全累積器(全 atomic;strip 0 = engine 輸出,其餘 rack slots)。
// 頻譜:RT append mono 到 ring(僅 atomic index),publish thread 取末 2048 點
// 做 Hann + radix-2 FFT → 256 線性 dB bins(FFT 不在 RT thread 跑)。
class MeterAccumulator final {
public:
    // Audio thread:samples 為本 block 樣本數,squared_sum 為振幅平方和
    void accumulate(std::size_t strip, float peak_l, float peak_r, float squared_sum_l,
                    float squared_sum_r, std::uint32_t samples) noexcept;
    // Audio thread:engine 最終輸出 mono((l+r)/2)進頻譜 ring
    void append_spectrum(const float* l, const float* r, std::uint32_t frames) noexcept;
    void set_runtime(float sample_rate, std::uint32_t buffer_size, std::uint32_t in_lat,
                     std::uint32_t out_lat) noexcept;
    void add_xrun() noexcept;

    // 30Hz publisher(非 RT):取走並清零區間累積、算頻譜、寫入 SHM(seqlock)。
    // instance_ids[0] = engine 輸出(慣例 0xFFFFFFFF、kinds[0] = 2)、其後 = 各軌
    // (kind 1)與 plugin(kind 0)strip 順序 — strip 位置由 ids 陣列索引本身表達。
    // running = false 時 spectrum_count 寫 0(stream 沒在跑,頻譜無意義)。
    void publish(TelemetryBlockShm& block, std::uint64_t xruns_total,
                 const std::uint32_t* instance_ids, const std::uint8_t* kinds,
                 std::size_t id_count, bool running) noexcept;

private:
    static void atomic_max(std::atomic<std::uint32_t>& t, float v) noexcept;
    static void atomic_add(std::atomic<std::uint32_t>& t, float v) noexcept;
    void compute_spectrum(float* out_db) noexcept;  // publish thread 專屬

    std::atomic<std::uint32_t> peak_l_bits_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> peak_r_bits_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> sq_l_bits_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> sq_r_bits_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> sample_counts_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> sample_rate_{};
    std::atomic<std::uint32_t> buffer_size_{};
    std::atomic<std::uint32_t> in_lat_{}, out_lat_{};

    // 頻譜 ring:RT 寫(l、r 交錯讀)、publish 讀末 2048 點;2 的冪,wrap 由 index 取模
    static constexpr std::size_t kSpectrumRingSize = 8192;
    static constexpr std::size_t kSpectrumFft = 2048;
    float spectrum_ring_[kSpectrumRingSize]{};
    alignas(64) std::atomic<std::uint32_t> ring_write_{};  // RT 寫(release)
};

// SHM 生命週期:建立(engine 唯一 writer)
HANDLE telemetry_create(TelemetryBlockShm** out_block) noexcept;

}  // namespace rmx
