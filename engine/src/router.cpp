#include "router.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <exception>
#include <optional>
#include <sstream>
#include <utility>

#include "sandbox.hpp"
#include "session.hpp"
#include "session_projection.hpp"

#include "command_contract.hpp"

namespace rmx {

namespace {

constexpr std::uint32_t kScanRootTimeoutMs = 10u * 60u * 1000u;

// Err → protocol wire code 的唯一對照表(contracts/command_contract.json errorCodes)。
// handler 一律輸出 enum;字串形式只在此出現。
struct ErrName {
    Err code;
    const char* wire;
};

constexpr ErrName kErrNames[] = {
    {Err::kUnsupportedVersion, "unsupported_version"},
    {Err::kBadFrame, "bad_frame"},
    {Err::kBadCommand, "bad_command"},
    {Err::kNotRunning, "not_running"},
    {Err::kAlreadyRunning, "already_running"},
    {Err::kDeviceOpenFailed, "device_open_failed"},
    {Err::kDeviceLost, "device_lost"},
    {Err::kTrackNotFound, "track_not_found"},
    {Err::kCycleDetected, "cycle_detected"},
    {Err::kDeviceBusy, "device_busy"},
    {Err::kAppNotFound, "app_not_found"},
    {Err::kUnsupportedWindows, "unsupported_windows"},
    {Err::kPluginNotFound, "plugin_not_found"},
    {Err::kPluginLoadFailed, "plugin_load_failed"},
    {Err::kPluginNoEditor, "plugin_no_editor"},
    {Err::kParamNotFound, "param_not_found"},
    {Err::kSessionIo, "session_io"},
    {Err::kPresetIo, "preset_io"},
    {Err::kPluginStateFailed, "plugin_state_failed"},
    {Err::kInternal, "internal"},
};

const char* to_code(Err code) {
    for (const auto& entry : kErrNames)
        if (entry.code == code) return entry.wire;
    return "internal";
}

}  // namespace

Router::~Router() { shutdown(); }

void Router::connect(std::uint64_t generation, FrameSink sink) {
    std::lock_guard<std::mutex> engine_lock(engine_mutex_);
    plugin_clipboard_.reset();
    nlohmann::json last_scan;
    {
        std::lock_guard<std::mutex> scan_lock(scan_mutex_);
        last_scan = last_scan_;
    }
    {
        std::lock_guard<std::mutex> connection_lock(connection_mutex_);
        connection_generation_ = generation;
        frame_sink_ = std::move(sink);
    }
    send_to(generation, make_event(
                            "snapshot",
                            session::snapshot_json(engine_, epoch_, revision_, last_scan)));
}

void Router::disconnect(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (generation != connection_generation_) return;
    frame_sink_ = {};
}

void Router::send_to(std::uint64_t generation, const nlohmann::json& frame) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (generation == connection_generation_ && frame_sink_) frame_sink_(frame);
}

void Router::send_current(const nlohmann::json& frame) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (frame_sink_) frame_sink_(frame);
}

void Router::send_protocol_error(std::uint64_t generation, std::uint64_t id,
                                 const std::string& code,
                                 const std::string& message) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    send_to(generation, make_reply_err(id, epoch_, code, message));
}

void Router::recover_from_structured_exception(std::uint64_t generation,
                                               std::uint64_t id,
                                               const std::string& message) {
    // dispatch() 在進入任何 handler 前取得此鎖。SEH 越過 C++ frame 時
    // lock_guard 不會解構；目前仍是同一 main thread，因此由 Router 收回鎖。
    engine_mutex_.unlock();
    send_protocol_error(generation, id, "internal", message);
}

void Router::load_registry() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    vst_registry_path_ = vst_registry::cache_path_from_env();
    std::string error;
    if (vst_registry::load(vst_registry_path_, vst_registry_, error)) {
        last_scan_ = vst_registry::plugins_json(vst_registry_);
        last_scan_failed_ = vst_registry::failures_json(vst_registry_);
        std::fprintf(stderr, "[engine] VST registry loaded: %zu entries\n",
                     vst_registry_.entries.size());
    } else {
        std::fprintf(stderr, "[engine] VST registry ignored: %s\n", error.c_str());
        vst_registry_ = {};
    }
}

void Router::set_editor_command_target(HWND target) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    EditorHost::instance().set_engine(&engine_);
    EditorHost::instance().set_command_target(target);
    host_attached_ = true;
}

