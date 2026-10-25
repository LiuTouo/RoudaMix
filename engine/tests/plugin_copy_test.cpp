#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "audio_engine.hpp"
#include "command_contract.hpp"
#include "router.hpp"
#include "session.hpp"
#include "session_projection.hpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(EXIT_FAILURE); } } while (0)

static void engine_copy(const std::string& module) {
    rmx::AudioEngine engine;
    std::uint32_t source{}, target{}, original{}, first{}, second{}, output{};
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "Source", 0, source));
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "Target", 0, target));
    CHECK(!engine.track_add(rmx::TrackKind::kOutput, "Monitor", 0, output));
    CHECK(!engine.track_set_dests(target, {output}));
    CHECK(!engine.add_plugin(source, module, "", original));
    CHECK(!engine.set_param(original, 100, 128.0 / 192000.0));
    CHECK(!engine.set_param(original, 101, 0.125));
    CHECK(!engine.set_bypass(original, true));
    CHECK(!engine.set_monitor_bypass(original, true));
    rmx::PluginSnapshot snapshot;
    CHECK(!engine.capture_plugin(original, snapshot));
    CHECK(snapshot.bypass && snapshot.monitor_bypass);
    CHECK(!snapshot.state.component.empty() && !snapshot.state.controller.empty());
    const auto saved_component = snapshot.state.component;
    CHECK(!engine.insert_plugin_snapshot(snapshot, target, 0, first));
    CHECK(first != original);
    const auto* a = engine.find_slot(original);
    const auto* b = engine.find_slot(first);
    CHECK(a && b && a->plugin != b->plugin && a->ring != b->ring);
    CHECK(a->monitor_ring != b->monitor_ring && a->latency_change_mailbox != b->latency_change_mailbox);
    CHECK(b->param_values == snapshot.params && b->bypass && b->monitor_bypass);
    CHECK(b->plugin->latency_samples() == 128);
    rmx::PluginSnapshot copied;
    CHECK(!engine.capture_plugin(first, copied));
    std::uint32_t source_opaque{}, copied_opaque{};
    std::memcpy(&source_opaque, snapshot.state.component.data() + snapshot.state.component.size() - 4, 4);
    std::memcpy(&copied_opaque, copied.state.component.data() + copied.state.component.size() - 4, 4);
    CHECK(source_opaque != 0 && source_opaque == copied_opaque);
    CHECK(std::abs(b->plugin->param_value(101) - 0.125) < 1e-8);
    CHECK(!engine.set_param(original, 101, 0.75));
    CHECK(std::abs(b->plugin->param_value(101) - 0.125) < 1e-8);
    CHECK(!engine.remove_plugin(original));
    CHECK(!engine.insert_plugin_snapshot(snapshot, target, 0, second));
    CHECK(first != second && engine.tracks()[1].chain[0].instance_id == second);
    CHECK(engine.tracks()[1].chain[1].instance_id == first);
    CHECK(snapshot.state.component == saved_component);
    CHECK(!engine.set_param(second, 101, 0.5));
    CHECK(std::abs(engine.find_slot(first)->plugin->param_value(101) - 0.125) < 1e-8);

    std::uint32_t unused{};
    CHECK(engine.insert_plugin_snapshot(snapshot, 99999, 0, unused)->code == rmx::Err::kTrackNotFound);
    CHECK(engine.insert_plugin_snapshot(snapshot, target, 3, unused)->code == rmx::Err::kBadCommand);
    auto broken = snapshot;
    broken.state.component.clear();
    const auto rejected_state = engine.insert_plugin_snapshot(broken, target, 1, unused);
    CHECK(rejected_state && rejected_state->code == rmx::Err::kPluginStateFailed);
    broken = snapshot; broken.module_path += ".missing";
    CHECK(engine.insert_plugin_snapshot(broken, target, 1, unused)->code == rmx::Err::kPluginLoadFailed);
    // 4 秒 latency 超出 2 秒路由上限，候選鏈失敗後原始順序仍完整。
    broken = snapshot; broken.bypass = false; broken.monitor_bypass = false;
    broken.params[0].second = 1.0;
    const auto rejected_latency = engine.insert_plugin_snapshot(broken, target, 1, unused);
    CHECK(rejected_latency && rejected_latency->code == rmx::Err::kPluginStateFailed);
    CHECK(engine.tracks()[1].chain.size() == 2);
    CHECK(engine.tracks()[1].chain[0].instance_id == second);
    CHECK(engine.tracks()[1].chain[1].instance_id == first);

    std::uint32_t placeholder{};
    CHECK(!engine.add_placeholder_plugin(source, "missing.vst3", "", "Missing", false,
        rmx::RackSlot::Availability::kMissing, "missing", {}, placeholder));
    CHECK(engine.capture_plugin(placeholder, snapshot)->code == rmx::Err::kPluginStateFailed);
    CHECK(snapshot.state.component == saved_component);
    CHECK(engine.capture_plugin(99999, snapshot)->code == rmx::Err::kPluginNotFound);

    // reader 故意保留舊圖，擷取必須逾時而不是與仍在飛行的 process 同時讀 state。
    const auto* held = engine.acquire_graph(); CHECK(held);
    const auto before = std::chrono::steady_clock::now();
    CHECK(engine.capture_plugin(first, snapshot)->code == rmx::Err::kPluginStateFailed);
    CHECK(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(1900));
    engine.release_graph();
    const auto* restored = engine.acquire_graph();
    CHECK(restored && restored->nodes[1].chain.size() == 2);
    CHECK(restored->nodes[1].chain[0].instance_id == second);
    engine.release_graph();
    CHECK(!engine.capture_plugin(first, snapshot));

    held = engine.acquire_graph(); CHECK(held);
    std::thread reader([&] { Sleep(30); engine.release_graph(); });
    CHECK(!engine.capture_plugin(first, snapshot));
    reader.join();

    // 監聽分岔採獨立 shadow，拷貝至有 low-latency 輸出的鏈仍可重建。
    std::uint32_t split{}, after{};
    CHECK(!engine.track_set_output_latency_policy(output, rmx::OutputLatencyPolicy::kLowLatency));
    CHECK(!engine.track_set_dests(target, {output}));
    auto split_snapshot = snapshot;
    split_snapshot.bypass = false; split_snapshot.monitor_bypass = true;
    CHECK(!engine.insert_plugin_snapshot(split_snapshot, target, 0, split));
    split_snapshot.monitor_bypass = false;
    CHECK(!engine.insert_plugin_snapshot(split_snapshot, target, 1, after));
    CHECK(engine.find_slot(after)->monitor_shadow);
    CHECK(engine.find_slot(after)->monitor_shadow != engine.find_slot(after)->plugin);
    CHECK(engine.find_slot(after)->monitor_shadow->latency_samples() == 128);
}

