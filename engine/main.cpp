// roudamix-engine — pipe server,單一 instance,client 斷線續跑等重連。
// M4a:main thread 跑 Win32 message loop(VST3 editor 需要 — JUCE 系 plugin 假設
// host 單一 UI thread;拆 thread 開 editor 會跨 thread 互等死鎖),pipe I/O 在
// worker thread;command 經 PostMessage 進 main thread dispatch(寫回 reply/event
// 由 main 執行,跨 thread pipe 寫用 g_write_mutex 序列化)。
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <exception>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include "audio_engine.hpp"
#include "editor_host.hpp"
#include "frame_io.hpp"
#include "protocol.hpp"
#include "sandbox.hpp"
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

// main thread 的 message-only window + 自訂訊息(pipe thread → main 的 task 隊列)
constexpr UINT WM_APP_TASK = WM_APP + 1;      // LPARAM = Task*(main 處理後 delete)
constexpr UINT WM_APP_QUIT = WM_APP + 2;      // shutdown_engine:main loop 退出
constexpr UINT WM_APP_CAPTURE_ERR = WM_APP + 4;  // WPARAM = track_id(app capture 偵錯)
constexpr wchar_t kMainWndClass[] = L"RmxEngineMain";
HWND g_main_hwnd = nullptr;
std::mutex g_write_mutex;  // pipe thread 與 main 都會寫:幀序列化
// 目前 pipe instance(pipe thread 擁有生命週期;斷線先清 null 再 CloseHandle)。
// watchdog/退出用它 CancelIoEx 喚醒 overlapped 的 accept/read。
std::atomic<HANDLE> g_active_pipe{nullptr};
// 連線代數:每次 accept 遞增。Task 帶當時 gen;dispatch 前驗證 —— 舊連線的
// 排隊 task 直接丟棄,不寫 reply(handle 可能已被 Close/重用為新連線)。
std::atomic<uint64_t> g_conn_gen{0};

// E:背景掃描 job 狀態(掃描 thread ↔ main thread;registry 進 snapshot lastScan)
struct ScanJob {
    std::uint64_t id{};
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
};
std::mutex g_scan_mutex;  // job 生命週期 + registry 寫入
ScanJob g_scan_job;
std::uint64_t g_scan_next_id = 1;
nlohmann::json g_last_scan = nlohmann::json::array();        // OK modules(全域 registry)
nlohmann::json g_last_scan_failed = nlohmann::json::array(); // 壞 module quarantine(診斷用)

struct Task {
    uint64_t gen;
    HANDLE client;
    Command cmd;
};

// 兩 thread(main dispatch / pipe thread bad_command reply)都會寫同一 pipe:
// 每次 write_frame 前後持鎖,幀不交錯
void send_frame(HANDLE client, const nlohmann::json& j) {
    std::lock_guard<std::mutex> lock(g_write_mutex);
    if (!rmx::write_frame(client, j))
        std::fprintf(stderr, "[engine] send_frame write failed: %lu\n", GetLastError());
}

nlohmann::json source_json(const rmx::TrackSource& src) {
    switch (src.type) {
        case rmx::TrackSource::kSine:
            return nlohmann::json{{"type", "sine"}, {"freq", src.sine_freq}};
        case rmx::TrackSource::kAsioIn:
            return nlohmann::json{{"type", "asioIn"},
                                  {"channel", src.asio_in_ch},
                                  {"mono", src.mono}};
        case rmx::TrackSource::kApp:
            return nlohmann::json{{"type", "app"},
                                  {"pid", src.pid},
                                  {"name", src.app_name.empty() ? nlohmann::json(nullptr)
                                                                : nlohmann::json(src.app_name)}};
        case rmx::TrackSource::kNone:
            return nullptr;
    }
    return nullptr;
}

nlohmann::json output_json(const rmx::TrackOutput& out) {
    switch (out.type) {
        case rmx::TrackOutput::kAsioOut:
            return nlohmann::json{{"type", "asioOut"}, {"channel", out.asio_out_ch}};
        case rmx::TrackOutput::kWasapiRender:
            return nlohmann::json{{"type", "wasapi"}, {"deviceId", out.wasapi_id}};
        case rmx::TrackOutput::kNone:
            return nullptr;
    }
    return nullptr;
}