void Router::set_capture_failed_callback(
    std::function<void(std::uint32_t)> callback) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    engine_.set_capture_failed_cb(std::move(callback));
}

void Router::set_latency_changed_callback(
    std::function<void(std::uint32_t, bool)> callback) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    engine_.set_latency_changed_cb(std::move(callback));
}

void Router::shutdown() {
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        scan_job_.cancel.store(true, std::memory_order_release);
    }
    if (scan_job_.thread.joinable()) scan_job_.thread.join();

    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (shutdown_) return;
    if (host_attached_) EditorHost::instance().shutdown();
    engine_.stop();
    shutdown_ = true;
}

bool Router::dispatch(std::uint64_t generation, const Command& command) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (command.protocol_version != kProtocolVersion) {
        send_to(generation, make_reply_err(command.id, epoch_, "unsupported_version",
                                           "server speaks protocol v2"));
        return false;
    }
    const auto reply_epoch = epoch_;
    const auto found = routes().find(command.kind);
    Outcome outcome = found == routes().end()
                          ? failure(Err::kInternal, command.kind + " not implemented yet (M2)")
                          : found->second(*this, command.payload);
    const bool should_shutdown = outcome.shutdown;
    complete(generation, command, reply_epoch, std::move(outcome));
    return should_shutdown;
}

bool Router::dispatch_guarded(std::uint64_t generation, const Command& command) {
    try {
        return dispatch(generation, command);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[engine] exception while dispatching %s: %s\n",
                     command.kind.c_str(), error.what());
        std::lock_guard<std::mutex> lock(engine_mutex_);
        send_to(generation, make_reply_err(command.id, epoch_, "internal", error.what()));
    } catch (...) {
        std::fprintf(stderr, "[engine] unknown exception while dispatching %s\n",
                     command.kind.c_str());
        std::lock_guard<std::mutex> lock(engine_mutex_);
        send_to(generation,
                make_reply_err(command.id, epoch_, "internal", "unknown exception"));
    }
    return false;
}

const std::unordered_map<std::string, Router::Route>& Router::routes() {
    static const std::unordered_map<std::string, Route> value{
        {"ping", &Router::route<EmptyRequest, &Router::handle_ping>},
        {"get_snapshot", &Router::route<EmptyRequest, &Router::handle_get_snapshot>},
        {"get_latency_report", &Router::route<EmptyRequest, &Router::handle_get_latency_report>},
        {"list_devices", &Router::route<EmptyRequest, &Router::handle_list_devices>},
        {"list_audio_apps", &Router::route<EmptyRequest, &Router::handle_list_audio_apps>},
        {"list_render_devices", &Router::route<EmptyRequest, &Router::handle_list_render_devices>},
        {"list_capture_devices", &Router::route<EmptyRequest, &Router::handle_list_capture_devices>},
        {"start", &Router::route<StartRequest, &Router::handle_start>},
        {"stop", &Router::route<EmptyRequest, &Router::handle_stop>},
        {"open_device_panel", &Router::route<EmptyRequest, &Router::handle_open_device_panel>},
        {"track_add", &Router::route<TrackAddRequest, &Router::handle_track_add>},
        {"track_remove", &Router::route<TrackIdRequest, &Router::handle_track_remove>},
        {"track_set", &Router::route<TrackSetRequest, &Router::handle_track_set>},
        {"track_set_source", &Router::route<TrackSourceRequest, &Router::handle_track_set_source>},
        {"track_set_dests", &Router::route<TrackDestsRequest, &Router::handle_track_set_dests>},
        {"track_set_output", &Router::route<TrackOutputRequest, &Router::handle_track_set_output>},
        {"track_set_output_latency_policy", &Router::route<TrackOutputLatencyPolicyRequest, &Router::handle_track_set_output_latency_policy>},
        {"track_move", &Router::route<TrackMoveRequest, &Router::handle_track_move>},
        {"start_scan", &Router::route<StartScanRequest, &Router::handle_start_scan>},
        {"cancel_scan", &Router::route<EmptyRequest, &Router::handle_cancel_scan>},
        {"add_plugin", &Router::route<AddPluginRequest, &Router::handle_add_plugin>},
        {"remove_plugin", &Router::route<InstanceRequest, &Router::handle_remove_plugin>},
        {"move_plugin", &Router::route<MovePluginRequest, &Router::handle_move_plugin>},
        {"copy_plugin", &Router::route<InstanceRequest, &Router::handle_copy_plugin>},
        {"paste_plugin", &Router::route<PastePluginRequest, &Router::handle_paste_plugin>},
        {"duplicate_plugin", &Router::route<DuplicatePluginRequest, &Router::handle_duplicate_plugin>},
        {"set_bypass", &Router::route<BypassRequest, &Router::handle_set_bypass>},
        {"set_monitor_bypass", &Router::route<BypassRequest, &Router::handle_set_monitor_bypass>},
        {"retry_plugin", &Router::route<RetryPluginRequest, &Router::handle_retry_plugin>},
        {"set_param", &Router::route<SetParamRequest, &Router::handle_set_param>},
        {"get_params", &Router::route<InstanceRequest, &Router::handle_get_params>},
        {"open_editor", &Router::route<InstanceRequest, &Router::handle_open_editor>},
        {"close_editor", &Router::route<InstanceRequest, &Router::handle_close_editor>},
        {"save_preset", &Router::route<PresetRequest, &Router::handle_save_preset>},
        {"load_preset", &Router::route<PresetRequest, &Router::handle_load_preset>},
        {"save_session", &Router::route<SaveSessionRequest, &Router::handle_save_session>},
        {"load_session", &Router::route<LoadSessionRequest, &Router::handle_load_session>},
        {"ensure_system_outputs", &Router::route<EmptyRequest, &Router::handle_ensure_system_outputs>},
        {"set_editor_owner", &Router::route<EditorOwnerRequest, &Router::handle_set_editor_owner>},
        {"shutdown_engine", &Router::route<EmptyRequest, &Router::handle_shutdown_engine>},
    };
    return value;
}

