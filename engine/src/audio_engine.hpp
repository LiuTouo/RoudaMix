// Engine 核心:ASIO device + 多軌 track graph + meter accumulate + SHM publish。
// 控制面(dispatch thread)呼叫 start/stop/track_*;RT callback 只碰 atomic 與
// snapshot。M5:單鏈 rack 改為多軌 DAG(見 track_graph.hpp)。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "asio_device.hpp"
#include "rack.hpp"
#include "telemetry.hpp"
#include "track_graph.hpp"

namespace rmx {

struct EngineStatusInfo {
    bool running{};
    std::string device_key;   // 空 = 未選
    float sample_rate{};      // 0 = 未啟動
    std::uint32_t buffer_size{};
    std::uint32_t input_latency{};
    std::uint32_t output_latency{};
    std::uint64_t xruns{};
    std::uint32_t track_count{};
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
        std::vector<std::string> input_names;     // per-channel 名稱(UI 下拉)
        std::vector<std::string> output_names;
    };
    std::vector<DeviceSummary> list_devices();

    // M5b:正在出聲的 app(active audio sessions;UI 程序選擇器用)
    struct AudioAppInfo {
        std::uint32_t pid{};
        std::string name;
    };
    std::vector<AudioAppInfo> list_audio_apps();
    // M5c:WASAPI render endpoints(串流軌裝置選擇器用;default = 預設裝置)
    struct RenderDeviceInfo {
        std::string id, name;
        bool is_default{};
        std::uint32_t sample_rate{};  // mix format 率
    };
    std::vector<RenderDeviceInfo> list_render_devices();
    // M5b/M5c:capture/render pump 偵錯(程序結束/裝置失效)→ main thread
    // PostMessage 轉 handle_track_failed(track_id);engine 不鎖、不碰 pipe
    void set_capture_failed_cb(std::function<void(std::uint32_t)> cb) {
        capture_failed_cb_ = std::move(cb);
    }
    void handle_track_failed(std::uint32_t track_id);  // main thread 專屬(capture 或 render)

    bool start(const std::string& device_key, std::optional<std::uint32_t> sample_rate,
               std::optional<std::uint32_t> buffer_size, std::string& err);
    void stop() noexcept;
    bool open_control_panel(std::string& err);

    // ---- tracks(控制面;全部假設 g_engine_mutex 已持有)----
    // code out:特殊錯誤碼(cycle_detected/device_busy/track_not_found/bad_command),
    // main.cpp 直接當 reply code 用;通用失敗設 bad_command
    bool track_add(TrackKind kind, const std::string& name, std::uint32_t color,
                   std::uint32_t& track_id, std::string& err);
    bool track_remove(std::uint32_t track_id, std::string& err);
    bool track_set(std::uint32_t track_id, std::optional<std::string> name,
                   std::optional<std::uint32_t> color, std::optional<float> gain,
                   std::optional<bool> mute, std::string& err);
    bool track_set_source(std::uint32_t track_id, const TrackSource& source,
                          std::string& err, std::string& code);
    bool track_set_dests(std::uint32_t track_id, std::vector<std::uint32_t> dests,
                         std::string& err, std::string& code);
    bool track_set_output(std::uint32_t track_id, const TrackOutput& output,
                          std::string& err, std::string& code);
    bool track_move(std::uint32_t track_id, std::size_t new_index, std::string& err);

    // ---- plugins(簽名與 M4 相同,只是搜尋/插入範圍變成各軌 chain)----
    bool add_plugin(std::uint32_t track_id, const std::string& module_path,
                    const std::string& class_id, std::uint32_t& instance_id, std::string& err);
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
    // set_param 只餵 RT ring、GUI 不知道。假設 g_engine_mutex 已持有
    void sync_controller_params(std::uint32_t instance_id);

    const std::vector<TrackNode>& tracks() const noexcept { return tracks_; }
    // editor host tab 列(全部軌的 plugin,「軌名 · plugin 名」)
    struct PluginTabInfo {
        std::uint32_t instance_id{};
        std::string label;
        bool editor_capable{};
        bool bypass{};
    };
    std::vector<PluginTabInfo> plugin_tabs() const;
    const RackSlot* find_slot(std::uint32_t instance_id) const noexcept;
    // 最近一次成功 start 的裝置/取樣率/緩衝(session serialize 用;stop 後仍保留)
    const std::string& last_device_key() const noexcept { return last_device_key_; }
    std::uint32_t last_sample_rate() const noexcept { return last_sample_rate_; }
    std::uint32_t last_buffer_size() const noexcept { return last_buffer_size_; }

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
    std::atomic<std::uint32_t> rt_sample_rate_{};  // Hz
    std::atomic<std::uint32_t> rt_plugin_fails_{};  // RT plugin process 失敗數
    std::atomic<int> panel_open_{0};               // 硬體面板開著(>0):期間禁 start(driver 重開 race)

    // tracks:control master + RT snapshot(atomic swap)。tracks_ 只在 main thread
    // 變(dispatch + editor performEdit 都在 main thread,無鎖即序列化)。
    std::vector<TrackNode> tracks_;
    std::uint32_t next_track_id_{1};
    std::uint32_t next_instance_id_{1};
    std::string last_device_key_;     // 空 = 從未成功 start
    std::uint32_t last_sample_rate_{};
    std::uint32_t last_buffer_size_{};
    std::atomic<TrackGraph*> rt_graph_{nullptr};
    struct Retired {
        TrackGraph* graph;
        std::uint64_t tick;  // GetTickCount64;RT 可能仍在用 → grace 後刪
    };
    std::vector<Retired> retired_;

    void swap_graph() noexcept;             // master 拷貝成新 graph、atomic 換、舊 graph 退役
    void retire_graph() noexcept;           // graph 退場(RT 改讀 nullptr)
    void clear_expired_retired(bool force) noexcept;  // grace > 500ms 才刪
    // 跑著時軌道動了 ASIO pair:以最新 source/output 聯集重建裝置 buffer
    // (createBuffers 只能在 stop 狀態;聯集沒變 = 不動)。失敗 = 串流已停
    bool rebuild_asio_channels(std::string& err);
    RackSlot* find_slot_mut(std::uint32_t instance_id) noexcept;
    TrackNode* find_track_mut(std::uint32_t track_id) noexcept;
    // 同 ASIO pair 全 engine 只能一軌用(source 與 output 各自方向內查重)
    bool asio_in_pair_busy(std::uint32_t ch, std::uint32_t except_track) const noexcept;
    bool asio_out_pair_busy(std::uint32_t ch, std::uint32_t except_track) const noexcept;
    // M5b:app capture 生命週期(控制面)。ensure 失敗 = t.track_error 帶原因
    bool ensure_capture(TrackNode& t, std::uint32_t dst_rate, std::string& err);
    void stop_capture(TrackNode& t) noexcept;
    void stop_captures() noexcept;
    // M5c:wasapi render sink 生命週期(同語意)
    bool ensure_render(TrackNode& t, std::uint32_t src_rate, std::string& err);
    void stop_render(TrackNode& t) noexcept;
    void stop_renders() noexcept;
    std::function<void(std::uint32_t)> capture_failed_cb_;
};

}  // namespace rmx
