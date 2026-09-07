#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "audio_engine.hpp"
#include "command_contract.hpp"
#include "router.hpp"
#include "session.hpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

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
    CHECK(frames[0]["payload"]["capabilities"].get<std::vector<std::string>>().back() == "pluginCopyV1");
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

int main(int argc, char** argv) {
    CHECK(argc == 2);
    SetEnvironmentVariableW(L"ROUDAMIX_VST_REGISTRY", nullptr);
    const auto module = std::filesystem::absolute(argv[1]).string();
    engine_copy(module);
    router_copy(module);
    std::puts("plugin_copy_test PASSED");
}