void Router::complete(std::uint64_t generation, const Command& command,
                      std::uint64_t reply_epoch, Outcome outcome) {
    if (!outcome.ok) {
        const char* wire = to_code(outcome.error.code);
        // contract errors 清單執法:code 必須屬於該指令宣告集;違反 = handler 分類
        // bug,debug build 中止,release 零開銷(NDEBUG)
        assert(rmx::contract::is_declared_error(command.kind, wire) &&
               "error code not declared for this command (see contracts/command_contract.json)");
        send_to(generation, make_reply_err(command.id, reply_epoch, wire,
                                           std::move(outcome.error.message)));
        if (outcome.push_status_on_error)
            send_to(generation,
                    make_event("status", session::status_json(engine_, revision_)));
        return;
    }

#ifndef NDEBUG
    try {
        contract::validate_result(command.kind, outcome.result);
    } catch (const contract::ValidationError& error) {
        std::fprintf(stderr, "result contract violation for %s: %s\n",
                     command.kind.c_str(), error.what());
        assert(false &&
               "result does not match command contract (see contracts/command_contract.json)");
    }
#endif

    const auto policy = policy_for(outcome.effect);
    if (policy.advances_revision) ++revision_;
    if (policy.broadcasts_status) {
        ++epoch_;
        send_to(generation, make_event("status", session::status_json(engine_, revision_)));
        EditorHost::instance().notify_tracks_changed();
    }
    send_to(generation, make_reply_ok(command.id, reply_epoch, std::move(outcome.result)));
}

void Router::publish_effect(Effect effect) {
    const auto policy = policy_for(effect);
    if (policy.advances_revision) ++revision_;
    if (!policy.broadcasts_status) return;
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        connected = static_cast<bool>(frame_sink_);
    }
    if (!connected) return;
    ++epoch_;
    send_current(make_event("status", session::status_json(engine_, revision_)));
    EditorHost::instance().notify_tracks_changed();
}

Router::Outcome Router::success(nlohmann::json result, Effect effect) {
    Outcome outcome;
    outcome.result = std::move(result);
    outcome.effect = effect;
    return outcome;
}

Router::Outcome Router::failure(Failure failure_value, bool push_status) {
    Outcome outcome;
    outcome.ok = false;
    outcome.error = std::move(failure_value);
    outcome.push_status_on_error = push_status;
    return outcome;
}

Router::Outcome Router::failure(Err code, std::string message, bool push_status) {
    return failure(Failure{code, std::move(message)}, push_status);
}

Router::EffectPolicy Router::policy_for(Effect effect) {
    switch (effect) {
        case Effect::kNone: return {};
        case Effect::kStatus: return {.broadcasts_status = true};
        case Effect::kDirty:
            return {.advances_revision = true, .broadcasts_status = true};
        case Effect::kSilentDirty: return {.advances_revision = true};
    }
    return {};
}