nlohmann::json tracks_json() {
    auto arr = nlohmann::json::array();
    // strip 預算與 swap_graph 同一純函式:master 序兩輪(track 先、plugin 後),
    // 超出 64 預算的節點 metered=false —— UI 顯示「無錶」而不是誤當靜音(P1-H)
    const auto strips = rmx::plan_telemetry_strips(g_engine.tracks(), rmx::kTelemetryStrips);
    std::size_t ti = 0;
    for (const auto& t : g_engine.tracks()) {
        auto plugins = nlohmann::json::array();
        for (const auto& s : t.chain) {
            auto params = nlohmann::json::array();
            for (const auto& [id, v] : s.param_values)
                params.push_back({{"paramId", id}, {"normalized", v}});
            plugins.push_back({
                {"instanceId", s.instance_id},
                {"name", s.name},
                {"pluginPath", s.module_path},
                {"classId", s.class_id},
                {"bypassed", s.bypass},
                {"params", params},
                // placeholder(missing/broken)標記:UI 黯淡顯示 + 重試/移除
                {"availability", rmx::availability_str(s.availability)},
                {"loadError", s.load_error.empty() ? nlohmann::json(nullptr)
                                                   : nlohmann::json(s.load_error)},
            });
        }
        const char* role = rmx::system_role_str(t.system_role);
        const bool metered =
            ti < strips.size() && strips[ti].track_strip != rmx::kNoStrip;
        arr.push_back({
            {"trackId", t.track_id},
            {"kind", rmx::track_kind_str(t.kind)},
            {"systemRole", role != nullptr ? nlohmann::json(role) : nlohmann::json(nullptr)},
            {"name", t.name},
            {"color", t.color},
            {"source", source_json(t.source)},
            {"dests", t.dests},
            {"output", output_json(t.output)},
            {"gain", t.gain},
            {"mute", t.mute},
            {"plugins", plugins},
            {"metered", metered},
            {"error", t.track_error.empty() ? nlohmann::json(nullptr) : nlohmann::json(t.track_error)},
        });
        ++ti;
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
        {"trackCount", s.track_count},
        {"pluginFails", s.plugin_fails},
        {"revision", g_engine.revision()},  // 權威 dirty 版號(含 set_param)
        {"tracks", tracks_json()},
        {"error", s.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(s.error)},
    };
    return j;
}

nlohmann::json snapshot_payload() {
    auto snap = rmx::make_snapshot_json(g_epoch.load(), status_json(), tracks_json());
    std::lock_guard<std::mutex> lock(g_scan_mutex);
    snap["lastScan"] = g_last_scan;  // 全域 plugin registry(重連後 UI 不用重掃)
    return snap;
}

// 成功 mutation:epoch 前進、對在線 client 廣播 status event(main thread 呼)
void after_mutation(HANDLE client) {
    uint64_t epoch = g_epoch.fetch_add(1) + 1;
    send_frame(client, rmx::make_event("status", status_json()));
    (void)epoch;
    rmx::EditorHost::instance().notify_tracks_changed();  // host 視窗 tab 同步
}

// stream 狀態變了但指令失敗(rebuild 回滾失敗 = engine 已停等):不動 epoch,
// 廣播權威 status 讓 UI 重同步(UI 之前只靠成功路徑的 status event 會顯示 stale running)
void push_status(HANDLE client) {
    send_frame(client, rmx::make_event("status", status_json()));
}

// ---- E:背景掃描 job。引擎主 thread 不等 worker(reply 立即回 jobId);
// progress/done/failed/cancelled 走 event;同時只允許一個 job(重複 start_scan
// 共用現行 job);結果 = 全域 registry(宣告於檔案頂部,snapshot lastScan 共用)----

void send_event_active(const nlohmann::json& ev) {
    if (HANDLE c = g_active_pipe.load(std::memory_order_acquire)) send_frame(c, ev);
}

