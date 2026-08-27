// roudamix-engine — pipe server,單一 instance,client 斷線續跑等重連。
// M1:ASIO 裝置 start/stop、sine/passthrough 音源、telemetry SHM publish。
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "audio_engine.hpp"
#include "frame_io.hpp"
#include "protocol.hpp"
#include "session.hpp"
#include "vst3_host.hpp"

namespace {

using rmx::Command;
using rmx::Frame;

std::atomic<uint64_t> g_epoch{0};
std::atomic<bool> g_exiting{false};
std::atomic<ULONGLONG> g_last_activity{GetTickCount64()};

// 引擎核心:常駐(process 生命週期);start/stop 只開關裝置,不分毀核心
rmx::AudioEngine g_engine;
std::mutex g_engine_mutex;  // dispatch 序列化(engine 控制面非 RT)

nlohmann::json rack_json() {
    auto arr = nlohmann::json::array();
    for (const auto& s : g_engine.rack()) {
        auto params = nlohmann::json::array();
        for (const auto& [id, v] : s.param_values)
            params.push_back({{"paramId", id}, {"normalized", v}});
        arr.push_back({
            {"instanceId", s.instance_id},
            {"name", s.name},
            {"pluginPath", s.module_path},
            {"classId", s.class_id},
            {"bypassed", s.bypass},
            {"params", params},
        });
    }
    return arr;
}

nlohmann::json status_json() {
    const auto s = g_engine.status();
    nlohmann::json j{
        {"running", s.running},
        {"deviceKey", s.running ? nlohmann::json(s.device_key) : nlohmann::json(nullptr)},
        {"sampleRate", s.sample_rate},
        {"bufferSize", s.running && s.buffer_size ? nlohmann::json(s.buffer_size)
                                                  : nlohmann::json(nullptr)},
        {"inputLatency", s.input_latency ? nlohmann::json(s.input_latency)
                                         : nlohmann::json(nullptr)},
        {"outputLatency", s.output_latency ? nlohmann::json(s.output_latency)
                                           : nlohmann::json(nullptr)},
        {"xruns", s.xruns},
        {"source", s.source},
        {"sineFreq", s.sine_freq},
        {"pluginFails", s.plugin_fails},
        {"rack", rack_json()},
        {"error", s.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(s.error)},
    };
    return j;
}

nlohmann::json snapshot_payload() {
    return rmx::make_snapshot_json(g_epoch.load(), status_json(), nlohmann::json::array());
}

// 成功 mutation:epoch 前進、對在線 client 廣播 status event
void after_mutation(HANDLE client) {
    uint64_t epoch = g_epoch.fetch_add(1) + 1;
    rmx::write_frame(client, rmx::make_event("status", status_json()));
    (void)epoch;
}

// 回覆 command;shutdown_engine 回 true(呼叫端 ack 後退出)
bool dispatch(HANDLE client, const Command& c) {
    if (c.protocol_version != rmx::kProtocolVersion) {
        rmx::write_frame(client, rmx::make_reply_err(c.id, g_epoch.load(),
                                                     "unsupported_version",
                                                     "server speaks protocol v1"));
        return false;
    }
    const uint64_t epoch = g_epoch.load();
    auto ok = [&](nlohmann::json result) {
        rmx::write_frame(client, rmx::make_reply_ok(c.id, epoch, std::move(result)));
    };
    auto fail = [&](const char* code, const std::string& msg) {
        rmx::write_frame(client, rmx::make_reply_err(c.id, epoch, code, msg));
    };

    if (c.kind == "ping") {
        ok(nlohmann::json{{"engineVersion", rmx::kEngineVersion}});
    } else if (c.kind == "get_snapshot") {
        ok(nlohmann::json{{"snapshot", snapshot_payload()}});
    } else if (c.kind == "list_devices") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        nlohmann::json devices = nlohmann::json::array();
        for (const auto& d : g_engine.list_devices()) {
            devices.push_back({
                {"deviceKey", d.key},
                {"name", d.name},
                {"maxIn", d.max_in},
                {"maxOut", d.max_out},
                {"sampleRates", d.sample_rates},
                {"preferredBufferSize", d.preferred_buffer},
            });
        }
        ok(nlohmann::json{{"devices", devices}});
    } else if (c.kind == "start") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine.status().running) {
            fail("already_running", "engine already running");
        } else {
            std::string err;
            const auto rate = c.payload["sampleRate"].is_null()
                                  ? std::optional<uint32_t>{}
                                  : std::optional<uint32_t>{c.payload["sampleRate"].get<uint32_t>()};
            if (g_engine.start(c.payload["deviceKey"].get<std::string>(), rate, err)) {
                after_mutation(client);
                ok(status_json());
            } else {
                fail("device_open_failed", err);
            }
        }
    } else if (c.kind == "stop") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        bool was = g_engine.status().running;
        g_engine.stop();
        if (was) after_mutation(client);
        ok(status_json());
    } else if (c.kind == "set_source") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const bool passthrough = c.payload["source"].get<std::string>() == "passthrough";
        const float freq = c.payload["sineFreq"].get<float>();
        std::string err;
        if (g_engine.set_source(passthrough, freq, err)) {
            after_mutation(client);
            ok(status_json());
        } else {
            fail("bad_command", err);
        }
    } else if (c.kind == "scan_plugins") {
        // 不持 g_engine_mutex:掃描可能數秒,別擋 rack mutation
        std::vector<std::filesystem::path> roots;
        if (c.payload.contains("roots") && c.payload["roots"].is_array()) {
            for (const auto& r : c.payload["roots"]) {
                if (r.is_string()) roots.push_back(std::filesystem::path{r.get<std::string>()});
            }
        }
        if (roots.empty()) {
            roots = {std::filesystem::path{L"C:\\Program Files\\Common Files\\VST3"},
                     std::filesystem::path{L"C:\\Program Files\\VST3"}};
        }
        nlohmann::json plugins = nlohmann::json::array();
        for (const auto& m : rmx::scan_vst3_dirs(roots)) {
            auto classes = nlohmann::json::array();
            for (const auto& ci : m.classes) {
                classes.push_back({{"uid", ci.uid},
                                   {"name", ci.name},
                                   {"vendor", ci.vendor},
                                   {"version", ci.version},
                                   {"subcategories", ci.subcategories}});
            }
            plugins.push_back({{"path", m.path.string()}, {"classes", classes}});
        }
        ok(nlohmann::json{{"plugins", plugins}});
    } else if (c.kind == "add_plugin") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string class_id;
        if (c.payload.contains("classId") && c.payload["classId"].is_string())
            class_id = c.payload["classId"].get<std::string>();
        std::uint32_t instance_id = 0;
        std::string err;
        if (g_engine.add_plugin(c.payload["path"].get<std::string>(), class_id,
                                instance_id, err)) {
            after_mutation(client);
            ok(nlohmann::json{{"instanceId", instance_id}, {"rack", rack_json()}});
        } else {
            fail("plugin_load_failed", err);
        }
    } else if (c.kind == "remove_plugin") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string err;
        if (g_engine.remove_plugin(c.payload["instanceId"].get<std::uint32_t>(), err)) {
            after_mutation(client);
            ok(nlohmann::json{{"rack", rack_json()}});
        } else {
            fail("plugin_not_found", err);
        }
    } else if (c.kind == "move_plugin") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string err;
        if (g_engine.move_plugin(c.payload["instanceId"].get<std::uint32_t>(),
                                 c.payload["newIndex"].get<std::size_t>(), err)) {
            after_mutation(client);
            ok(nlohmann::json{{"rack", rack_json()}});
        } else {
            fail("bad_command", err);
        }
    } else if (c.kind == "set_bypass") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string err;
        if (g_engine.set_bypass(c.payload["instanceId"].get<std::uint32_t>(),
                                c.payload["bypassed"].get<bool>(), err)) {
            after_mutation(client);
            ok(nlohmann::json{{"rack", rack_json()}});
        } else {
            fail("plugin_not_found", err);
        }
    } else if (c.kind == "set_param") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string err;
        // 高頻(旋鈕拖動):成功只 reply,不廣播 status/不動 epoch
        if (g_engine.set_param(c.payload["instanceId"].get<std::uint32_t>(),
                               c.payload["paramId"].get<std::uint32_t>(),
                               c.payload["value"].get<double>(), err)) {
            ok(nlohmann::json::object());
        } else {
            fail("param_not_found", err);
        }
    } else if (c.kind == "get_params") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const auto* slot = g_engine.find_slot(c.payload["instanceId"].get<std::uint32_t>());
        if (slot == nullptr) {
            fail("plugin_not_found", "unknown instanceId");
        } else {
            auto params = nlohmann::json::array();
            for (const auto& info : slot->plugin->params()) {
                double value = info.default_normalized;
                for (const auto& [id, v] : slot->param_values)
                    if (id == info.id) value = v;
                params.push_back({{"paramId", info.id},
                                  {"name", info.title},
                                  {"normalized", value},
                                  {"default", info.default_normalized},
                                  {"bypass", info.is_bypass}});
            }
            ok(nlohmann::json{{"instanceId", slot->instance_id}, {"params", params}});
        }
    } else if (c.kind == "save_session") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::filesystem::path file;
        if (c.payload["path"].is_string()) {
            file = std::filesystem::path(c.payload["path"].get<std::string>());
        } else {
            file = rmx::session::default_path();
        }
        std::string err;
        if (rmx::session::save(g_engine, file, err, c.payload)) {
            ok(nlohmann::json{{"savedPath", file.string()}});
        } else {
            fail("session_io", err);
        }
    } else if (c.kind == "load_session") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const auto file = std::filesystem::path(c.payload["path"].get<std::string>());
        nlohmann::json applied;
        std::string err;
        if (rmx::session::load(g_engine, file, applied, err)) {
            after_mutation(client);
            ok(applied);
        } else {
            fail("session_io", err);
        }
    } else if (c.kind == "shutdown_engine") {
        ok(nlohmann::json::object());
        return true;
    } else {
        fail("internal", c.kind + " not implemented yet (M2)");
    }
    return false;
}

