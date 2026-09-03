// Engine 核心:ASIO device + 多軌 track graph + meter accumulate + SHM publish。
// 控制面由 Router 呼叫 start/stop/track_*;RT callback 只碰 atomic 與
// snapshot。M5:單鏈 rack 改為多軌 DAG(見 track_graph.hpp)。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
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

enum class PluginMutationFailure {
    kNone,
    kNotFound,
    kStateFailed,
    kBadCommand,
};

enum class PresetLoadFailure {
    kNone,
    kNotFound,
    kPresetIo,
    kPluginStateFailed,
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
        std::string path;  // P1-C:exe 完整路徑(辨識同名程序;拿不到 = 空)
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
    void set_latency_changed_cb(std::function<void(std::uint32_t, bool)> cb) {
        latency_changed_cb_ = std::move(cb);
    }
    // VST callback 只置 atomic mailbox；publish thread 再排 message，main thread
    // 持 engine mutex 後在此刷新並交易 graph。
    void handle_latency_changed(std::uint32_t instance_id, bool monitor_shadow);
    void handle_track_failed(std::uint32_t track_id);  // main thread 專屬(capture 或 render)

    bool start(const std::string& device_key, std::optional<std::uint32_t> sample_rate,
               std::optional<std::uint32_t> buffer_size, std::string& err);
    void stop() noexcept;
    bool open_control_panel(std::string& err);

    // ---- tracks(控制面;全部由 Router 的臨界區序列化)----
    // code out:特殊錯誤碼(cycle_detected/device_busy/track_not_found/bad_command),
    // main.cpp 直接當 reply code 用;通用失敗設 bad_command
    bool track_add(TrackKind kind, const std::string& name, std::uint32_t color,
                   std::uint32_t& track_id, std::string& err);
    bool track_remove(std::uint32_t track_id, std::string& err);
    bool track_set(std::uint32_t track_id, std::optional<std::string> name,
                   std::optional<std::uint32_t> color, std::optional<float> gain,
                   std::optional<bool> mute, std::string& err);
    bool track_set_output_latency_policy(std::uint32_t track_id, OutputLatencyPolicy policy,
                                  std::string& err);
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
    // session 載入:module 載不動也要原位置保留 metadata。plugin == nullptr 的
    // placeholder(不參與 DSP = 等同 bypass),availability/load_error 記原因;
    // params 直接進 host 權威表(placeholder 沒有 plugin metadata 可查)
    bool add_placeholder_plugin(std::uint32_t track_id, const std::string& module_path,
                                const std::string& class_id, const std::string& name,
                                bool bypassed, RackSlot::Availability why,
                                const std::string& load_error,
                                const std::vector<std::pair<std::uint32_t, double>>& params,
                                std::uint32_t& instance_id, std::string& err);
    // placeholder → 真 plugin(原 instanceId/位置/params/bypass 保留)。
    // 前置:呼叫端先過 sandbox verify(session 與 dispatch 同規;worker 不在 fail closed)
    bool load_placeholder(std::uint32_t instance_id, const std::string& module_path,
                          const std::string& class_id, std::string& err);
    bool remove_plugin(std::uint32_t instance_id, std::string& err,
                       PluginMutationFailure* failure = nullptr);
    bool move_plugin(std::uint32_t instance_id, std::size_t to_index, std::string& err);
    bool set_bypass(std::uint32_t instance_id, bool bypass, std::string& err,
                    PluginMutationFailure* failure = nullptr);
    bool set_monitor_bypass(std::uint32_t instance_id, bool bypass, std::string& err,
                            PluginMutationFailure* failure = nullptr);
    bool set_param(std::uint32_t instance_id, std::uint32_t param_id, double value,
                   std::string& err);
    // preset(檔案式 .vstpreset;控制面，由 Router 臨界區序列化)。
    // load 成功後 host 端 param 權威值自 controller 重同步
    bool save_preset(std::uint32_t instance_id, const std::filesystem::path& file,
                     std::string& err);
    bool load_preset(std::uint32_t instance_id, const std::filesystem::path& file,
                     std::string& err, PresetLoadFailure* failure = nullptr);
    // 把 host 權威值推給 controller(editor GUI 顯示同步);session 載入後呼,
    // set_param 只餵 RT ring、GUI 不知道。由 Router 臨界區序列化。
    void sync_controller_params(std::uint32_t instance_id);

    // 系統輸出(monitor/stream)補齊/去重:session 載入後、新 session 建立時呼。
    // 有變動才 swap_graph。回傳是否動了 graph；dirty revision 由 Router 推進。
    bool ensure_system_outputs();
    // session 載入路徑:檔案帶的 role 套到剛建好的軌(ensure_system_outputs 之後
    // 會去重/補齊)。未知 id = 無操作
    void set_track_system_role(std::uint32_t track_id, SystemRole role);
    // session 載入:全軌清空(含系統輸出軌——track_remove 擋系統軌,這裡是載入
    // 前的整段重建,必須能清)。editor/capture/render 一併收
    void clear_all_tracks();