std::vector<std::filesystem::path> Router::default_vst_roots() {
    std::vector<std::filesystem::path> roots{
        std::filesystem::path{L"C:\\Program Files\\Common Files\\VST3"},
        std::filesystem::path{L"C:\\Program Files\\VST3"},
    };
    const DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (needed > 1) {
        std::wstring local_app_data(static_cast<std::size_t>(needed), L'\0');
        const DWORD written =
            GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), needed);
        if (written > 0 && written < needed) {
            local_app_data.resize(written);
            roots.push_back(std::filesystem::path{local_app_data} / L"Programs" /
                            L"Common" / L"VST3");
        }
    }
    return roots;
}

void Router::scan_job_thread(std::uint64_t job_id,
                             std::vector<std::filesystem::path> roots) {
    vst_registry::Registry candidate;
    candidate.roots = roots;
    std::string job_error;
    int outcome = 0;
    const std::size_t total = roots.size();
    for (std::size_t index = 0; index < total; ++index) {
        if (scan_job_.cancel.load(std::memory_order_acquire)) {
            outcome = 2;
            break;
        }
        send_current(make_event(
            "scan_progress",
            {{"jobId", job_id},
             {"done", index},
             {"total", total},
             {"root", vst_registry::path_utf8(roots[index])}}));
        std::error_code root_error;
        const bool root_exists = std::filesystem::exists(roots[index], root_error);
        if (root_error) {
            job_error = "cannot inspect VST root: " +
                        vst_registry::path_utf8(roots[index]) + ": " +
                        root_error.message();
            outcome = 1;
            break;
        }
        if (!root_exists) continue;
        if (!std::filesystem::is_directory(roots[index], root_error) || root_error) {
            job_error = "VST root is not an accessible directory: " +
                        vst_registry::path_utf8(roots[index]);
            outcome = 1;
            break;
        }
        const auto worker = sandbox::run_worker(
            {"--scan", vst_registry::path_utf8(roots[index])},
            kScanRootTimeoutMs, &scan_job_.cancel);
        if (worker.cancelled) {
            outcome = 2;
            break;
        }
        if (!worker.spawned) {
            job_error = "sandbox worker unavailable (roudamix-worker.exe missing)";
            outcome = 1;
            break;
        }
        if (worker.timed_out) {
            job_error = "scan timed out after 10 minutes (worker killed)";
            outcome = 1;
            break;
        }
        if (worker.exit_code != 0) {
            job_error = "scan worker exited with code " +
                        std::to_string(worker.exit_code) + " for root: " +
                        vst_registry::path_utf8(roots[index]);
            outcome = 1;
            break;
        }
        std::istringstream stream(worker.output);
        std::string line;
        while (std::getline(stream, line)) {
            const auto json = nlohmann::json::parse(line, nullptr, false);
            if (json.is_discarded()) continue;
            vst_registry::Entry entry;
            if (vst_registry::entry_from_json(json, entry))
                candidate.entries.push_back(std::move(entry));
        }
    }

    nlohmann::json committed_plugins;
    nlohmann::json committed_failed;
    if (outcome == 0) {
        vst_registry::normalize(candidate);
        if (!vst_registry::save_atomic(vst_registry_path_, candidate, job_error)) outcome = 1;
    }
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        if (outcome == 0) {
            vst_registry_ = std::move(candidate);
            last_scan_ = vst_registry::plugins_json(vst_registry_);
            last_scan_failed_ = vst_registry::failures_json(vst_registry_);
            committed_plugins = last_scan_;
            committed_failed = last_scan_failed_;
        }
        scan_job_.running.store(false, std::memory_order_release);
    }
    if (outcome == 0) {
        send_current(make_event("scan_done", {{"jobId", job_id},
                                                {"plugins", committed_plugins},
                                                {"failed", committed_failed}}));
    } else if (outcome == 1) {
        send_current(make_event("scan_failed", {{"jobId", job_id},
                                                  {"error", job_error}}));
    } else {
        send_current(make_event("scan_cancelled", {{"jobId", job_id}}));
    }
}

Router::Outcome Router::handle_ping(const EmptyRequest&) {
    return success({{"engineVersion", kEngineVersion}});
}

Router::Outcome Router::handle_get_snapshot(const EmptyRequest&) {
    nlohmann::json last_scan;
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        last_scan = last_scan_;
    }
    return success({{"snapshot",
                     session::snapshot_json(engine_, epoch_, revision_, last_scan)}});
}

