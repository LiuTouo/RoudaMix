// Engine 核心:ASIO device + 音源(sine/passthrough)+ meter accumulate + SHM publish。
// 控制面(dispatch thread)呼叫 start/stop/set_source;RT callback 只碰 atomic。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "asio_device.hpp"
#include "rack.hpp"
#include "telemetry.hpp"

namespace rmx {

struct EngineStatusInfo {
    bool running{};
    std::string device_key;   // 空 = 未選
    float sample_rate{};      // 0 = 未啟動
    std::uint32_t buffer_size{};
    std::uint32_t input_latency{};
    std::uint32_t output_latency{};
    std::uint64_t xruns{};
    std::string source;       // "sine" | "passthrough"
    float sine_freq{440.0F};
    bool input_mono{true};    // 輸入 ch1 複製到 L+R(mic 監聽);false = 1:1 立體聲
    std::uint32_t plugin_fails{};  // RT plugin process 失敗累計
    std::string error;        // 最近錯誤(空 = 無)
};

class AudioEngine final : public IAudioCallback {
public:
    AudioEngine();
    ~AudioEngine();

    // 裝置列舉 + 輕量 probe(開 driver 讀能力後即關;單 driver 崩由上層 SEH 吸收)
    struct DeviceSummary {
        std::string key, name;
        std::uint32_t max_in{}, max_out{}, min_buffer{}, max_buffer{}, preferred_buffer{};
        std::vector<std::uint32_t> sample_rates;
        std::vector<std::uint32_t> buffer_sizes;  // granularity 展開(driver 真正接受的)
        std::uint32_t current_sample_rate{};      // driver 現行率(硬體面板才是權威)
    };
    std::vector<DeviceSummary> list_devices();

    bool start(const std::string& device_key, std::optional<std::uint32_t> sample_rate,
               std::optional<std::uint32_t> buffer_size, std::optional<bool> input_mono,
               std::string& err);
    void stop() noexcept;
    bool set_source(bool passthrough, float sine_freq, std::string& err);
    bool open_control_panel(std::string& err);

    // ---- rack(控制面;全部假設 g_engine_mutex 已持有)----
    bool add_plugin(const std::string& module_path, const std::string& class_id,
                    std::uint32_t& instance_id, std::string& err);
    bool remove_plugin(std::uint32_t instance_id, std::string& err);
    bool move_plugin(std::uint32_t instance_id, std::size_t to_index, std::string& err);
    bool set_bypass(std::uint32_t instance_id, bool bypass, std::string& err);
    bool set_param(std::uint32_t instance_id, std::uint32_t param_id, double value,
                   std::string& err);
    // preset(檔案式 .vstpreset;控制面,假設 g_engine_mutex 已持有)。
    // load 成功後 host 端 param 權威值自 controller 重同步
    bool save_preset(std::uint32_t instance_id, const std::filesystem::path& file,
                     std::string& err);
    bool load_preset(std::uint32_t instance_id, const std::filesystem::path& file,
                     std::string& err);
    // 把 host 權威值推給 controller(editor GUI 顯示同步);session 載入後呼,
    // set_param 只餵 RT、GUI 不知道。假設 g_engine_mutex 已持有
    void sync_controller_params(std::uint32_t instance_id);
    const std::vector<RackSlot>& rack() const noexcept { return rack_; }
    // 最近一次成功 start 的裝置/取樣率/緩衝(session serialize 用;stop 後仍保留)
    const std::string& last_device_key() const noexcept { return last_device_key_; }
    std::uint32_t last_sample_rate() const noexcept { return last_sample_rate_; }
    std::uint32_t last_buffer_size() const noexcept { return last_buffer_size_; }
    // 找 slot(add 後 UI 要參數表用);nullptr = 無
    const RackSlot* find_slot(std::uint32_t instance_id) const noexcept;

    [[nodiscard]] EngineStatusInfo status() const;

private:
    void process(const AudioBlock& block) noexcept override;  // RT

    AsioDevice device_;
    MeterAccumulator meters_;
    TelemetryBlockShm* shm_{};
    HANDLE shm_mapping_{};

    std::thread publish_thread_;
    std::atomic<bool> exiting_{false};

    // RT 狀態
    std::atomic<std::uint32_t> source_passthrough_{false};
    std::atomic<std::uint32_t> sine_freq_bits_{};  // f32 bit pattern
    std::atomic<std::uint32_t> rt_sample_rate_{};  // Hz
    std::atomic<std::uint64_t> rt_phase_{};        // sine 相位(1<<32 = 2π)
    std::atomic<std::uint32_t> rt_plugin_fails_{};  // RT plugin process 失敗數
    std::atomic<std::uint32_t> input_mono_{1u};    // 1 = ch1 複製 L+R(預設 mic 場景)
    std::atomic<int> panel_open_{0};               // 硬體面板開著(>0):期間禁 start(driver 重開 race)

    // rack:control master + RT snapshot(atomic swap)。rack_ 只在 main thread
    // 變(dispatch + editor performEdit 都在 main thread,無鎖即序列化)。
    std::vector<RackSlot> rack_;
    std::uint32_t next_instance_id_{1};
    std::string last_device_key_;     // 空 = 從未成功 start
    std::uint32_t last_sample_rate_{};
    std::uint32_t last_buffer_size_{};
    std::atomic<RackChain*> rt_rack_{nullptr};
    struct Retired {
        RackChain* chain;
        std::uint64_t tick;  // GetTickCount64;RT 可能仍在用 → grace 後刪
    };
    std::vector<Retired> retired_;

    // RT ping-pong bus([pingpong][channel])
    float rt_bus_[2][2][kMaxBlockFrames]{};

    void swap_rack() noexcept;              // master 拷貝成新 chain、atomic 換、舊鏈退役
    void retire_rack() noexcept;            // 鏈退場(RT 改讀 nullptr)
    void clear_expired_retired(bool force) noexcept;  // grace > 500ms 才刪
    RackSlot* find_slot_mut(std::uint32_t instance_id) noexcept;
};

}  // namespace rmx
