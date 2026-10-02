#include "fake_wasapi.hpp"
#include "router.hpp"
#include "command_contract.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <source_location>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using nlohmann::json;
struct Session {
    rmx::Router router;
    std::vector<json> frames;
    std::uint64_t id{};
    Session() { router.connect(1, [&](const auto& f) { frames.push_back(f); }); }
    json send(const std::string& kind, json payload = json::object()) {
        frames.clear();
        CHECK(!router.dispatch_guarded(1, {rmx::kProtocolVersion, ++id, kind, std::move(payload)}));
        for (const auto& f : frames) if (f.contains("id") && f["id"] == id) return f;
        CHECK(false); return {};
    }
    json ok(const std::string& kind, json payload = json::object()) {
        auto f = send(kind, std::move(payload));
        if (!f["ok"].get<bool>()) std::fprintf(stderr, "%s\n", f.dump().c_str());
        CHECK(f["ok"].get<bool>());
        rmx::contract::validate_result(kind, f["result"]); return f["result"];
    }
    json status() { return ok("get_snapshot")["snapshot"]["status"]; }
    void start() { ok("start", {{"deviceKey", "wasapi"}, {"sampleRate", nullptr}, {"bufferSize", nullptr}}); }
};
template<class F> void eventually(F f, const std::source_location where = std::source_location::current()) {
    for (int i = 0; i < 200; ++i) { if (f()) return; Sleep(10); }
    std::fprintf(stderr, "Timed out at line %u (master=%f speaker=%f)\n", where.line(),
        fake_wasapi::peak(L"master"), fake_wasapi::peak(L"speaker"));
    CHECK(f());
}
static void restore_and_retry(const std::string& module) {
    Session s;
    const auto defaults = s.ok("ensure_system_outputs");
    json output, monitor;
    for (const auto& track : defaults["tracks"]) {
        if (track["systemRole"] == "monitor") monitor = track["trackId"];
        if (track["systemRole"] == "stream") output = track["trackId"];
    }
    CHECK(!output.is_null() && !monitor.is_null());
    const auto input = s.ok("track_add", {{"kind", "audio"}, {"name", "Input"}})["trackId"];
    const auto aux = s.ok("track_add", {{"kind", "audio"}, {"name", "Aux"}})["trackId"];
    const auto fx = s.ok("track_add", {{"kind", "fx"}, {"name", "Effect"}})["trackId"];
    s.ok("track_set_source", {{"trackId", input}, {"source", {{"type", "wasapiIn"}, {"deviceId", "mic"}}}});
    s.ok("track_set_source", {{"trackId", aux}, {"source", {{"type", "wasapiIn"}, {"deviceId", "mic2"}}}});
    s.ok("track_set_dests", {{"trackId", input}, {"dests", json::array({fx})}});
    s.ok("track_set_dests", {{"trackId", aux}, {"dests", json::array()}});
    s.ok("track_set_dests", {{"trackId", fx}, {"dests", json::array({output, monitor})}});
    s.ok("track_set_sidechain", {{"trackId", fx}, {"sources", json::array({aux})}});
    s.ok("track_set_output", {{"trackId", output}, {"output", {{"type", "wasapi"}, {"deviceId", "speaker"}}}});
    s.ok("track_set_output", {{"trackId", monitor}, {"output", {{"type", "asioOut"}, {"channel", 0u}}}});
    s.ok("track_set_output_latency_policy", {{"trackId", monitor}, {"policy", "lowLatency"}});
    const auto plugin = s.ok("add_plugin", {{"trackId", fx}, {"path", module}})["instanceId"];
    s.ok("set_monitor_bypass", {{"instanceId", plugin}, {"bypassed", true}});
    s.ok("set_param", {{"instanceId", plugin}, {"paramId", 100u}, {"value", 128.0 / 192000.0}});
    s.start();
    const auto path = std::filesystem::temp_directory_path() /
        ("roudamix-wasapi-" + std::to_string(GetCurrentProcessId()) + ".rmsession");
    s.ok("save_session", {{"path", path.string()}});
    for (const bool running : {true, false}) {
        if (!running) s.ok("stop");
        const auto loaded = s.ok("load_session", {{"path", path.string()}});
        CHECK(loaded["deviceKey"] == "wasapi");
        if (running) s.ok("stop"); // same command sequence as App's restore queue
        s.start(); Sleep(100); fake_wasapi::clear_samples();
        eventually([] { return fake_wasapi::peak(L"speaker") > .74F; });
        CHECK(fake_wasapi::peak(L"speaker") < .76F);
        eventually([] { return fake_wasapi::peak(L"master") > .24F; });
        CHECK(fake_wasapi::peak(L"master") < .26F); // bypass applies only to low-latency output
        const auto restored = s.status();
        for (const auto& track : restored["tracks"]) {
            if (track["systemRole"] == "monitor") CHECK(track["latencyPolicy"] == "lowLatency");
            if (track["systemRole"] == "stream") CHECK(track["latencyPolicy"] == "fullPdc");
            for (const auto& slot : track["plugins"]) {
                CHECK(slot["monitorBypassed"] == true);
                CHECK(slot["params"][0]["normalized"] == 128.0 / 192000.0);
            }
        }
        CHECK(s.status()["pluginFails"] == 0);
    }
    json saved;
    { std::ifstream in(path); in >> saved; }
    for (auto& track : saved["tracks"])
        for (auto& plugin : track["plugins"]) plugin["pluginPath"] = module + ".missing";
    { std::ofstream out(path); out << saved; }
    const auto loaded = s.ok("load_session", {{"path", path.string()}});
    CHECK(!loaded["missing"].empty());
    json instance;
    const auto restored = s.status();
    for (const auto& track : restored["tracks"])
        if (!track["plugins"].empty()) instance = track["plugins"][0]["instanceId"];
    CHECK(!instance.is_null());
    CHECK(!s.send("retry_plugin", {{"instanceId", instance}, {"path", module + ".missing"}})["ok"].get<bool>());
    s.ok("retry_plugin", {{"instanceId", instance}, {"path", module}});
    fake_wasapi::clear_samples();
    eventually([] { return fake_wasapi::peak(L"speaker") > .74F; });
    CHECK(s.status()["pluginFails"] == 0);
    s.ok("stop"); CHECK(fake_wasapi::clients == 0);
    std::filesystem::remove(path);
}
static void rejected_initialization(const std::string& module) {
    rmx::AudioEngine engine;
    std::uint32_t track{}, placeholder{}, unused{};
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "Input", 0, track));
    CHECK(!engine.add_placeholder_plugin(track, module, "", "Fixture", false,
        rmx::RackSlot::Availability::kLoadFailed, "test", {}, placeholder));
    CHECK(!engine.start("wasapi", {}, {}));
    SetEnvironmentVariableW(L"ROUDAMIX_TEST_REJECT_SETUP", L"1");
    CHECK(engine.add_plugin(track, module, "", unused).has_value());
    CHECK(engine.load_placeholder(placeholder, module, "").has_value());
    Sleep(30);
    CHECK(engine.status().plugin_fails == 0);
    CHECK(engine.tracks()[0].chain.size() == 1 && engine.find_slot(placeholder)->is_placeholder());
    SetEnvironmentVariableW(L"ROUDAMIX_TEST_REJECT_SETUP", nullptr);
    engine.stop(); CHECK(fake_wasapi::clients == 0);
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetEnvironmentVariableW(L"ROUDAMIX_VST_REGISTRY", nullptr);
    {
        Session s;
        const auto input = s.ok("track_add", {{"kind", "audio"}, {"name", "Input"}})["trackId"];
        const auto output = s.ok("track_add", {{"kind", "output"}, {"name", "Monitor"}})["trackId"];
        s.ok("track_set_source", {{"trackId", input}, {"source", {{"type", "sine"}, {"freq", 440}}}});
        s.ok("track_set_output", {{"trackId", output}, {"output", {{"type", "asioOut"}, {"channel", 0u}}}});
        s.ok("track_set_dests", {{"trackId", input}, {"dests", json::array({output})}});
        for (int i = 0; i < 2; ++i) {
            fake_wasapi::clear_samples(); s.start();
            eventually([] { return fake_wasapi::peak(L"master") > .01F; });
            s.ok("stop"); CHECK(fake_wasapi::clients == 0);
        }
        fake_wasapi::fail_start = true;
        CHECK(!s.send("start", {{"deviceKey", "wasapi"}, {"sampleRate", nullptr}, {"bufferSize", nullptr}})["ok"].get<bool>());
        CHECK(!s.status()["running"].get<bool>());
        CHECK(fake_wasapi::clients == 0);
        fake_wasapi::fail_start = false;
        fake_wasapi::fail_buffer_size = true;
        CHECK(!s.send("start", {{"deviceKey", "wasapi"}, {"sampleRate", nullptr}, {"bufferSize", nullptr}})["ok"].get<bool>());
        CHECK(fake_wasapi::clients == 0);
        fake_wasapi::fail_buffer_size = false;
        s.ok("track_set_source", {{"trackId", input}, {"source", nullptr}});
        s.start();
        for (const auto* endpoint : {"", "mic2"}) {
            fake_wasapi::clear_samples();
            s.ok("track_set_source", {{"trackId", input}, {"source", {{"type", "wasapiIn"}, {"deviceId", endpoint}}}});
            const float expected = *endpoint ? .5F : .25F;
            eventually([&] { return fake_wasapi::peak(L"master") >= expected - .01F; });
            CHECK(fake_wasapi::peak(L"master") <= expected + .01F);
        }
        s.ok("track_set_source", {{"trackId", input}, {"source", nullptr}});
        CHECK(fake_wasapi::clients == 1);
        Sleep(50); fake_wasapi::clear_samples(); Sleep(50);
        CHECK(fake_wasapi::peak(L"master") == 0);
        fake_wasapi::fail_endpoint = L"mic";
        CHECK(!s.send("track_set_source", {{"trackId", input}, {"source", {{"type", "wasapiIn"}, {"deviceId", ""}}}})["ok"].get<bool>());
        fake_wasapi::fail_endpoint.clear();
        const auto app = s.ok("track_add", {{"kind", "app"}, {"name", "App"}})["trackId"];
        s.ok("track_set_dests", {{"trackId", app}, {"dests", json::array({output})}});
        for (int i = 0; i < 2; ++i) {
            s.ok("track_set_source", {{"trackId", app}, {"source", {{"type", "app"}, {"pid", GetCurrentProcessId()}}}});
            fake_wasapi::clear_samples();
            eventually([] { return fake_wasapi::peak(L"master") > .74F; });
        }
        s.ok("track_set_source", {{"trackId", app}, {"source", nullptr}});
        fake_wasapi::fail_endpoint = L"app";
        CHECK(!s.send("track_set_source", {{"trackId", app}, {"source", {{"type", "app"}, {"pid", GetCurrentProcessId()}}}})["ok"].get<bool>());
        fake_wasapi::fail_endpoint.clear();
        s.ok("stop"); CHECK(fake_wasapi::clients == 0);
        const auto aux = s.ok("track_add", {{"kind", "audio"}, {"name", "Sidechain"}})["trackId"];
        const auto fx = s.ok("track_add", {{"kind", "fx"}, {"name", "Effect"}})["trackId"];
        s.ok("track_set_dests", {{"trackId", input}, {"dests", json::array({fx})}});
        s.ok("track_set_dests", {{"trackId", fx}, {"dests", json::array({output})}});
        s.ok("track_set_source", {{"trackId", aux}, {"source", {{"type", "wasapiIn"}, {"deviceId", "mic2"}}}});
        s.ok("track_set_dests", {{"trackId", aux}, {"dests", json::array()}});
        s.ok("track_set_sidechain", {{"trackId", fx}, {"sources", json::array({aux})}});
        s.ok("track_set_output", {{"trackId", output}, {"output", {{"type", "asioOut"}, {"channel", 0u}}}});
        s.ok("track_set_source", {{"trackId", input}, {"source", {{"type", "wasapiIn"}, {"deviceId", "mic"}}}});
        s.start();
        const auto before = s.status()["pluginFails"];
        const auto plugin = s.ok("add_plugin", {{"trackId", fx}, {"path", argv[1]}})["instanceId"];
        fake_wasapi::clear_samples();
        eventually([] { return fake_wasapi::peak(L"master") > .74F; });
        CHECK(fake_wasapi::peak(L"master") < .76F);
        CHECK(s.status()["pluginFails"] == before);
        s.ok("track_set_output_latency_policy", {{"trackId", output}, {"policy", "lowLatency"}});
        s.ok("set_monitor_bypass", {{"instanceId", plugin}, {"bypassed", true}});
        const auto shadowed = s.ok("add_plugin", {{"trackId", fx}, {"path", argv[1]}})["instanceId"];
        const auto preset = std::filesystem::temp_directory_path() /
            ("roudamix-wasapi-" + std::to_string(GetCurrentProcessId()) + ".vstpreset");
        s.ok("set_param", {{"instanceId", shadowed}, {"paramId", 100u}, {"value", 128.0 / 192000.0}});
        Sleep(30);
        s.ok("save_preset", {{"instanceId", shadowed}, {"path", preset.string()}});
        s.ok("set_param", {{"instanceId", shadowed}, {"paramId", 100u}, {"value", 0.0}});
        s.ok("load_preset", {{"instanceId", shadowed}, {"path", preset.string()}});
        Sleep(100); // graph crossfade and restored plugin delay settle
        fake_wasapi::clear_samples();
        eventually([] { return fake_wasapi::peak(L"master") > .74F; });
        CHECK(fake_wasapi::peak(L"master") < .76F);
        CHECK(s.status()["pluginFails"] == before);
        s.ok("remove_plugin", {{"instanceId", shadowed}});
        std::filesystem::remove(preset);
        s.ok("remove_plugin", {{"instanceId", plugin}});
        s.ok("track_set_sidechain", {{"trackId", fx}, {"sources", json::array()}});
        s.ok("track_set_source", {{"trackId", aux}, {"source", nullptr}});
        s.ok("stop");
        s.ok("track_set_source", {{"trackId", input}, {"source", {{"type", "wasapiIn"}, {"deviceId", "mic"}}}});
        s.start();
        for (const auto* endpoint : {"speaker", "headphones"}) {
            s.ok("track_set_output", {{"trackId", output}, {"output", {{"type", "wasapi"}, {"deviceId", endpoint}}}});
            fake_wasapi::clear_samples();
            const std::wstring wide(endpoint, endpoint + std::strlen(endpoint));
            eventually([&] { return fake_wasapi::peak(wide) > .24F; });
            CHECK(fake_wasapi::clients == 3);
        }
        s.ok("track_set_output", {{"trackId", output}, {"output", nullptr}});
        CHECK(fake_wasapi::clients == 2);
        fake_wasapi::clear_samples(); Sleep(50);
        CHECK(fake_wasapi::peak(L"headphones") == 0);
        fake_wasapi::fail_endpoint = L"speaker";
        CHECK(!s.send("track_set_output", {{"trackId", output}, {"output", {{"type", "wasapi"}, {"deviceId", "speaker"}}}})["ok"].get<bool>());
        fake_wasapi::fail_endpoint.clear();
        s.ok("stop"); CHECK(fake_wasapi::clients == 0);
        std::atomic<int> failures{0};
        s.ok("track_set_output", {{"trackId", output}, {"output", {{"type", "wasapi"}, {"deviceId", "speaker"}}}});
        s.router.set_capture_failed_callback([&](std::uint32_t id) {
            CHECK(id == 0); ++failures;  // queued control-thread notification
        });
        s.start();
        const auto revision = s.status()["revision"];
        fake_wasapi::lost = true;
        eventually([&] { return !s.status()["running"].get<bool>(); });
        eventually([&] { return failures.load() == 1; });
        s.router.handle_track_failed(0);
        CHECK(s.frames.back()["kind"] == "status");
        CHECK(!s.frames.back()["payload"]["running"].get<bool>());
        CHECK(!s.frames.back()["payload"]["error"].is_null());
        CHECK(!s.status()["error"].is_null());
        CHECK(s.status()["revision"] == revision);
        CHECK(fake_wasapi::clients == 0);
        s.ok("stop"); s.ok("stop");
        fake_wasapi::lost = false;
        s.start();
        s.router.handle_track_failed(0); // stale failure must not stop a recovered stream
        CHECK(s.status()["running"].get<bool>());
        CHECK(s.status()["error"].is_null());
        fake_wasapi::clear_samples();
        eventually([] { return fake_wasapi::peak(L"speaker") > .24F; });
        s.ok("stop"); CHECK(failures == 1); CHECK(fake_wasapi::clients == 0);
    }
    restore_and_retry(argv[1]);
    rejected_initialization(argv[1]);
    {
        Session failed;
        failed.start();
        fake_wasapi::stall_events = true;
        Sleep(50);
        fake_wasapi::lost = true;
        eventually([&] { return !failed.status()["running"].get<bool>(); });
        CHECK(!failed.status()["error"].is_null());
        // No explicit Stop or notification handler: destruction still joins the failed pump.
    }
    CHECK(fake_wasapi::clients == 0);
    fake_wasapi::stall_events = false; fake_wasapi::lost = false;
    {
        rmx::AudioEngine engine;
        CHECK(!engine.start("wasapi", {}, {}));
        // Healthy WASAPI destruction must stop callbacks before freeing graph/track members.
    }
    CHECK(fake_wasapi::clients == 0);
    CoUninitialize();
    std::puts("WASAPI runtime passed");
}
