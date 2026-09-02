#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "audio_engine.hpp"
#include "editor_host.hpp"
#include "protocol.hpp"
#include "router_requests.hpp"
#include "vst_registry.hpp"

namespace rmx {

class Router final {
public:
    using FrameSink = std::function<void(const nlohmann::json&)>;

    Router() = default;
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    void connect(std::uint64_t generation, FrameSink sink);
    void disconnect(std::uint64_t generation);
    bool dispatch_guarded(std::uint64_t generation, const Command& command);
    // /EHsc 不會為 SEH 展開 dispatch 的 lock_guard；只由外層 SEH handler 呼叫。
    void recover_from_structured_exception(std::uint64_t generation,
                                           std::uint64_t id,
                                           const std::string& message);
    void send_protocol_error(std::uint64_t generation, std::uint64_t id,
                             const std::string& code, const std::string& message);

    void load_registry();
    void set_editor_command_target(HWND target);
    void set_capture_failed_callback(std::function<void(std::uint32_t)> callback);
    void set_latency_changed_callback(
        std::function<void(std::uint32_t, bool)> callback);
    void handle_host_command(const EditorHostCmd& command);
    void handle_track_failed(std::uint32_t track_id);
    void handle_latency_changed(std::uint32_t instance_id, bool monitor_shadow);
    void shutdown();

private:
    enum class Effect { kNone, kStatus, kDirty, kSilentDirty };

    struct EffectPolicy {
        bool advances_revision{};
        bool broadcasts_status{};
    };

    struct Outcome {
        bool ok{true};
        nlohmann::json result = nlohmann::json::object();
        std::string error_code;
        std::string error_message;
        Effect effect{Effect::kNone};
        bool push_status_on_error{};
        bool shutdown{};
    };

    using EmptyRequest = router_request::Empty;
    using StartRequest = router_request::Start;
    using TrackAddRequest = router_request::TrackAdd;
    using TrackIdRequest = router_request::TrackId;
    using TrackSetRequest = router_request::TrackSet;
    using TrackSourceRequest = router_request::TrackSourceSet;
    using TrackDestsRequest = router_request::TrackDestsSet;
    using TrackOutputRequest = router_request::TrackOutputSet;
    using TrackLatencyPolicyRequest = router_request::TrackLatencyPolicySet;
    using TrackMoveRequest = router_request::TrackMove;
    using StartScanRequest = router_request::StartScan;
    using AddPluginRequest = router_request::AddPlugin;
    using InstanceRequest = router_request::Instance;
    using MovePluginRequest = router_request::MovePlugin;
    using BypassRequest = router_request::Bypass;
    using RetryPluginRequest = router_request::RetryPlugin;
    using SetParamRequest = router_request::SetParam;
    using PresetRequest = router_request::Preset;
    using SaveSessionRequest = router_request::SaveSession;
    using LoadSessionRequest = router_request::LoadSession;
    using EditorOwnerRequest = router_request::EditorOwner;

    using Route = Outcome (*)(Router& router, const nlohmann::json& payload);

    template <typename Request, Outcome (Router::*Handler)(const Request&)>
    static Outcome route(Router& router, const nlohmann::json& payload) {
        return (router.*Handler)(Request{payload});
    }

    struct ScanJob {
        std::uint64_t id{};
        std::thread thread;
        std::atomic<bool> running{false};
        std::atomic<bool> cancel{false};
    };

    void send_to(std::uint64_t generation, const nlohmann::json& frame);
    void send_current(const nlohmann::json& frame);
    bool dispatch(std::uint64_t generation, const Command& command);
    static const std::unordered_map<std::string, Route>& routes();
    void complete(std::uint64_t generation, const Command& command,
                  std::uint64_t reply_epoch, Outcome outcome);
    void publish_effect(Effect effect);
    static Outcome success(nlohmann::json result = nlohmann::json::object(),
                           Effect effect = Effect::kNone);
    static Outcome failure(std::string code, std::string message,
                           bool push_status = false);
    static EffectPolicy policy_for(Effect effect);
    bool verify_plugin(const std::string& module_path, const std::string& class_id,
                       std::string& error);
    static std::vector<std::filesystem::path> default_vst_roots();
    void scan_job_thread(std::uint64_t job_id,
                         std::vector<std::filesystem::path> roots);

    Outcome handle_ping(const EmptyRequest& request);
    Outcome handle_get_snapshot(const EmptyRequest& request);
    Outcome handle_get_latency_report(const EmptyRequest& request);
    Outcome handle_list_devices(const EmptyRequest& request);
    Outcome handle_list_audio_apps(const EmptyRequest& request);
    Outcome handle_list_render_devices(const EmptyRequest& request);
    Outcome handle_start(const StartRequest& request);
    Outcome handle_stop(const EmptyRequest& request);
    Outcome handle_open_device_panel(const EmptyRequest& request);
    Outcome handle_track_add(const TrackAddRequest& request);
    Outcome handle_track_remove(const TrackIdRequest& request);
    Outcome handle_track_set(const TrackSetRequest& request);
    Outcome handle_track_set_source(const TrackSourceRequest& request);
    Outcome handle_track_set_dests(const TrackDestsRequest& request);
    Outcome handle_track_set_output(const TrackOutputRequest& request);
    Outcome handle_track_set_latency_policy(const TrackLatencyPolicyRequest& request);
    Outcome handle_track_move(const TrackMoveRequest& request);
    Outcome handle_start_scan(const StartScanRequest& request);
    Outcome handle_cancel_scan(const EmptyRequest& request);
    Outcome handle_add_plugin(const AddPluginRequest& request);
    Outcome handle_remove_plugin(const InstanceRequest& request);
    Outcome handle_move_plugin(const MovePluginRequest& request);
    Outcome handle_set_bypass(const BypassRequest& request);
    Outcome handle_set_monitor_bypass(const BypassRequest& request);
    Outcome handle_retry_plugin(const RetryPluginRequest& request);
    Outcome handle_set_param(const SetParamRequest& request);
    Outcome handle_get_params(const InstanceRequest& request);
    Outcome handle_open_editor(const InstanceRequest& request);
    Outcome handle_close_editor(const InstanceRequest& request);
    Outcome handle_save_preset(const PresetRequest& request);
    Outcome handle_load_preset(const PresetRequest& request);
    Outcome handle_save_session(const SaveSessionRequest& request);
    Outcome handle_load_session(const LoadSessionRequest& request);
    Outcome handle_ensure_system_outputs(const EmptyRequest& request);
    Outcome handle_set_editor_owner(const EditorOwnerRequest& request);
    Outcome handle_shutdown_engine(const EmptyRequest& request);
    Outcome handle_bypass(const BypassRequest& request, bool monitor);

    std::mutex engine_mutex_;
    AudioEngine engine_;
    std::uint64_t epoch_{};
    std::uint64_t revision_{};
    nlohmann::json last_scan_ = nlohmann::json::array();

    std::mutex scan_mutex_;
    ScanJob scan_job_;
    std::uint64_t next_scan_id_{1};
    nlohmann::json last_scan_failed_ = nlohmann::json::array();
    vst_registry::Registry vst_registry_;
    std::filesystem::path vst_registry_path_;
    bool host_attached_{};
    bool shutdown_{};

    std::mutex connection_mutex_;
    std::uint64_t connection_generation_{};
    FrameSink frame_sink_;
};

}  // namespace rmx