static void router_copy(const std::string& module) {
    rmx::Router router;
    std::vector<nlohmann::json> frames;
    std::uint64_t id{};
    router.connect(1, [&](const auto& frame) { frames.push_back(frame); });
    const auto capabilities = frames[0]["payload"]["capabilities"].get<std::vector<std::string>>();
    CHECK(std::find(capabilities.begin(), capabilities.end(), "pluginCopyV1") != capabilities.end());
    CHECK(std::find(capabilities.begin(), capabilities.end(), "trackDuplicateV1") != capabilities.end());
    auto send = [&](const std::string& kind, nlohmann::json payload) {
        frames.clear();
        CHECK(!router.dispatch_guarded(1, {rmx::kProtocolVersion, ++id, kind, std::move(payload)}));
        for (const auto& frame : frames) {
            if (!frame.contains("id") || frame["id"] != id) continue;
            if (frame["ok"].get<bool>()) rmx::contract::validate_result(kind, frame["result"]);
            return frame;
        }
        CHECK(false); return nlohmann::json{};
    };
    auto ok = [&](const std::string& kind, nlohmann::json payload) {
        auto reply = send(kind, std::move(payload));
        if (!reply["ok"].get<bool>()) std::fprintf(stderr, "%s: %s\n", kind.c_str(), reply.dump().c_str());
        CHECK(reply["ok"].get<bool>()); return reply["result"];
    };
    const auto source = ok("track_add", {{"kind", "audio"}, {"name", "Source"}})["trackId"];
    const auto target = ok("track_add", {{"kind", "audio"}, {"name", "Target"}})["trackId"];
    const auto original = ok("add_plugin", {{"trackId", source}, {"path", module}})["instanceId"];
    ok("set_param", {{"instanceId", original}, {"paramId", 101}, {"value", 0.125}});
    const auto revision = ok("get_snapshot", nlohmann::json::object())["snapshot"]["status"]["revision"].get<std::uint64_t>();
    const auto clip = ok("copy_plugin", {{"instanceId", original}})["clipboardId"];
    CHECK(frames.size() == 1);
    CHECK(ok("get_snapshot", nlohmann::json::object())["snapshot"]["status"]["revision"] == revision);
    ok("set_param", {{"instanceId", original}, {"paramId", 101}, {"value", 0.75}});
    const auto duplicate = ok("duplicate_plugin", {{"instanceId", original}, {"trackId", target}, {"newIndex", 0}})["instanceId"];
    ok("remove_plugin", {{"instanceId", original}});
    const auto before_paste = ok("get_snapshot", nlohmann::json::object())["snapshot"]["status"]["revision"].get<std::uint64_t>();
    const auto pasted = ok("paste_plugin", {{"clipboardId", clip}, {"trackId", target}, {"newIndex", 0}});
    CHECK(pasted["instanceId"] != duplicate);
    const auto after_paste = ok("get_snapshot", nlohmann::json::object())["snapshot"];
    CHECK(after_paste["status"]["revision"] == before_paste + 1);
    const auto& chain = after_paste["tracks"][1]["plugins"];
    CHECK(chain.size() == 2);
    CHECK(chain[0]["params"][1]["normalized"] == 0.125);
    CHECK(chain[1]["params"][1]["normalized"] == 0.75);
    // 錯誤 copy 不破壞剪貼簿；重複 paste 繼續使用點選複製時的快照。
    CHECK(!send("copy_plugin", {{"instanceId", original}})["ok"].get<bool>());
    ok("paste_plugin", {{"clipboardId", clip}, {"trackId", target}, {"newIndex", 2}});

    const auto folder = std::filesystem::temp_directory_path() /
        ("roudamix-copy-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    const auto session = (folder / "copy.rmsession").string();
    ok("save_session", {{"path", session}});
    ok("load_session", {{"path", session}});
    CHECK(!send("paste_plugin", {{"clipboardId", clip}, {"trackId", target}, {"newIndex", 0}})["ok"].get<bool>());
    const auto loaded = ok("get_snapshot", nlohmann::json::object())["snapshot"];
    bool found = false;
    nlohmann::json reloaded_id;
    for (const auto& track : loaded["tracks"]) {
        if (track["name"] != "Target") continue;
        const auto& plugins = track["plugins"];
        CHECK(plugins.size() == 3);
        CHECK(plugins[0]["availability"] == "ok");
        CHECK(plugins[0]["params"][1]["normalized"] == 0.125);
        CHECK(plugins[1]["params"][1]["normalized"] == 0.75);
        reloaded_id = plugins[0]["instanceId"];
        found = true;
    }
    CHECK(found);
    const auto new_clip = ok("copy_plugin", {{"instanceId", reloaded_id}})["clipboardId"];
    router.connect(1, [&](const auto& frame) { frames.push_back(frame); });
    CHECK(!send("paste_plugin", {{"clipboardId", new_clip}, {"trackId", target}, {"newIndex", 0}})["ok"].get<bool>());
    std::filesystem::remove_all(folder);
}

// 使用既有 fixture 與真正的 AudioEngine callback，避免只驗 planner 的資料形狀。
static void sidechain_audio(const std::string& module) {
    rmx::AudioEngine engine;
    std::uint32_t source{}, fx{}, sink{}, instance{};
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "Source", 0, source));
    CHECK(!engine.track_add(rmx::TrackKind::kFx, "Sidechain FX", 0, fx));
    CHECK(!engine.track_add(rmx::TrackKind::kOutput, "Sink", 0, sink));
    rmx::TrackSource tone;
    tone.type = rmx::TrackSource::kSine;
    CHECK(!engine.track_set_source(source, tone));
    CHECK(!engine.track_set_dests(fx, {sink}));
    CHECK(!engine.add_plugin(fx, module, "", instance));
    CHECK(engine.find_slot(instance)->plugin->initialize(48000.0, 64));
    CHECK(engine.find_slot(instance)->plugin->sidechain_capable());
    CHECK(!engine.track_set_sidechain(fx, {source}));

    // 不啟動硬體：rate=0 時相位不前進，四分之一週期固定產生 0.25。
    const auto input = engine.tracks()[0].buf;
    const auto effect = engine.tracks()[1].buf;
    const auto output = engine.tracks()[2].buf;
    input->sine_phase = 1ULL << 30;
    auto process = [&] {
        static_cast<rmx::IAudioCallback&>(engine).process({64, {}, {}});
        CHECK(engine.status().plugin_fails == 0);
    };
    process();
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < 64; ++i) {
            CHECK(input->sc[ch][i] == 0.0F);
            CHECK(effect->in[ch][i] == 0.0F);  // aux 不直接加入可聽 main bus
            CHECK(std::abs(effect->sc[ch][i] - 0.25F) < 1e-6F);
            CHECK(std::abs(output->in[ch][i] - 0.25F) < 1e-6F); // fixture 將 aux 加進輸出
        }
    }
    const auto tracks = rmx::session::tracks_json(engine);
    CHECK(tracks[0]["sidechain"].empty());
    CHECK(tracks[1]["sidechain"] == nlohmann::json::array({source}));

    // main 與 aux 同向並存，fixture 各收一次；清空 aux 後不可殘留上一塊。
    CHECK(!engine.track_set_dests(source, {fx}));
    process();
    CHECK(std::abs(output->in[0][0] - 0.5F) < 1e-6F);
    CHECK(!engine.track_set_sidechain(fx, {}));
    process();
    CHECK(effect->sc[0][0] == 0.0F);
    CHECK(std::abs(output->in[0][0] - 0.25F) < 1e-6F);
    CHECK(!engine.track_set_dests(source, {}));
    process();
    CHECK(output->in[0][0] == 0.0F);
}

