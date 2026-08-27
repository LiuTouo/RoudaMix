// Telemetry SHM(`Local\roudamix-telemetry`)— 契約:contracts/telemetry_abi.md
// RT 端 accumulate(僅 atomic);30Hz publish thread 寫 seqlock block。
#pragma once

#include <atomic>
#include <cstdint>

#include <windows.h>

namespace rmx {

constexpr std::uint32_t kTelemetryMagic = 0x524D5854;  // "RMXT"
constexpr std::uint32_t kTelemetryAbiVersion = 1;
constexpr std::size_t kTelemetryStrips = 16;
constexpr std::size_t kTelemetryBytes = 4096;

#pragma pack(push, 8)
struct TelemetryStripShm {
    std::uint32_t instance_id;
    std::uint32_t pad0;
    float peak_l;
    float peak_r;
    float rms_l;
    float rms_r;
    std::uint32_t reserved[2];
};
static_assert(sizeof(TelemetryStripShm) == 32);

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
};
static_assert(sizeof(TelemetryBlockShm) == 560);
#pragma pack(pop)

// RT 安全累積器(全 atomic;strip 0 = engine 輸出,其餘保留給 M2 rack)
class MeterAccumulator final {
public:
    // Audio thread:samples 為本 block 樣本數,squared_sum 為振幅平方和
    void accumulate(std::size_t strip, float peak_l, float peak_r, float squared_sum_l,
                    float squared_sum_r, std::uint32_t samples) noexcept;
    void set_runtime(float sample_rate, std::uint32_t buffer_size, std::uint32_t in_lat,
                     std::uint32_t out_lat) noexcept;
    void add_xrun() noexcept;

    // 30Hz publisher(非 RT):取走並清零區間累積、寫入 SHM(seqlock)。
    // instance_ids[0] = engine 輸出(慣例 0xFFFFFFFF)、其後 = rack slots 順序。
    void publish(TelemetryBlockShm& block, std::uint64_t xruns_total,
                 const std::uint32_t* instance_ids, std::size_t id_count) noexcept;

private:
    static void atomic_max(std::atomic<std::uint32_t>& t, float v) noexcept;
    static void atomic_add(std::atomic<std::uint32_t>& t, float v) noexcept;

    std::atomic<std::uint32_t> peak_l_bits_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> peak_r_bits_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> sq_l_bits_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> sq_r_bits_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> sample_counts_[kTelemetryStrips]{};
    std::atomic<std::uint32_t> sample_rate_{};
    std::atomic<std::uint32_t> buffer_size_{};
    std::atomic<std::uint32_t> in_lat_{}, out_lat_{};
};

// SHM 生命週期:建立(engine 唯一 writer)
HANDLE telemetry_create(TelemetryBlockShm** out_block) noexcept;

}  // namespace rmx
