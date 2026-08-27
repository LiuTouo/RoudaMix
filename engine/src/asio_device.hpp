// ASIO device runtime — 移植自 ProMixArea asio_adapter.cpp(縮編:去 probe worker、
// loopback diagnostic、strong types;保留列舉、message-only window、單例 gate、
// 雙 XRun 來源、selective buffer、kAsioOverload)
#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pcm_convert.hpp"

namespace rmx {

struct DriverEntry {
    std::string clsid;  // deviceKey
    std::string name;
};

std::vector<DriverEntry> enumerate_drivers();

struct DeviceCapability {
    std::uint32_t max_in{};             // getChannels
    std::uint32_t max_out{};
    std::uint32_t min_buffer{};
    std::uint32_t max_buffer{};
    std::uint32_t preferred_buffer{};
    std::uint32_t current_sample_rate{};
    std::uint32_t input_latency{};      // getLatencies(有效時)
    std::uint32_t output_latency{};
    bool latency_valid{};
    std::vector<std::uint32_t> sample_rates;  // canSampleRate 驗證清單
    std::vector<PcmType> input_types;         // per-channel sample type
    std::vector<PcmType> output_types;
};

struct AudioBlock {
    std::uint32_t frames{};
    std::span<float* const> inputs;   // planar f32
    std::span<float* const> outputs;
};

class IAudioCallback {
public:
    virtual ~IAudioCallback() = default;
    virtual void process(const AudioBlock& block) noexcept = 0;
};

class AsioDevice final {
public:
    AsioDevice() = default;
    ~AsioDevice();
    AsioDevice(const AsioDevice&) = delete;
    AsioDevice& operator=(const AsioDevice&) = delete;

    // 開 driver(registry 列舉 → asioOpenDriver → message-only window init)並探測能力。
    bool probe(const std::string& clsid, std::string& err);
    [[nodiscard]] const DeviceCapability& capability() const { return cap_; }
    [[nodiscard]] const std::string& clsid() const { return clsid_; }
    [[nodiscard]] const std::string& name() const { return name_; }

    // 建緩衝:前 in_count 輸入 / 前 out_count 輸出、設取樣率、單例 gate。
    bool prepare(std::uint32_t sample_rate, std::size_t in_count, std::size_t out_count,
                 std::string& err);
    bool start(std::string& err);
    void stop() noexcept;
    void close() noexcept;  // 停 + dispose + 關 driver

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::uint64_t xruns() const noexcept;
    [[nodiscard]] std::uint32_t take_notifications() noexcept;

    void set_callback(IAudioCallback* callback) noexcept { callback_ = callback; }
    [[nodiscard]] std::uint32_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] std::uint64_t callbacks() const noexcept;

private:
    struct Impl;
    Impl* impl_{};
    void close_impl_part(Impl* impl) noexcept;
    // 能力/probe 狀態(由 impl_ 填)
    DeviceCapability cap_;
    std::string clsid_;
    std::string name_;
    std::atomic<IAudioCallback*> callback_{};
    std::atomic<std::uint64_t> xruns_{};
    std::atomic<std::uint64_t> callbacks_{};
    std::atomic<std::uint32_t> notifications_{};
    std::atomic<bool> running_{false};
    std::uint32_t block_size_{};

    friend struct Impl;
};

}  // namespace rmx