// 掃描 thread body:逐 root 跑 worker(cancel 可中斷),行解析 OK/FAIL;
// 收尾把 registry 換掉並推結案 event。worker 不在 = fail closed(job failed,
// 不 fallback in-process:壞 DLL 的代價是炸 engine process,不能省 worker)
void scan_job_thread(std::uint64_t job_id, std::vector<std::filesystem::path> roots) {
    nlohmann::json plugins = nlohmann::json::array();
    nlohmann::json failed = nlohmann::json::array();
    std::string job_error;
    int outcome = 0;  // 0 = done、1 = failed、2 = cancelled
    const std::size_t total = roots.size();
    for (std::size_t i = 0; i < total; ++i) {
        if (g_scan_job.cancel.load(std::memory_order_acquire)) {
            outcome = 2;
            break;
        }
        send_event_active(rmx::make_event(
            "scan_progress",
            {{"jobId", job_id}, {"done", i}, {"total", total}, {"root", roots[i].string()}}));
        // 掃描在隔離 worker 跑:壞 module 崩潰只死 worker,已 flush 的行 = 增量照收
        const auto r = rmx::sandbox::run_worker({"--scan", roots[i].string()}, 120000,
                                                &g_scan_job.cancel);
        if (r.cancelled) {
            outcome = 2;
            break;
        }
        if (!r.spawned) {
            job_error = "sandbox worker unavailable (roudamix-worker.exe missing)";
            outcome = 1;
            break;
        }
        if (r.timed_out) {
            job_error = "scan timed out after 120s (worker killed)";
            outcome = 1;
            break;
        }
        std::istringstream stream(r.output);
        std::string line;
        while (std::getline(stream, line)) {
            const auto tab1 = line.find('\t');
            if (tab1 == std::string::npos) continue;
            const auto tab2 = line.find('\t', tab1 + 1);
            if (tab2 == std::string::npos) continue;
            const std::string status = line.substr(0, tab1);
            const std::string path = line.substr(tab1 + 1, tab2 - tab1 - 1);
            if (status == "OK") {
                nlohmann::json classes =
                    nlohmann::json::parse(line.substr(tab2 + 1), nullptr, false);
                if (classes.is_discarded() || !classes.is_array() || classes.empty()) continue;
                plugins.push_back({{"path", path}, {"classes", classes}});
            } else if (status == "FAIL") {
                // 壞 module 進 quarantine(診斷用);add_plugin 的 verify 也不會放它進來
                failed.push_back({{"path", path},
                                  {"error", line.substr(tab2 + 1)}});
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_scan_mutex);
        g_last_scan = std::move(plugins);
        g_last_scan_failed = std::move(failed);
        g_scan_job.running.store(false, std::memory_order_release);
    }
    if (outcome == 0) {
        std::lock_guard<std::mutex> lock(g_scan_mutex);
        send_event_active(rmx::make_event("scan_done", {
            {"jobId", job_id},
            {"plugins", g_last_scan},
            {"failed", g_last_scan_failed},
        }));
    } else if (outcome == 1) {
        send_event_active(rmx::make_event(
            "scan_failed", {{"jobId", job_id}, {"error", job_error}}));
    } else {
        send_event_active(
            rmx::make_event("scan_cancelled", {{"jobId", job_id}}));
    }
}

// EditorHost 內部指令(bypass / 載入 preset):main thread 排隊執行,這裡持鎖
// 走與 dispatch 正規分支相同的路徑(host wnd_proc 可能在持鎖中重入,絕不直接鎖)
void handle_host_cmd(const rmx::EditorHostCmd& cmd) {
    HANDLE client = g_active_pipe.load();
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    std::string err;
    if (cmd.kind == rmx::kHostBypass) {
        // 對話框/排隊期間 slot 可能已移除:查無即棄
        const auto* slot = g_engine.find_slot(cmd.instance_id);
        if (slot == nullptr) return;
        (void)g_engine.set_bypass(cmd.instance_id, !slot->bypass, err);
    } else if (cmd.kind == rmx::kHostPreset) {
        if (g_engine.find_slot(cmd.instance_id) == nullptr) return;
        (void)g_engine.load_preset(cmd.instance_id,
                                   std::filesystem::path(cmd.path), err);
        // 失敗沉默:plugin 聲音不變即訊號;成功走 after_mutation 同步 UI/host tab
    } else if (cmd.kind == rmx::kHostSavePreset) {
        if (g_engine.find_slot(cmd.instance_id) == nullptr) return;
        (void)g_engine.save_preset(cmd.instance_id,
                                   std::filesystem::path(cmd.path), err);
    } else {
        return;
    }
    if (client != nullptr) after_mutation(client);
}

// 回覆 command;shutdown_engine 回 true(呼叫端 ack 後退出)。main thread 專屬。
bool dispatch(HANDLE client, const Command& c) {
    if (c.protocol_version != rmx::kProtocolVersion) {
        send_frame(client, rmx::make_reply_err(c.id, g_epoch.load(),
                                               "unsupported_version",
                                               "server speaks protocol v2"));
        return false;
    }
    const uint64_t epoch = g_epoch.load();
    auto ok = [&](nlohmann::json result) {
        send_frame(client, rmx::make_reply_ok(c.id, epoch, std::move(result)));
    };
    auto fail = [&](const char* code, const std::string& msg) {
        send_frame(client, rmx::make_reply_err(c.id, epoch, code, msg));
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
                {"currentSampleRate", d.current_sample_rate},
                {"minBufferSize", d.min_buffer},
                {"maxBufferSize", d.max_buffer},
                {"preferredBufferSize", d.preferred_buffer},
                {"bufferSizes", d.buffer_sizes},
                {"inputNames", d.input_names},
                {"outputNames", d.output_names},
            });
        }
        ok(nlohmann::json{{"devices", devices}});
    } else if (c.kind == "list_audio_apps") {
        // 主 thread COM(STA)列舉 active audio sessions;不持鎖(純讀系統狀態)
        nlohmann::json apps = nlohmann::json::array();
        for (const auto& a : g_engine.list_audio_apps()) {
            apps.push_back({{"pid", a.pid},
                            {"name", a.name},
                            {"path", a.path.empty() ? nlohmann::json(nullptr)
                                                    : nlohmann::json(a.path)}});
        }
        ok(nlohmann::json{{"apps", apps}});
    } else if (c.kind == "list_render_devices") {
        nlohmann::json devices = nlohmann::json::array();
        for (const auto& d : g_engine.list_render_devices()) {
            devices.push_back({
                {"id", d.id},
                {"name", d.name},
                {"default", d.is_default},
                {"sampleRate", d.sample_rate},
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
            const auto buffer =
                c.payload.contains("bufferSize") && c.payload["bufferSize"].is_number_unsigned()
                    ? std::optional<uint32_t>{c.payload["bufferSize"].get<uint32_t>()}
                    : std::optional<uint32_t>{};
            if (g_engine.start(c.payload["deviceKey"].get<std::string>(), rate, buffer, err)) {
                after_mutation(client);
                ok(status_json());
            } else {
                fail("device_open_failed", err);
                // 失敗也廣播權威 status(engine 可能停在 stopped):UI 不得顯示 stale running
                push_status(client);
            }
        }
    } else if (c.kind == "stop") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        // 冪等 stop 也推 status:UI 收到正確 stopped(reply 的 result UI 不套,
        // status event 才是 UI 的對齊來源)
        g_engine.stop();
        after_mutation(client);
        ok(status_json());
    } else if (c.kind == "open_device_panel") {
        // controlPanel() 多數 driver 同步開視窗即返;少數 modal(關面板才返)= 凍結
        // dispatch 與 pipe reply。detach thread 開、立刻回覆;面板期間 start 被
        // engine panel_open_ 擋(driver 銷毀 race)。SSL(Thesycon)例外:controlPanel
        // 是 stub,fallback spawn vendor 面板 exe(AsioDevice 內處理)。
        // 面板關閉(modal 返回 / cpl exe 結束)= driver 現行設定可能已變,推
        // devices_changed 讓 client 重 list_devices;寫前驗 gen,斷線重連後不寫舊 handle
        if (!g_engine.status().running) {
            fail("not_running", "engine not running");
        } else {
            const uint64_t gen = g_conn_gen.load(std::memory_order_acquire);
            std::thread([client, gen] {
                std::string err;
                if (!g_engine.open_control_panel(err)) {
                    std::fprintf(stderr, "[engine] open panel failed: %s\n", err.c_str());
                    return;
                }
                if (gen == g_conn_gen.load(std::memory_order_acquire))
                    send_frame(client, rmx::make_event("devices_changed", nlohmann::json::object()));
            }).detach();
            ok(nlohmann::json{{"panel", true}});
        }
    } else if (c.kind == "track_add") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        rmx::TrackKind kind = rmx::TrackKind::kAudio;
        const auto ks = c.payload["kind"].get<std::string>();
        if (ks == "app") kind = rmx::TrackKind::kApp;
        else if (ks == "fx") kind = rmx::TrackKind::kFx;
        else if (ks == "output") kind = rmx::TrackKind::kOutput;
        std::string name;
        if (c.payload.contains("name") && c.payload["name"].is_string())
            name = c.payload["name"].get<std::string>();
        std::uint32_t color = 0;
        if (c.payload.contains("color") && c.payload["color"].is_number_unsigned())
            color = c.payload["color"].get<std::uint32_t>();
        std::uint32_t track_id = 0;
        std::string err;
        if (g_engine.track_add(kind, name, color, track_id, err)) {
            after_mutation(client);
            ok(nlohmann::json{{"trackId", track_id}, {"tracks", tracks_json()}});
        } else {
            fail("bad_command", err);
        }
    } else if (c.kind == "track_remove") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string err;
        if (g_engine.track_remove(c.payload["trackId"].get<std::uint32_t>(), err)) {
            after_mutation(client);
            ok(nlohmann::json{{"tracks", tracks_json()}});
        } else {
            fail("track_not_found", err);
        }
    } else if (c.kind == "track_set") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        auto opt_str = [&](const char* k) -> std::optional<std::string> {
            return c.payload.contains(k) && c.payload[k].is_string()
                       ? std::optional<std::string>{c.payload[k].get<std::string>()}
                       : std::nullopt;
        };
        auto opt_u32 = [&](const char* k) -> std::optional<std::uint32_t> {
            return c.payload.contains(k) && c.payload[k].is_number_unsigned()
                       ? std::optional<std::uint32_t>{c.payload[k].get<std::uint32_t>()}
                       : std::nullopt;
        };
        auto opt_f32 = [&](const char* k) -> std::optional<float> {
            return c.payload.contains(k) && c.payload[k].is_number()
                       ? std::optional<float>{c.payload[k].get<float>()}
                       : std::nullopt;
        };
        auto opt_bool = [&](const char* k) -> std::optional<bool> {
            return c.payload.contains(k) && c.payload[k].is_boolean()
                       ? std::optional<bool>{c.payload[k].get<bool>()}
                       : std::nullopt;
        };
        std::string err;
        if (g_engine.track_set(c.payload["trackId"].get<std::uint32_t>(), opt_str("name"),
                               opt_u32("color"), opt_f32("gain"), opt_bool("mute"), err)) {
            after_mutation(client);
            ok(nlohmann::json{{"tracks", tracks_json()}});
        } else {
            fail("bad_command", err);
        }
    } else if (c.kind == "track_set_source" || c.kind == "track_set_output" ||
               c.kind == "track_set_dests") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const auto track_id = c.payload["trackId"].get<std::uint32_t>();
        std::string err, code("bad_command");
        bool okr = false;
        if (c.kind == "track_set_source") {
            rmx::TrackSource src;
            const auto& sj = c.payload["source"];
            if (!sj.is_null()) {
                const auto t = sj["type"].get<std::string>();
                if (t == "sine") {
                    src.type = rmx::TrackSource::kSine;
                    src.sine_freq = sj["freq"].get<float>();
                } else if (t == "asioIn") {
                    src.type = rmx::TrackSource::kAsioIn;
                    src.asio_in_ch = sj["channel"].get<std::uint32_t>();
                    if (sj.contains("mono") && sj["mono"].is_boolean())
                        src.mono = sj["mono"].get<bool>();
                } else if (t == "app") {
                    src.type = rmx::TrackSource::kApp;
                    src.pid = sj["pid"].get<std::uint32_t>();
                    if (sj.contains("name") && sj["name"].is_string())
                        src.app_name = sj["name"].get<std::string>();
                }
            }
            okr = g_engine.track_set_source(track_id, src, err, code);
        } else if (c.kind == "track_set_output") {
            rmx::TrackOutput out;
            const auto& oj = c.payload["output"];
            if (!oj.is_null()) {
                const auto t = oj["type"].get<std::string>();
                if (t == "asioOut") {
                    out.type = rmx::TrackOutput::kAsioOut;
                    out.asio_out_ch = oj["channel"].get<std::uint32_t>();
                } else if (t == "wasapi") {
                    out.type = rmx::TrackOutput::kWasapiRender;
                    out.wasapi_id = oj["deviceId"].get<std::string>();
                }
            }
            okr = g_engine.track_set_output(track_id, out, err, code);
        } else {
            std::vector<std::uint32_t> dests;
            for (const auto& d : c.payload["dests"])
                if (d.is_number_unsigned()) dests.push_back(d.get<std::uint32_t>());
            okr = g_engine.track_set_dests(track_id, std::move(dests), err, code);
        }
        if (okr) {
            after_mutation(client);
            ok(nlohmann::json{{"tracks", tracks_json()}});
        } else {
            fail(code.c_str(), err);
            // rebuild_asio_channels 失敗 = engine 可能已 stopped(或回滾):推權威 status
            push_status(client);
        }
    } else if (c.kind == "track_move") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string err;
        if (g_engine.track_move(c.payload["trackId"].get<std::uint32_t>(),
                                c.payload["newIndex"].get<std::size_t>(), err)) {
            after_mutation(client);
            ok(nlohmann::json{{"tracks", tracks_json()}});
        } else {
            fail("bad_command", err);
        }
    } else if (c.kind == "start_scan") {
        // 立即回 jobId;掃描在背景 thread(不佔 dispatch/editor thread)。
        // 同時一個 job:已在跑 = 共用現行 job(reused = true)
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
        std::uint64_t job_id = 0;
        bool reused = false;
        {
            std::lock_guard<std::mutex> lock(g_scan_mutex);
            if (g_scan_job.running.load(std::memory_order_acquire)) {
                job_id = g_scan_job.id;
                reused = true;
            } else {
                if (g_scan_job.thread.joinable()) g_scan_job.thread.join();  // 上輪已完,快收
                g_scan_job.cancel.store(false, std::memory_order_release);
                job_id = g_scan_next_id++;
                g_scan_job.id = job_id;
                g_scan_job.running.store(true, std::memory_order_release);
                g_scan_job.thread = std::thread(scan_job_thread, job_id, std::move(roots));
            }
        }
        ok(nlohmann::json{{"jobId", job_id}, {"reused", reused}});
    } else if (c.kind == "cancel_scan") {
        std::uint64_t job_id = 0;
        bool cancelling = false;
        {
            std::lock_guard<std::mutex> lock(g_scan_mutex);
            if (g_scan_job.running.load(std::memory_order_acquire)) {
                g_scan_job.cancel.store(true, std::memory_order_release);
                job_id = g_scan_job.id;
                cancelling = true;
            }
        }
        ok(nlohmann::json{{"jobId", job_id}, {"cancelling", cancelling}});
    } else if (c.kind == "add_plugin") {
        std::string class_id;
        if (c.payload.contains("classId") && c.payload["classId"].is_string())
            class_id = c.payload["classId"].get<std::string>();
        const std::string module_path = c.payload["path"].get<std::string>();
        // 載入前試爆:module 在 worker 驗(載入 + initialize + setActive)。壞 DLL
        // 的 heap 污染只發生在 worker process,engine 不接髒 module。worker 不在 =
        // fallback in-process(旧行為)。驗證率/塊 = 現行 start 值,未跑 = 常見預設
        {
            const auto st = g_engine.status();
            const double rate = st.running ? static_cast<double>(st.sample_rate) : 48000.0;
            const std::uint32_t block =
                st.running && st.buffer_size > 0 ? st.buffer_size : 512u;
            std::string verr;
            if (!rmx::sandbox::verify_module(module_path, class_id, rate, block, verr)) {
                fail("plugin_load_failed", verr);
                return false;
            }
        }
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::uint32_t instance_id = 0;
        std::string err;
        const auto track_id = c.payload["trackId"].get<std::uint32_t>();
        if (g_engine.add_plugin(track_id, module_path, class_id, instance_id, err)) {
            after_mutation(client);
            ok(nlohmann::json{{"instanceId", instance_id},
                              {"trackId", track_id},
                              {"tracks", tracks_json()}});
        } else {
            fail("plugin_load_failed", err);
        }
    } else if (c.kind == "remove_plugin") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string err;
        if (g_engine.remove_plugin(c.payload["instanceId"].get<std::uint32_t>(), err)) {
            after_mutation(client);
            ok(nlohmann::json{{"tracks", tracks_json()}});
        } else {
            fail("plugin_not_found", err);
        }
    } else if (c.kind == "move_plugin") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string err;
        if (g_engine.move_plugin(c.payload["instanceId"].get<std::uint32_t>(),
                                 c.payload["newIndex"].get<std::size_t>(), err)) {
            after_mutation(client);
            ok(nlohmann::json{{"tracks", tracks_json()}});
        } else {
            fail("bad_command", err);
        }
    } else if (c.kind == "set_bypass") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        std::string err;
        if (g_engine.set_bypass(c.payload["instanceId"].get<std::uint32_t>(),
                                c.payload["bypassed"].get<bool>(), err)) {
            after_mutation(client);
            ok(nlohmann::json{{"tracks", tracks_json()}});
        } else {
            fail("plugin_not_found", err);
        }
    } else if (c.kind == "retry_plugin") {
        // placeholder → 真 plugin。與 add_plugin 同規:先 worker sandbox preflight
        // (fail closed:worker 不在不 in-process 試爆),過了才在 engine 內原位載回
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const auto instance = c.payload["instanceId"].get<std::uint32_t>();
        std::string path_override;
        if (c.payload.contains("path") && c.payload["path"].is_string())
            path_override = c.payload["path"].get<std::string>();
        const auto* slot = g_engine.find_slot(instance);
        if (slot == nullptr) {
            fail("plugin_not_found", "unknown instanceId");
        } else if (!slot->is_placeholder()) {
            fail("bad_command", "instance is not a placeholder");
        } else {
            const std::string& module_path =
                path_override.empty() ? slot->module_path : path_override;
            const std::string class_id = slot->class_id;
            const auto st = g_engine.status();
            const double rate =
                st.running ? static_cast<double>(st.sample_rate) : 48000.0;
            const std::uint32_t block =
                st.running && st.buffer_size > 0 ? st.buffer_size : 512u;
            std::string verr;
            if (rmx::sandbox::worker_path().empty()) {
                fail("plugin_load_failed",
                     "sandbox worker unavailable (roudamix-worker.exe missing)");
            } else if (!rmx::sandbox::verify_module(module_path, class_id, rate, block, verr)) {
                fail("plugin_load_failed", verr);
            } else {
                std::string err;
                if (g_engine.load_placeholder(instance, module_path, class_id, err)) {
                    after_mutation(client);
                    ok(nlohmann::json{{"instanceId", instance}, {"tracks", tracks_json()}});
                } else {
                    fail("plugin_load_failed", err);
                }
            }
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
        } else if (slot->plugin == nullptr) {
            // placeholder:無 metadata,回空表(UI 顯示遺失狀態即可)
            ok(nlohmann::json{{"instanceId", slot->instance_id},
                              {"params", nlohmann::json::array()}});
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
    } else if (c.kind == "open_editor") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const auto instance = c.payload["instanceId"].get<std::uint32_t>();
        const auto* slot = g_engine.find_slot(instance);
        std::string err;
        if (slot == nullptr) {
            fail("plugin_not_found", "unknown instanceId");
        } else if (slot->plugin == nullptr) {
            fail("plugin_no_editor", "plugin not loaded (placeholder)");
        } else if (rmx::EditorHost::instance().open(instance, err)) {
            ok(nlohmann::json{{"instanceId", instance}, {"editor", true}});
        } else {
            fail("plugin_no_editor", err);
        }
    } else if (c.kind == "close_editor") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const auto* slot = g_engine.find_slot(c.payload["instanceId"].get<std::uint32_t>());
        if (slot == nullptr) {
            fail("plugin_not_found", "unknown instanceId");
        } else {
            rmx::EditorHost::instance().close(slot->instance_id);
            ok(nlohmann::json::object());
        }
    } else if (c.kind == "save_preset" || c.kind == "load_preset") {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        const auto instance = c.payload["instanceId"].get<std::uint32_t>();
        if (g_engine.find_slot(instance) == nullptr) {
            fail("plugin_not_found", "unknown instanceId");
        } else {
            const auto file = std::filesystem::path(c.payload["path"].get<std::string>());
            std::string err;
            if (c.kind == "save_preset") {
                if (g_engine.save_preset(instance, file, err))
                    ok(nlohmann::json{{"savedPath", c.payload["path"].get<std::string>()}});
                else
                    fail("preset_io", err);
            } else if (g_engine.load_preset(instance, file, err)) {
                after_mutation(client);
                ok(nlohmann::json{{"tracks", tracks_json()}});
            } else {
                // 檔案開不了/格式壞 = preset_io;plugin 拒套 state = plugin_state_failed
                const bool bad_state = err.find("rejected preset") != std::string::npos;
                fail(bad_state ? "plugin_state_failed" : "preset_io", err);
            }
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
            // revision 隨回:UI 以此定 clean 基準(非 mutation,值 = 現值)
            ok(nlohmann::json{{"savedPath", file.string()},
                              {"revision", g_engine.revision()}});
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
            applied["revision"] = g_engine.revision();  // UI 以此定 clean 基準(load 後不誤標 dirty)
            ok(applied);  // 含 deviceKey/sampleRate/bufferSize + missing[](diagnostics)
        } else {
            fail("session_io", err);
        }
    } else if (c.kind == "ensure_system_outputs") {
        // 新 session/缺少系統輸出時補回(monitor/stream 恰好各一);沒得補 = no-op
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        if (g_engine.ensure_system_outputs()) {
            after_mutation(client);
        }
        ok(nlohmann::json{{"tracks", tracks_json()}, {"revision", g_engine.revision()}});
    } else if (c.kind == "set_editor_owner") {
        // UI 主視窗 HWND:editor host 掛成 owned 浮動視窗(無工作列項、隨主程式)
        rmx::EditorHost::instance().set_owner(
            reinterpret_cast<HWND>(c.payload["hwnd"].get<std::uint64_t>()));
        ok(nlohmann::json::object());
    } else if (c.kind == "shutdown_engine") {
        ok(nlohmann::json::object());
        return true;
    } else {
        fail("internal", c.kind + " not implemented yet (M2)");
    }
    return false;
}