static void sidechain_rollback(const std::string& module) {
    rmx::AudioEngine engine;
    std::uint32_t source{}, spare{}, fx{}, other_fx{}, output{}, instance{};
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "Slow source", 0, source));
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "Spare source", 0, spare));
    CHECK(!engine.track_add(rmx::TrackKind::kFx, "Slow FX", 0, fx));
    CHECK(!engine.track_add(rmx::TrackKind::kFx, "Other FX", 0, other_fx));
    CHECK(!engine.track_add(rmx::TrackKind::kOutput, "Output", 0, output));
    CHECK(!engine.track_set_dests(fx, {output}));

    rmx::Vst3Plugin fixture(module, "");
    CHECK(fixture.loaded());
    rmx::PluginSnapshot slow;
    slow.module_path = module;
    slow.params = {{100, 60000.0 / 192000.0}};
    std::string error;
    CHECK(fixture.capture_runtime_state(slow.state, error));
    CHECK(!engine.insert_plugin_snapshot(slow, source, 0, instance));
    CHECK(!engine.insert_plugin_snapshot(slow, fx, 0, instance));
    CHECK(!engine.track_set_sidechain(other_fx, {source}));
    CHECK(!engine.track_set_sidechain(fx, {spare}));
    const auto before = rmx::session::serialize(engine);
    const auto before_status = rmx::session::tracks_json(engine);

    // 兩段 60000 samples 接起來超過 48000 Hz 的兩秒上限。
    // 這是在候選邊已寫入後失敗，需還原所有來源，不能只還原 FX 本身。
    const auto fail = engine.track_set_sidechain(fx, {source});
    CHECK(fail && fail->code == rmx::Err::kPluginStateFailed);
    CHECK(rmx::session::serialize(engine) == before);
    CHECK(rmx::session::tracks_json(engine) == before_status);
    const auto* graph = engine.acquire_graph();
    CHECK(graph && graph->route_plan.ok());
    CHECK((graph->nodes[0].sidechain_dests == std::vector<std::uint32_t>{other_fx}));
    CHECK((graph->nodes[1].sidechain_dests == std::vector<std::uint32_t>{fx}));
    engine.release_graph();
}