// 服務單一 client:接上先推 snapshot,之後 command/reply 迴圈。回 true = shutdown 請求
bool serve_client(HANDLE client) {
    if (!rmx::write_frame(client, rmx::make_event("snapshot", snapshot_payload()))) {
        return false;
    }
    for (;;) {
        std::optional<std::vector<uint8_t>> frame;
        try {
            frame = rmx::read_frame(client);
        } catch (const rmx::FrameIoError& e) {
            std::fprintf(stderr, "[engine] bad frame: %s\n", e.what());
            return false;
        }
        if (!frame) return false;  // EOF / 斷線
        g_last_activity.store(GetTickCount64());
        nlohmann::json j = nlohmann::json::parse(*frame, nullptr, false);
        if (j.is_discarded()) {
            std::fprintf(stderr, "[engine] invalid JSON payload\n");
            return false;
        }
        try {
            Frame f = rmx::parse_frame(j, /*strict=*/false);
            if (auto* cmd = std::get_if<Command>(&f)) {
                if (dispatch(client, *cmd)) return true;
            }
            // server 收到 reply/event:契約上不該出現,忽略(容錯不斷線)
        } catch (const rmx::ParseError& e) {
            std::fprintf(stderr, "[engine] parse error: %s\n", e.what());
            rmx::write_frame(client, rmx::make_reply_err(0, g_epoch.load(), "bad_command",
                                                         e.what()));
        }
    }
}