// C++ 例外層:dispatch 內任何 throw 都必須回 error reply,client 才不會乾等 hang 死
bool dispatch_guarded(HANDLE client, const Command& c) {
    try {
        return dispatch(client, c);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[engine] exception while dispatching %s: %s\n", c.kind.c_str(),
                     e.what());
        send_frame(client, rmx::make_reply_err(c.id, g_epoch.load(), "internal", e.what()));
    } catch (...) {
        std::fprintf(stderr, "[engine] unknown exception while dispatching %s\n", c.kind.c_str());
        send_frame(client,
                   rmx::make_reply_err(c.id, g_epoch.load(), "internal", "unknown exception"));
    }
    return false;
}

// SEH 包裹層(含 C++ 物件的函式不能直接用 __try;guarded 拆另一函式)
bool dispatch_seh(HANDLE client, const Command& c) {
    bool shutdown = false;
    __try {
        shutdown = dispatch_guarded(client, c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "[engine] SEH exception while dispatching %s: %lX\n",
                     c.kind.c_str(), GetExceptionCode());
    }
    return shutdown;
}

LRESULT CALLBACK main_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    if (msg == WM_APP_TASK) {
        auto* task = reinterpret_cast<Task*>(lp);
        if (task != nullptr) {
            // 舊連線的積壓 task:client 已斷、handle 已 Close(值可能被新連線重用)
            // —— 丟棄不 dispatch。dispatch 執行中斷線的窄窗口由 write 的 bounded
            // timeout 兜底(最壞 = 一個壞幀寫進新連線,client 端容錯跳過)
            if (task->gen != g_conn_gen.load(std::memory_order_acquire)) {
                delete task;
                return 0;
            }
            if (dispatch_seh(task->client, task->cmd))
                PostMessageW(h, WM_APP_QUIT, 0, 0);
            delete task;
        }
        return 0;
    }
    if (msg == rmx::WM_APP_HOSTCMD) {
        auto* cmd = reinterpret_cast<rmx::EditorHostCmd*>(lp);
        if (cmd != nullptr) {
            handle_host_cmd(*cmd);
            delete cmd;
        }
        return 0;
    }
    if (msg == WM_APP_CAPTURE_ERR) {
        // capture/render pump 偵錯(pump thread → callback → PostMessage):
        // 標軌 error、收 pump、廣播 status(UI 顯示軌道出錯)
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        g_engine.handle_track_failed(static_cast<std::uint32_t>(wp));
        if (g_active_pipe.load() != nullptr) after_mutation(g_active_pipe.load());
        return 0;
    }
    if (msg == WM_APP_QUIT) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 單一 client 生命週期:推 snapshot + 讀幀轉 dispatch。