Router::Outcome Router::handle_get_latency_report(const EmptyRequest&) {
    return success({{"report", session::latency_report_json(engine_)}});
}

Router::Outcome Router::handle_list_devices(const EmptyRequest&) {
    auto devices = nlohmann::json::array();
    for (const auto& device : engine_.list_devices()) {
        devices.push_back({{"deviceKey", device.key},
                           {"name", device.name},
                           {"maxIn", device.max_in},
                           {"maxOut", device.max_out},
                           {"sampleRates", device.sample_rates},
                           {"currentSampleRate", device.current_sample_rate},
                           {"minBufferSize", device.min_buffer},
                           {"maxBufferSize", device.max_buffer},
                           {"preferredBufferSize", device.preferred_buffer},
                           {"bufferSizes", device.buffer_sizes},
                           {"inputNames", device.input_names},
                           {"outputNames", device.output_names}});
    }
    return success({{"devices", std::move(devices)}});
}

Router::Outcome Router::handle_list_audio_apps(const EmptyRequest&) {
    auto apps = nlohmann::json::array();
    for (const auto& app : engine_.list_audio_apps())
        apps.push_back({{"pid", app.pid},
                        {"name", app.name},
                        {"path", app.path.empty() ? nlohmann::json(nullptr)
                                                   : nlohmann::json(app.path)}});
    return success({{"apps", std::move(apps)}});
}

Router::Outcome Router::handle_list_render_devices(const EmptyRequest&) {
    auto devices = nlohmann::json::array();
    for (const auto& device : engine_.list_render_devices())
        devices.push_back({{"id", device.id},
                           {"name", device.name},
                           {"default", device.is_default},
                           {"sampleRate", device.sample_rate}});
    return success({{"devices", std::move(devices)}});
}

Router::Outcome Router::handle_list_capture_devices(const EmptyRequest&) {
    auto devices = nlohmann::json::array();
    for (const auto& device : engine_.list_capture_devices())
        devices.push_back({{"id", device.id},
                           {"name", device.name},
                           {"default", device.is_default},
                           {"sampleRate", device.sample_rate}});
    return success({{"devices", std::move(devices)}});
}

Router::Outcome Router::handle_start(const StartRequest& request) {
    if (engine_.status().running)
        return failure(Err::kAlreadyRunning, "engine already running");
    if (auto fail = engine_.start(request.device_key, request.sample_rate, request.buffer_size))
        return failure(std::move(*fail), true);
    return success(session::status_json(engine_, revision_), Effect::kStatus);
}

Router::Outcome Router::handle_stop(const EmptyRequest&) {
    engine_.stop();
    return success(session::status_json(engine_, revision_), Effect::kStatus);
}

Router::Outcome Router::handle_open_device_panel(const EmptyRequest&) {
    if (!engine_.status().running)
        return failure(Err::kNotRunning, "engine not running");
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        generation = connection_generation_;
    }
    std::thread([this, generation] {
        if (auto fail = engine_.open_control_panel()) {
            std::fprintf(stderr, "[engine] open panel failed: %s\n", fail->message.c_str());
            return;
        }
        send_to(generation, make_event("devices_changed", nlohmann::json::object()));
    }).detach();
    return success({{"panel", true}});
}

Router::Outcome Router::handle_track_add(const TrackAddRequest& request) {
    std::uint32_t track_id = 0;
    if (auto fail = engine_.track_add(request.kind, request.name, request.color, track_id))
        return failure(std::move(*fail));
    return success({{"trackId", track_id}, {"tracks", session::tracks_json(engine_)}},
                   Effect::kDirty);
}