// idle watchdog:12h 無 client 活動 → 取消 blocking ConnectNamedPipe 讓主迴圈退出
void idle_watchdog(HANDLE main_thread) {
    constexpr ULONGLONG kIdleTimeoutMs = 12ull * 60 * 60 * 1000;  // 12h
    for (;;) {
        if (g_exiting.load()) return;
        // 分段睡:shutdown 時 join 不必等滿 60s
        for (int i = 0; i < 240 && !g_exiting.load(); ++i) Sleep(250);
        if (g_exiting.load()) return;
        if (GetTickCount64() - g_last_activity.load() > kIdleTimeoutMs) {
            CancelSynchronousIo(main_thread);
            g_exiting.store(true);
            return;
        }
    }
}

// SEH 包裹層(含 C++ 物件的函式不能直接用 __try)
bool serve_client_seh(HANDLE client) {
    bool shutdown = false;
    __try {
        shutdown = serve_client(client);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[engine] SEH exception while serving client: %lX\n",
                     GetExceptionCode());
    }
    return shutdown;
}

}  // namespace

int main() {
    HANDLE singleton = CreateMutexW(nullptr, TRUE, rmx::kSingletonMutex);
    if (singleton == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "[engine] another instance already running, exiting\n");
        return 0;
    }

    HANDLE self = GetCurrentThread();
    std::thread watchdog(idle_watchdog, self);

    while (!g_exiting.load()) {
        HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\roudamix-engine",
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "[engine] CreateNamedPipeW failed: %lu\n", GetLastError());
            break;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr)
                             ? TRUE
                             : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }
        std::printf("[engine] client connected\n");
        g_last_activity.store(GetTickCount64());
        bool shutdown = serve_client_seh(pipe);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        g_last_activity.store(GetTickCount64());
        std::printf("[engine] client disconnected (shutdown=%d)\n", shutdown ? 1 : 0);
        if (shutdown) break;
    }

    g_exiting.store(true);
    if (watchdog.joinable()) watchdog.join();
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine.stop();
    }
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return 0;
}