// duplicate_track:整軌複製(設定 + 插件鏈 + placeholder 保留)、原軌正下方插入、
// 後綴不疊加、系統輸出軌拒絕、未知 id、模組消失 = 整軌回滾零殘留
static void router_duplicate_track(const std::string& module) {
    rmx::Router router;
    std::vector<nlohmann::json> frames;
    std::uint64_t id{};
    router.connect(1, [&](const auto& frame) { frames.push_back(frame); });
    auto send = [&](const std::string& kind, nlohmann::json payload) {
        frames.clear();
        CHECK(!router.dispatch_guarded(1, {rmx::kProtocolVersion, ++id, kind, std::move(payload)}));
        for (const auto& frame : frames) {
            if (!frame.contains("id") || frame["id"] != id) continue;
            if (frame["ok"].get<bool>()) rmx::contract::validate_result(kind, frame["result"]);
            return frame;
        }
        CHECK(false); return nlohmann::json{};
    };
    auto ok = [&](const std::string& kind, nlohmann::json payload) {
        auto reply = send(kind, std::move(payload));
        if (!reply["ok"].get<bool>()) std::fprintf(stderr, "%s: %s\n", kind.c_str(), reply.dump().c_str());
        CHECK(reply["ok"].get<bool>()); return reply["result"];
    };
    auto snapshot_tracks = [&]() {
        return ok("get_snapshot", nlohmann::json::object())["snapshot"]["tracks"];
    };

    const auto guitar = ok("track_add", {{"kind", "audio"}, {"name", "吉他"}, {"color", 0x4da3ff}})["trackId"].get<std::uint32_t>();
    const auto fx = ok("track_add", {{"kind", "fx"}, {"name", "FX"}})["trackId"].get<std::uint32_t>();
    const auto sink = ok("track_add", {{"kind", "output"}, {"name", "Out"}})["trackId"].get<std::uint32_t>();
    ok("track_set_source", {{"trackId", guitar}, {"source", {{"type", "sine"}, {"freq", 220}}}});
    ok("track_set_dests", {{"trackId", guitar}, {"dests", {fx, sink}}});
    ok("track_set_dests", {{"trackId", fx}, {"dests", {sink}}});
    ok("track_set_sidechain", {{"trackId", fx}, {"sources", {guitar}}});
    ok("track_set", {{"trackId", guitar}, {"gain", 1.5}, {"mute", true}});
    // 註:asioOut sink 在無 ASIO 裝置的測試環境回 ok 但不持久(既有引擎行為),
    // 故 output 副本斷言用 null;latency policy 與軌種仍可驗
    ok("track_set_output_latency_policy", {{"trackId", sink}, {"policy", "lowLatency"}});
    const auto plug = ok("add_plugin", {{"trackId", fx}, {"path", module}})["instanceId"].get<std::uint32_t>();
    ok("set_param", {{"instanceId", plug}, {"paramId", 101}, {"value", 0.25}});
    ok("set_bypass", {{"instanceId", plug}, {"bypassed", true}});
    ok("set_plugin_name", {{"instanceId", plug}, {"name", "我的效果"}});

    // 一般輌完整複製 + 插入原軌正下方
    const auto copy_id = ok("duplicate_track", {{"trackId", guitar}})["trackId"].get<std::uint32_t>();
    CHECK(copy_id != guitar);
    auto tracks = snapshot_tracks();
    CHECK(tracks.size() == 4);
    CHECK(tracks[1]["trackId"] == copy_id);
    const auto& copy = tracks[1];
    CHECK(copy["name"] == "吉他 複製");
    CHECK(copy["kind"] == "audio");
    CHECK(copy["color"] == 0x4da3ff);
    CHECK(copy["gain"] == 1.5);
    CHECK(copy["mute"] == true);
    CHECK(copy["source"]["type"] == "sine");
    CHECK(copy["source"]["freq"] == 220);
    CHECK((copy["dests"] == nlohmann::json::array({fx, sink})));
    CHECK(copy["plugins"].empty());
    CHECK(tracks[0]["name"] == "吉他");
    CHECK(tracks[0]["dests"].size() == 2);  // 原軌不受影響
    // 側鏈:fx 同時收原軌與副本(來源邊指向兩個不同 id)
    CHECK((tracks[2]["sidechain"] == nlohmann::json::array({guitar, copy_id})));

    // 輸出軌(output + latency policy)與插件鏈(參數/bypass/自訂名)複製
    ok("duplicate_track", {{"trackId", sink}});
    ok("duplicate_track", {{"trackId", fx}});
    tracks = snapshot_tracks();
    CHECK(tracks.size() == 6);  // [吉他, 吉他複製, FX, FX複製, Out, Out複製]
    const auto& fx_copy = tracks[3];
    CHECK(fx_copy["name"] == "FX 複製");
    CHECK(fx_copy["kind"] == "fx");
    CHECK((fx_copy["dests"] == nlohmann::json::array({sink})));
    CHECK((fx_copy["sidechain"] == nlohmann::json::array()));  // 來源邊不自動複製
    CHECK(fx_copy["plugins"].size() == 1);
    CHECK(fx_copy["plugins"][0]["instanceId"] != plug);
    CHECK(fx_copy["plugins"][0]["name"] == "我的效果");
    CHECK(fx_copy["plugins"][0]["displayName"] == "我的效果");
    CHECK(fx_copy["plugins"][0]["bypassed"] == true);
    CHECK(fx_copy["plugins"][0]["availability"] == "ok");
    double copied_value = -1;
    for (const auto& p : fx_copy["plugins"][0]["params"])
        if (p["paramId"] == 101) copied_value = p["normalized"].get<double>();
    CHECK(std::abs(copied_value - 0.25) < 1e-9);
    const auto& out_copy = tracks[5];
    CHECK(out_copy["name"] == "Out 複製");
    CHECK(out_copy["kind"] == "output");
    CHECK(out_copy["output"].is_null());  // 來源 sink 同值(見 setup 註)
    CHECK(out_copy["latencyPolicy"] == "lowLatency");

    // 後綴不疊加:副本的副本仍叫「吉他 複製」
    ok("duplicate_track", {{"trackId", copy_id}});
    tracks = snapshot_tracks();
    CHECK(tracks.size() == 7);
    CHECK(tracks[2]["name"] == "吉他 複製");

    // 未知 id
    const auto missing = send("duplicate_track", {{"trackId", 99999}});
    CHECK(!missing["ok"].get<bool>());
    CHECK(missing["error"]["code"] == "track_not_found");

    // 系統輸出軌不可複製
    ok("ensure_system_outputs", nlohmann::json::object());
    tracks = snapshot_tracks();
    std::uint32_t monitor = 0;
    for (const auto& t : tracks)
        if (t["systemRole"] == "monitor") monitor = t["trackId"].get<std::uint32_t>();
    CHECK(monitor != 0);
    const auto system_dup = send("duplicate_track", {{"trackId", monitor}});
    CHECK(!system_dup["ok"].get<bool>());
    CHECK(system_dup["error"]["code"] == "bad_command");

    // 插入失敗 = 整軌回滾零殘留。已載入的 module 檔被 OS 鎖住刪不掉,改用超限
    // latency(fixture param 100 = 1.0 → 4s,超出 2s 路由上限)迫使副本插入失敗
    const auto folder = std::filesystem::temp_directory_path() /
        ("roudamix-dup-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    const auto doomed = ok("track_add", {{"kind", "audio"}, {"name", "Doomed"}})["trackId"].get<std::uint32_t>();
    const auto doomed_plug = ok("add_plugin", {{"trackId", doomed}, {"path", module}})["instanceId"].get<std::uint32_t>();
    ok("set_param", {{"instanceId", doomed_plug}, {"paramId", 100}, {"value", 1.0}});
    ok("track_set_dests", {{"trackId", doomed}, {"dests", {sink}}});
    const auto before_count = snapshot_tracks().size();
    const auto failed = send("duplicate_track", {{"trackId", doomed}});
    CHECK(!failed["ok"].get<bool>());
    CHECK(failed["error"]["code"] == "plugin_state_failed");
    tracks = snapshot_tracks();
    CHECK(tracks.size() == before_count);
    for (const auto& t : tracks) CHECK(t["name"] != "Doomed 複製");
    // 原軌不受回滾影響,插件仍在
    for (const auto& t : tracks)
        if (t["trackId"] == doomed) CHECK(t["plugins"].size() == 1);

    // placeholder(遺失模組)原樣保留鏈位:手寫 session 檔 → load → 複製
    nlohmann::json placeholder_plugin = {
        {"pluginPath", "C:\\missing\\gone.vst3"}, {"classId", ""},
        {"name", "Gone"}, {"displayName", "自訂名"},
        {"bypassed", true}, {"monitorBypassed", true},
        {"params", nlohmann::json::array({{{"paramId", 5}, {"normalized", 0.5}}})},
        {"availability", "loadFailed"}, {"loadError", "gone"}};
    nlohmann::json session_file = {
        {"roudamixSession", 4},
        {"deviceKey", nullptr}, {"sampleRate", nullptr}, {"bufferSize", nullptr},
        {"tracks", nlohmann::json::array({nlohmann::json{
            {"trackId", 1}, {"kind", "audio"}, {"systemRole", nullptr},
            {"latencyPolicy", "fullPdc"}, {"name", "PH"}, {"color", 7},
            {"source", nullptr}, {"dests", nlohmann::json::array()},
            {"sidechain", nlohmann::json::array()}, {"output", nullptr},
            {"gain", 1.0}, {"mute", false},
            {"plugins", nlohmann::json::array({placeholder_plugin})}}})}};
    const auto session_path = folder / "placeholder.rmsession";
    { std::ofstream out(session_path); out << session_file.dump(2); }
    ok("load_session", {{"path", session_path.string()}});
    tracks = snapshot_tracks();
    // load 清空重建 + ensure_system_outputs 補 monitor/stream
    CHECK(tracks.size() == 3);
    CHECK(tracks[0]["name"] == "PH");
    const auto ph_id = tracks[0]["trackId"].get<std::uint32_t>();
    CHECK(tracks[0]["plugins"][0]["availability"] == "loadFailed");
    ok("duplicate_track", {{"trackId", ph_id}});
    tracks = snapshot_tracks();
    CHECK(tracks.size() == 4);
    CHECK(tracks[1]["name"] == "PH 複製");
    const auto& ph_copy = tracks[1]["plugins"][0];
    CHECK(ph_copy["availability"] == "loadFailed");
    // 註:session 載入對 placeholder 不回填 displayName(既有行為),副本如實複製
    // 源頭狀態;displayName 複製路徑由上面 fx 副本(「我的效果」)覆蓋
    CHECK(ph_copy["name"] == "Gone");
    CHECK(ph_copy["displayName"].is_null());
    CHECK(ph_copy["bypassed"] == true);
    CHECK(ph_copy["monitorBypassed"] == true);
    CHECK(ph_copy["loadError"] == "gone");
    CHECK(ph_copy["params"][0]["paramId"] == 5);
    CHECK(ph_copy["params"][0]["normalized"] == 0.5);
    std::filesystem::remove_all(folder);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    SetEnvironmentVariableW(L"ROUDAMIX_VST_REGISTRY", nullptr);
    const auto module = std::filesystem::absolute(argv[1]).string();
    sidechain_audio(module);
    sidechain_rollback(module);
    engine_copy(module);
    router_copy(module);
    router_duplicate_track(module);
    std::puts("plugin_copy_test PASSED");
}