// 推 snapshot 必須持 g_engine_mutex:rack_ 由 main thread 改(erase/push_back),
// pipe thread 無鎖並行迭代 vector = UB(曾炸 SEH C0000005)。鎖序 engine→write
// 與 dispatch 各分支一致,無死鎖。
void serve_client(HANDLE pipe, uint64_t gen) {
    g_last_activity.store(GetTickCount64());
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        send_frame(pipe, rmx::make_event("snapshot", snapshot_payload()));
    }
    for (;;) {
        std::optional<std::vector<uint8_t>> frame;
        try {
            frame = rmx::read_frame(pipe);
        } catch (const rmx::FrameIoError& e) {
            std::fprintf(stderr, "[engine] bad frame: %s\n", e.what());
            break;
        }
        if (!frame) break;  // EOF/broken pipe
        g_last_activity.store(GetTickCount64());
        nlohmann::json j = nlohmann::json::parse(*frame, nullptr, false);
        if (j.is_discarded()) {
            std::fprintf(stderr, "[engine] invalid JSON payload\n");
            break;
        }
        try {
            Frame f = rmx::parse_frame(j, /*strict=*/false);
            if (auto* cmd = std::get_if<Command>(&f)) {
                auto* task = new Task{gen, pipe, std::move(*cmd)};
                if (!PostMessageW(g_main_hwnd, WM_APP_TASK, 0,
                                  reinterpret_cast<LPARAM>(task))) {
                    delete task;  // main window 已不在:engine 正在退場
                    break;
                }
            }
            // server 收到 reply/event:契約上不該出現,忽略(容錯不斷線)
        } catch (const rmx::ParseError& e) {
            send_frame(pipe, rmx::make_reply_err(0, g_epoch.load(), "bad_command",
                                                 e.what()));
        }
    }
}

