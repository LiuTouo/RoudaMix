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
    std::int32_t buffer_granularity{};  // getBufferSize:>0 步進、0 = 2 的冪、-1 = 任意
    std::uint32_t current_sample_rate{};
    std::uint32_t input_latency{};      // getLatencies(有效時)
    std::uint32_t output_latency{};
    bool latency_valid{};
    std::vector<std::uint32_t> sample_rates;  // canSampleRate 驗證清單
    std::vector<PcmType> input_types;         // per-channel sample type
    std::vector<PcmType> output_types;
    std::vector<std::string> input_names;     // per-channel 名稱(getChannelInfo;UI 下拉用)
    std::vector<std::string> output_names;

    // granularity 展開的合法 buffer 清單(UI 下拉直接用;空 = 常見值過濾 [min,max])
    [[nodiscard]] std::vector<std::uint32_t> buffer_options() const;
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

    // 建緩衝:顯式 channel index 集合(M5:軌道自選 pair,不再「前 N 軌」)、
    // 設取樣率、單例 gate。buffer_size = 0 用 driver preferred;否則需在
    // [min,max] 內(driver 是最終權威,granularity 不合 createBuffers 會失敗回 err)
    bool prepare(std::uint32_t sample_rate, const std::vector<std::uint32_t>& in_channels,
                 const std::vector<std::uint32_t>& out_channels, std::uint32_t buffer_size,
                 std::string& err);
    bool start(std::string& err);
    void stop() noexcept;
    void close() noexcept;  // 停 + dispose + 關 driver
    // 開 driver 自帶控制面板(硬體設定視窗);須 probe 過。driver 面板是硬體
    // 設定最終權威(取樣率/緩衝 driver 拒絕時,使用者從面板改)
    bool open_control_panel(std::string& err);

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::uint64_t xruns() const noexcept;
    [[nodiscard]] std::uint32_t take_notifications() noexcept;

    void set_callback(IAudioCallback* callback) noexcept { callback_ = callback; }
    [[nodiscard]] std::uint32_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] std::uint64_t callbacks() const noexcept;
    // scratch 位置 → 裝置 channel index(prepare 後有效;AudioEngine 解析
    // 軌道 source/output 對應 block.inputs/outputs 哪個位置用)
    [[nodiscard]] const std::vector<std::uint32_t>& input_map() const noexcept {
        return input_map_;
    }
    [[nodiscard]] const std::vector<std::uint32_t>& output_map() const noexcept {
        return output_map_;
    }

private:
    struct Impl;
    Impl* impl_{};
    void close_impl_part(Impl* impl) noexcept;
    // 能力/probe 狀態(由 impl_ 填)
    DeviceCapability cap_;
    std::string clsid_;
    std::string name_;
    std::string driver_dll_path_;  // vendor 面板 exe fallback 掃描基準
    std::atomic<IAudioCallback*> callback_{};
    std::atomic<std::uint64_t> xruns_{};
    std::atomic<std::uint64_t> callbacks_{};
    std::atomic<std::uint32_t> notifications_{};
    std::atomic<bool> running_{false};
    std::uint32_t block_size_{};
    std::vector<std::uint32_t> input_map_;   // scratch index → 裝置 channel
    std::vector<std::uint32_t> output_map_;

    friend struct Impl;
};

}  // namespace rmx