Router::Outcome Router::handle_track_remove(const TrackIdRequest& request) {
    if (auto fail = engine_.track_remove(request.track_id)) return failure(std::move(*fail));
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_track_set(const TrackSetRequest& request) {
    if (auto fail = engine_.track_set(request.track_id, request.name, request.color,
                                      request.gain, request.mute))
        return failure(std::move(*fail));
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_track_set_source(const TrackSourceRequest& request) {
    if (auto fail = engine_.track_set_source(request.track_id, request.source))
        return failure(std::move(*fail), true);
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_track_set_dests(const TrackDestsRequest& request) {
    if (auto fail = engine_.track_set_dests(request.track_id, request.dests))
        return failure(std::move(*fail), true);
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_track_set_output(const TrackOutputRequest& request) {
    if (auto fail = engine_.track_set_output(request.track_id, request.output))
        return failure(std::move(*fail), true);
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_track_set_output_latency_policy(
    const TrackOutputLatencyPolicyRequest& request) {
    if (auto fail = engine_.track_set_output_latency_policy(request.track_id, request.policy))
        return failure(std::move(*fail));
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_track_move(const TrackMoveRequest& request) {
    if (auto fail = engine_.track_move(request.track_id, request.new_index))
        return failure(std::move(*fail));
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_start_scan(const StartScanRequest& request) {
    auto roots = request.roots;
    if (roots.empty()) roots = default_vst_roots();

    std::uint64_t job_id = 0;
    bool reused = false;
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        if (scan_job_.running.load(std::memory_order_acquire)) {
            job_id = scan_job_.id;
            reused = true;
        } else {
            if (scan_job_.thread.joinable()) scan_job_.thread.join();
            scan_job_.cancel.store(false, std::memory_order_release);
            job_id = next_scan_id_++;
            scan_job_.id = job_id;
            scan_job_.running.store(true, std::memory_order_release);
            scan_job_.thread =
                std::thread(&Router::scan_job_thread, this, job_id, std::move(roots));
        }
    }
    return success({{"jobId", job_id}, {"reused", reused}});
}

Router::Outcome Router::handle_cancel_scan(const EmptyRequest&) {
    std::uint64_t job_id = 0;
    bool cancelling = false;
    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        if (scan_job_.running.load(std::memory_order_acquire)) {
            scan_job_.cancel.store(true, std::memory_order_release);
            job_id = scan_job_.id;
            cancelling = true;
        }
    }
    return success({{"jobId", job_id}, {"cancelling", cancelling}});
}

Router::Outcome Router::handle_add_plugin(const AddPluginRequest& request) {
    std::string error;
    if (session::preflight_plugin(engine_, request.path, request.class_id, error) !=
        sandbox::PreflightFailure::kNone)
        return failure(Err::kPluginLoadFailed, std::move(error));
    std::uint32_t instance_id = 0;
    if (auto fail = engine_.add_plugin(request.track_id, request.path, request.class_id,
                                       instance_id))
        return failure(std::move(*fail));
    return success({{"instanceId", instance_id},
                    {"trackId", request.track_id},
                    {"tracks", session::tracks_json(engine_)}},
                   Effect::kDirty);
}

Router::Outcome Router::handle_remove_plugin(const InstanceRequest& request) {
    if (auto fail = engine_.remove_plugin(request.instance_id))
        return failure(std::move(*fail));
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_copy_plugin(const InstanceRequest& request) {
    PluginSnapshot snapshot;
    if (auto fail = engine_.capture_plugin(request.instance_id, snapshot))
        return failure(std::move(*fail));
    plugin_clipboard_ = std::move(snapshot);
    ++clipboard_sequence_;
    return success({{"clipboardId", std::to_string(clipboard_sequence_)},
                    {"name", plugin_clipboard_->name}});
}

Router::Outcome Router::insert_plugin_copy(const PluginSnapshot& snapshot,
                                           std::uint32_t track_id, std::size_t index) {
    std::string error;
    if (session::preflight_plugin(engine_, snapshot.module_path, snapshot.class_id, error) !=
        sandbox::PreflightFailure::kNone)
        return failure(Err::kPluginLoadFailed, std::move(error));
    std::uint32_t instance_id{};
    if (auto fail = engine_.insert_plugin_snapshot(snapshot, track_id, index, instance_id))
        return failure(std::move(*fail));
    return success({{"instanceId", instance_id}, {"trackId", track_id},
                    {"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_paste_plugin(const PastePluginRequest& request) {
    if (!plugin_clipboard_ || request.clipboard_id != std::to_string(clipboard_sequence_))
        return failure(Err::kBadCommand, "plugin clipboard is empty or expired");
    return insert_plugin_copy(*plugin_clipboard_, request.track_id, request.new_index);
}

Router::Outcome Router::handle_duplicate_plugin(const DuplicatePluginRequest& request) {
    PluginSnapshot snapshot;
    if (auto fail = engine_.capture_plugin(request.instance_id, snapshot))
        return failure(std::move(*fail));
    return insert_plugin_copy(snapshot, request.track_id, request.new_index);
}

Router::Outcome Router::handle_move_plugin(const MovePluginRequest& request) {
    if (auto fail = engine_.move_plugin(request.instance_id, request.new_index))
        return failure(std::move(*fail));
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_bypass(const BypassRequest& request, bool monitor) {
    auto fail = monitor ? engine_.set_monitor_bypass(request.instance_id, request.bypassed)
                        : engine_.set_bypass(request.instance_id, request.bypassed);
    if (fail) return failure(std::move(*fail));
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_set_bypass(const BypassRequest& request) {
    return handle_bypass(request, false);
}

Router::Outcome Router::handle_set_monitor_bypass(const BypassRequest& request) {
    return handle_bypass(request, true);
}

Router::Outcome Router::handle_retry_plugin(const RetryPluginRequest& request) {
    const auto* slot = engine_.find_slot(request.instance_id);
    if (slot == nullptr) return failure(Err::kPluginNotFound, "unknown instanceId");
    if (!slot->is_placeholder())
        return failure(Err::kBadCommand, "instance is not a placeholder");
    const std::string module_path =
        request.path ? *request.path : slot->module_path;
    const std::string class_id = slot->class_id;
    std::string error;
    if (session::preflight_plugin(engine_, module_path, class_id, error) !=
        sandbox::PreflightFailure::kNone)
        return failure(Err::kPluginLoadFailed, std::move(error));
    if (auto fail = engine_.load_placeholder(request.instance_id, module_path, class_id))
        return failure(std::move(*fail));
    // #13:retry = 使用者對該 module 的明確核准,寫回 registry(見 approve_module)
    approve_module(module_path, slot);
    return success({{"instanceId", request.instance_id},
                    {"tracks", session::tracks_json(engine_)}},
                   Effect::kDirty);
}

// #13:把「使用者核准過的 module」以 path+fingerprint 記進 registry,session restore
// 閘門(session.cpp restore_allowed)才會放行。掃描來的 entry 只重釘 fingerprint
// (保留 classes);registry 外的路徑(relocate 挑的新檔)合成 entry。registry 檔
// 寫失敗只損跨啟動持久,本輪記憶體內核准仍生效。
void Router::approve_module(const std::string& module_path, const RackSlot* slot) {
    const auto path = vst_registry::path_from_utf8(module_path);
    if (path.empty()) return;
    vst_registry::Fingerprint fp;
    std::string error;
    if (!vst_registry::fingerprint(path, fp, error)) return;
    const auto key = vst_registry::path_key(path);
    std::lock_guard<std::mutex> lock(scan_mutex_);
    bool merged = false;
    for (auto& existing : vst_registry_.entries) {
        if (vst_registry::path_key(existing.path) == key) {
            existing.fingerprint = fp;
            merged = true;
            break;
        }
    }
    if (!merged) {
        vst_registry::Entry entry;
        entry.path = path;
        entry.fingerprint = fp;
        entry.classes.push_back({{"uid", slot != nullptr ? slot->class_id : ""},
                                 {"name", slot != nullptr ? slot->name : ""},
                                 {"vendor", ""},
                                 {"version", ""},
                                 {"subcategories", ""}});
        vst_registry_.entries.push_back(std::move(entry));
        vst_registry::normalize(vst_registry_);
    }
    last_scan_ = vst_registry::plugins_json(vst_registry_);
    if (!vst_registry_path_.empty())
        (void)vst_registry::save_atomic(vst_registry_path_, vst_registry_, error);
}

Router::Outcome Router::handle_set_param(const SetParamRequest& request) {
    if (auto fail = engine_.set_param(request.instance_id, request.param_id, request.value))
        return failure(std::move(*fail));
    return success(nlohmann::json::object(), Effect::kSilentDirty);
}

Router::Outcome Router::handle_get_params(const InstanceRequest& request) {
    const auto* slot = engine_.find_slot(request.instance_id);
    if (slot == nullptr) return failure(Err::kPluginNotFound, "unknown instanceId");
    if (slot->plugin == nullptr)
        return success({{"instanceId", slot->instance_id},
                        {"params", nlohmann::json::array()}});
    auto params = nlohmann::json::array();
    for (const auto& info : slot->plugin->params()) {
        double value = info.default_normalized;
        for (const auto& [id, current] : slot->param_values)
            if (id == info.id) value = current;
        params.push_back({{"paramId", info.id},
                          {"name", info.title},
                          {"normalized", value},
                          {"default", info.default_normalized},
                          {"bypass", info.is_bypass}});
    }
    return success({{"instanceId", slot->instance_id},
                    {"params", std::move(params)}});
}

Router::Outcome Router::handle_open_editor(const InstanceRequest& request) {
    const auto* slot = engine_.find_slot(request.instance_id);
    if (slot == nullptr) return failure(Err::kPluginNotFound, "unknown instanceId");
    if (slot->plugin == nullptr)
        return failure(Err::kPluginNoEditor, "plugin not loaded (placeholder)");
    std::string error;
    if (!EditorHost::instance().open(request.instance_id, error))
        return failure(Err::kPluginNoEditor, std::move(error));
    return success({{"instanceId", request.instance_id}, {"editor", true}});
}

Router::Outcome Router::handle_close_editor(const InstanceRequest& request) {
    const auto* slot = engine_.find_slot(request.instance_id);
    if (slot == nullptr) return failure(Err::kPluginNotFound, "unknown instanceId");
    EditorHost::instance().close(slot->instance_id);
    return success();
}

Router::Outcome Router::handle_save_preset(const PresetRequest& request) {
    if (engine_.find_slot(request.instance_id) == nullptr)
        return failure(Err::kPluginNotFound, "unknown instanceId");
    if (auto fail = engine_.save_preset(request.instance_id,
                                        std::filesystem::path(request.path)))
        return failure(std::move(*fail));
    return success({{"savedPath", request.path}});
}

Router::Outcome Router::handle_load_preset(const PresetRequest& request) {
    if (auto fail = engine_.load_preset(request.instance_id,
                                        std::filesystem::path(request.path)))
        return failure(std::move(*fail));
    return success({{"tracks", session::tracks_json(engine_)}}, Effect::kDirty);
}

Router::Outcome Router::handle_save_session(const SaveSessionRequest& request) {
    const auto file = request.path ? std::filesystem::path(*request.path)
                                   : session::default_path();
    if (auto fail = session::save(engine_, file, request.overrides_json()))
        return failure(std::move(*fail));
    return success({{"savedPath", file.string()}, {"revision", revision_}});
}

Router::Outcome Router::handle_load_session(const LoadSessionRequest& request) {
    plugin_clipboard_.reset();
    nlohmann::json applied;
    if (auto fail = session::load(engine_, request.path, applied))
        return failure(std::move(*fail));
    applied["revision"] = revision_ + 1;
    return success(std::move(applied), Effect::kDirty);
}

Router::Outcome Router::handle_ensure_system_outputs(const EmptyRequest&) {
    const bool changed = engine_.ensure_system_outputs();
    return success({{"tracks", session::tracks_json(engine_)},
                    {"revision", revision_ + (changed ? 1u : 0u)}},
                   changed ? Effect::kDirty : Effect::kNone);
}

Router::Outcome Router::handle_set_editor_owner(const EditorOwnerRequest& request) {
    EditorHost::instance().set_owner(
        reinterpret_cast<HWND>(request.hwnd));
    return success();
}

Router::Outcome Router::handle_shutdown_engine(const EmptyRequest&) {
    auto outcome = success();
    outcome.shutdown = true;
    return outcome;
}

void Router::handle_host_command(const EditorHostCmd& command) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    Effect effect = Effect::kNone;
    if (command.kind == kHostBypass) {
        const auto* slot = engine_.find_slot(command.instance_id);
        if (slot == nullptr) return;
        effect = engine_.set_bypass(command.instance_id, !slot->bypass)
                     ? Effect::kDirty
                     : Effect::kStatus;
    } else if (command.kind == kHostPreset) {
        if (engine_.find_slot(command.instance_id) == nullptr) return;
        effect = engine_.load_preset(command.instance_id,
                                     std::filesystem::path(command.path))
                     ? Effect::kDirty
                     : Effect::kStatus;
    } else if (command.kind == kHostSavePreset) {
        if (engine_.find_slot(command.instance_id) == nullptr) return;
        (void)engine_.save_preset(command.instance_id,
                                  std::filesystem::path(command.path));
        effect = Effect::kStatus;
    } else {
        return;
    }
    publish_effect(effect);
}

void Router::handle_track_failed(std::uint32_t track_id) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    engine_.handle_track_failed(track_id);
    publish_effect(Effect::kStatus);
}

void Router::handle_latency_changed(std::uint32_t instance_id,
                                    bool monitor_shadow) {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    engine_.handle_latency_changed(instance_id, monitor_shadow);
    send_current(make_event("status", session::status_json(engine_, revision_)));
}

}  // namespace rmx