// pipe worker:accept → serve → 斷線回 accept 續跑。單 client 炸(SEH)不死
// thread —— 攔下、收 client、繼續 accept。
void pipe_serve_inner() {
    while (!g_exiting.load()) {
        HANDLE pipe = CreateNamedPipeW(
            L"\\\\.\\pipe\\roudamix-engine",
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "[engine] CreateNamedPipeW failed: %lu\n", GetLastError());
            break;
        }
        g_active_pipe.store(pipe);
        // overlapped accept:分段等,shutdown(idle watchdog/退出)醒得來
        OVERLAPPED conn{};
        conn.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = FALSE;
        if (ConnectNamedPipe(pipe, &conn) ||
            GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        } else if (GetLastError() == ERROR_IO_PENDING && conn.hEvent != nullptr) {
            while (!g_exiting.load()) {
                if (WaitForSingleObject(conn.hEvent, 500) == WAIT_OBJECT_0) {
                    DWORD dummy = 0;
                    if (GetOverlappedResult(pipe, &conn, &dummy, FALSE)) connected = TRUE;
                    break;
                }
            }
        }
        if (conn.hEvent != nullptr) CloseHandle(conn.hEvent);
        if (!connected) {
            g_active_pipe.store(nullptr);
            CloseHandle(pipe);
            continue;
        }
        std::printf("[engine] client connected\n");
        const uint64_t gen = g_conn_gen.fetch_add(1) + 1;
        // SEH 只包 serve_client:client 端壞輸入/plugin 炸不帶走整個 thread,
        // 收掉這個 client 繼續 accept(callee 內 C++ 物件不 unwind,leak 一次可接受)
        __try {
            serve_client(pipe, gen);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::fprintf(stderr, "[engine] SEH while serving client: %lX\n",
                         GetExceptionCode());
        }
        // 不 FlushFileBuffers:Flush 會等 client 讀完所有 server 寫的 byte ——
        // client 斷線不讀 = 永卡(pipe thread 卡死無法 accept)
        g_active_pipe.store(nullptr);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        g_last_activity.store(GetTickCount64());
        std::printf("[engine] client disconnected\n");
    }
}