    const std::vector<TrackNode>& tracks() const noexcept { return tracks_; }
    // control-plane snapshot 與 SHM publisher 共用 active graph 已提交的 plan。
    // graph 尚未建立／stop 後才從 control master 純函式重建。
    [[nodiscard]] TelemetryStripPlan telemetry_strip_plan() const;
    // editor host 顯示資料：tab 用 plugin 名，視窗標題用音軌名
    struct PluginTabInfo {
        std::uint32_t instance_id{};
        std::string label;
        std::string track_name;
        bool editor_capable{};
        bool bypass{};
    };
    std::vector<PluginTabInfo> plugin_tabs() const;
    const RackSlot* find_slot(std::uint32_t instance_id) const noexcept;
    bool primary_route_processes(std::uint32_t instance_id) const noexcept;
    // 最近一次成功 start 的裝置/取樣率/緩衝(session serialize 用;stop 後仍保留)
    const std::string& last_device_key() const noexcept { return last_device_key_; }
    std::uint32_t last_sample_rate() const noexcept { return last_sample_rate_; }
    std::uint32_t last_buffer_size() const noexcept { return last_buffer_size_; }

    [[nodiscard]] EngineStatusInfo status() const;
    [[nodiscard]] PdcPlanResult latency_plan() const { return last_route_plan_.latency; }
    [[nodiscard]] std::uint64_t latency_generation() const noexcept {
        return latency_generation_.load(std::memory_order_relaxed);
    }

private:
    void process(const AudioBlock& block) noexcept override;  // RT
    void bind_latency_callback(RackSlot& slot);
    static void refresh_latency(RackSlot& slot) noexcept;
    bool ensure_monitor_shadows(std::string& err);
    bool prepare_monitor_variants(std::string& err);

    AsioDevice device_;
    MeterAccumulator meters_;
    TelemetryBlockShm* shm_{};
    HANDLE shm_mapping_{};

    std::thread publish_thread_;
    std::atomic<bool> exiting_{false};

    // RT 狀態
    std::atomic<std::uint32_t> rt_sample_rate_{};  // Hz
    std::atomic<std::uint32_t> rt_plugin_fails_{};  // RT plugin process 失敗數
    std::uint64_t plugin_timing_overhead_{};        // control thread 啟動時校準
    std::atomic<int> panel_open_{0};               // 硬體面板開著(>0):期間禁 start(driver 重開 race)

    // tracks:control master + RT snapshot(atomic swap)。tracks_ 只在 Router
    // 臨界區內變更；RT 只讀 atomic snapshot。
    std::vector<TrackNode> tracks_;
    std::uint32_t next_track_id_{1};
    std::uint32_t next_instance_id_{1};
    std::atomic<std::uint64_t> latency_generation_{0};
    RoutePlan last_route_plan_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PdcDelayLine>> pdc_delay_states_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PdcDelayLine>> dry_delay_states_;
    std::string last_device_key_;     // 空 = 從未成功 start
    std::uint32_t last_sample_rate_{};
    std::uint32_t last_buffer_size_{};
    std::atomic<TrackGraph*> rt_graph_{nullptr};
    struct Retired {
        TrackGraph* graph;
        std::uint64_t tick;  // GetTickCount64;RT 可能仍在用 → grace 後刪
    };
    std::vector<Retired> retired_;

    bool swap_graph() noexcept;  // candidate 合法才 atomic commit
    bool swap_safety_graph() noexcept;  // 複製目前 RT routing，只暫停 plugin process
    void retire_graph() noexcept;           // graph 退場(RT 改讀 nullptr)
    void clear_expired_retired(bool force) noexcept;  // grace > 500ms 才刪
    // 跑著時軌道動了 ASIO pair:以最新 source/output 聯集重建裝置 buffer
    // (createBuffers 只能在 stop 狀態;聯集沒變 = 不動)。失敗 = 串流已停
    bool rebuild_asio_channels(std::string& err);
    RackSlot* find_slot_mut(std::uint32_t instance_id) noexcept;
    // bypass 旗標的交易式提交:set_bypass / set_monitor_bypass 共用流程——
    // 寫旗標、prepare/swap 任一失敗即回滾並還原 monitor shadow graph。
    bool commit_bypass_flag(bool RackSlot::* flag, std::uint32_t instance_id, bool value,
                            std::string& err, PluginMutationFailure* failure);
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
    std::function<void(std::uint32_t, bool)> latency_changed_cb_;
};

}  // namespace rmx