void pipe_serve_thread() {
    pipe_serve_inner();
}

// idle watchdog:12h 無 client 活動 → 取消 pipe thread 的 blocking IO 讓它退出
void idle_watchdog() {
    constexpr ULONGLONG kIdleTimeoutMs = 12ull * 60 * 60 * 1000;  // 12h
    for (;;) {
        if (g_exiting.load()) return;
        // 分段睡:shutdown 時 join 不必等滿 60s
        for (int i = 0; i < 240 && !g_exiting.load(); ++i) Sleep(250);
        if (g_exiting.load()) return;
        if (GetTickCount64() - g_last_activity.load() > kIdleTimeoutMs) {
            if (HANDLE h = g_active_pipe.load(); h != nullptr) CancelIoEx(h, nullptr);
            PostMessageW(g_main_hwnd, WM_APP_QUIT, 0, 0);
            return;
        }
    }
}

}  // namespace

int main() {
    HANDLE singleton = CreateMutexW(nullptr, TRUE, rmx::kSingletonMutex);
    if (singleton == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "[engine] another instance already running, exiting\n");
        return 0;
    }

    // main thread = UI thread:VST3 editor(createView/attached/removed)與 plugin
    // GUI 訊息全部由本 thread 的 message loop 服務(STA 是 Win32 GUI 標配)
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::fprintf(stderr, "[engine] CoInitializeEx failed\n");
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &main_wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMainWndClass;
    RegisterClassExW(&wc);
    // message-only window:不進 z-order / 不收廣播,只收 PostMessage 的 task
    g_main_hwnd = CreateWindowExW(0, kMainWndClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                  nullptr, wc.hInstance, nullptr);
    std::fprintf(stderr, "[engine] main window hwnd=%p\n", static_cast<void*>(g_main_hwnd));

    rmx::EditorHost::instance().set_engine(&g_engine);
    rmx::EditorHost::instance().set_command_target(g_main_hwnd);
    // M5b/M5c:capture/render pump 偵錯 → main thread 排隊(pump thread 禁碰 pipe/鎖)
    g_engine.set_capture_failed_cb([](std::uint32_t track_id) {
        PostMessageW(g_main_hwnd, WM_APP_CAPTURE_ERR, track_id, 0);
    });

    std::thread pipe_thread(pipe_serve_thread);
    std::thread watchdog(idle_watchdog);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_exiting.store(true);
    // 掃描 job 收工(cancel 旗標讓 worker wait 100ms 內退出;thread 收完 registry
    // 才離開)。此時 message loop 已停,不會有並發的 start_scan,直接 join
    g_scan_job.cancel.store(true, std::memory_order_release);
    if (g_scan_job.thread.joinable()) g_scan_job.thread.join();
    // shutdown_engine 的 ack 已寫進 pipe buffer,但 client 可能還沒讀 —— process
    // 關 pipe handle 會連未讀資料一起丟(client 收 EOF 而非 ack)。短等它收走。
    Sleep(150);
    if (HANDLE h = g_active_pipe.load(); h != nullptr)
        CancelIoEx(h, nullptr);  // 喚醒 overlapped accept/read
    if (pipe_thread.joinable()) pipe_thread.join();
    if (watchdog.joinable()) watchdog.join();
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        rmx::EditorHost::instance().shutdown();  // detach view:plugins 活著時拆,避開解構順序
        g_engine.stop();
    }
    if (g_main_hwnd != nullptr) DestroyWindow(g_main_hwnd);
    CoUninitialize();
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return 0;
}
