#include "fake_wasapi.hpp"
#include "router.hpp"
#include "command_contract.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using nlohmann::json;
// Fail the control thread's next allocation without perturbing the audio/worker threads.
thread_local bool fail_allocation = false;
void* operator new(std::size_t n) {
    if (fail_allocation) { fail_allocation = false; throw std::bad_alloc(); }
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

struct Session {
    rmx::Router router;
    std::vector<json> frames;
    std::uint64_t id{}, generation{1};
    Session() { connect(); }
    void connect() { router.connect(generation, [&](const auto& frame) { frames.push_back(frame); }); }
    json send(const std::string& kind, json payload = json::object()) {
        frames.clear();
        CHECK(!router.dispatch_guarded(generation, {rmx::kProtocolVersion, ++id, kind, std::move(payload)}));
        for (const auto& f : frames) if (f.contains("id") && f["id"] == id) return f;
        CHECK(false); return {};
    }
    json ok(const std::string& kind, json payload = json::object()) {
        const auto reply = send(kind, std::move(payload));
        if (!reply["ok"].get<bool>()) std::fprintf(stderr, "%s\n", reply.dump().c_str());
        CHECK(reply["ok"].get<bool>());
        rmx::contract::validate_result(kind, reply["result"]);
        return reply["result"];
    }
    void error(const std::string& kind, json payload, const char* code) {
        auto r = send(kind, std::move(payload)); CHECK(r["ok"] == false); CHECK(r["error"]["code"] == code);
    }
    json snapshot() { return ok("get_snapshot")["snapshot"]; }
    void start() { ok("start", {{"deviceKey", "wasapi"}, {"sampleRate", nullptr}, {"bufferSize", nullptr}}); }
};
static json key(const json& id) { return {{"measurementId", id}}; }
static json warmed(Session& s, const json& id) {
    for (int i = 0; i < 1500; ++i) {
        auto r = s.ok("get_loudness_report", key(id))["report"];
        if (r["elapsedSeconds"].get<double>() >= .5) return r;
        Sleep(10);
    }
    CHECK(false); return {};
}
static void router_lifecycle() {
    Session s;
    CHECK(s.snapshot()["capabilities"].get<std::vector<std::string>>().back() == "loudnessAnalysisV1");
    s.error("start_loudness", {{"trackId", 1}}, "not_running");
    for (const auto* kind : {"start_loudness", "get_loudness_report", "reset_loudness", "stop_loudness"}) {
        bool rejected = false;
        try { rmx::contract::validate_command_payload(kind, json::object()); }
        catch (const rmx::contract::ValidationError&) { rejected = true; }
        CHECK(rejected);
    }
    s.error("reset_loudness", key("missing"), "bad_command");
    s.ok("stop_loudness", key("missing"));
    const auto track = s.ok("track_add", {{"kind", "audio"}})["trackId"];
    s.ok("track_set_source", {{"trackId", track}, {"source", {{"type", "sine"}, {"freq", 1000}}}});
    s.start();
    s.error("start_loudness", {{"trackId", 999999}}, "track_not_found");
    const auto before = s.snapshot();
    const auto generation = s.ok("get_latency_report")["report"]["generation"];
    auto report = s.ok("start_loudness", {{"trackId", track}})["report"];
    const auto first = report["measurementId"];
    CHECK(s.frames.size() == 1); // Measurement controls must not publish status/revision effects.
    CHECK(report["state"] == "measuring" && report["elapsedSeconds"] == 0);
    CHECK(report["momentaryLufs"].is_null() && report["truePeakDbtp"].is_null());
    CHECK(!warmed(s, first)["momentaryLufs"].is_null());
    report = s.ok("reset_loudness", key(first))["report"];
    const auto second = report["measurementId"];
    CHECK(first != second && report["elapsedSeconds"] == 0);
    for (auto field : {"momentaryLufs", "shortTermLufs", "integratedLufs", "truePeakDbtp", "loudnessRangeLu"})
        CHECK(report[field].is_null());
    s.error("get_loudness_report", key(first), "bad_command");
    s.error("reset_loudness", key(first), "bad_command");
    s.ok("stop_loudness", key(first));
    CHECK(s.ok("get_loudness_report", key(second))["report"]["measurementId"] == second);
    CHECK(s.snapshot()["status"]["revision"] == before["status"]["revision"]);
    CHECK(s.ok("get_latency_report")["report"]["generation"] == generation);
    s.ok("stop");
    CHECK(s.ok("get_loudness_report", key(second))["report"]["state"] == "stopped");
    s.error("reset_loudness", key(second), "not_running");
    s.start();
    s.error("get_loudness_report", key(second), "bad_command");
    const auto third = s.ok("start_loudness", {{"trackId", track}})["report"]["measurementId"];
    CHECK(third != first && third != second);
    s.router.disconnect(999); // An obsolete owner cannot clear the current measurement.
    s.ok("get_loudness_report", key(third));
    s.router.disconnect(s.generation);
    ++s.generation; s.connect();
    s.error("get_loudness_report", key(third), "bad_command");
    const auto fourth = s.ok("start_loudness", {{"trackId", track}})["report"]["measurementId"];
    s.ok("stop_loudness", key(fourth));
    CHECK(s.frames.size() == 1);
    s.ok("stop_loudness", key(fourth));
    s.error("get_loudness_report", key(fourth), "bad_command");
    const auto fifth = s.ok("start_loudness", {{"trackId", track}})["report"]["measurementId"];
    ++s.generation; s.connect(); // Replacing an owner also clears an active measurement.
    s.error("get_loudness_report", key(fifth), "bad_command");
    const auto sixth = s.ok("start_loudness", {{"trackId", track}})["report"]["measurementId"];
    s.ok("track_remove", {{"trackId", track}});
    s.error("get_loudness_report", key(sixth), "bad_command");
    s.ok("stop");
}
static rmx::LoudnessReport warmed(rmx::AudioEngine& engine, const std::string& id) {
    rmx::LoudnessReport report;
    for (int i = 0; i < 1500; ++i) {
        CHECK(!engine.get_loudness_report(id, report));
        if (report.elapsed_seconds >= .5) return report;
        Sleep(10);
    }
    CHECK(false); return {};
}
static void graph_lifetime_and_audio() {
    rmx::AudioEngine engine;
    std::uint32_t input{}, output{};
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "tone", 0, input));
    CHECK(!engine.track_add(rmx::TrackKind::kOutput, "output", 0, output));
    rmx::TrackSource source; source.type = rmx::TrackSource::kSine; source.sine_freq = 1000;
    CHECK(!engine.track_set_source(input, source));
    CHECK(!engine.track_set_dests(input, {output}));
    CHECK(!engine.start("wasapi", {}, {}));
    const auto generation = engine.latency_generation();
    rmx::LoudnessReport report;
    CHECK(!engine.start_loudness(input, report));
    const auto initial_id = report.measurement_id;
    auto baseline = warmed(engine, initial_id);
    CHECK(baseline.momentary_lufs);
    auto* old_graph = engine.acquire_graph();
    CHECK(old_graph && old_graph->loudness);
    auto shell = old_graph->loudness;
    fail_allocation = true;
    CHECK(engine.reset_loudness(initial_id, report)->code == rmx::Err::kInternal);
    CHECK(!engine.get_loudness_report(initial_id, report));
    CHECK(report.state == rmx::LoudnessReport::State::kMeasuring);
    std::set<std::string> ids{initial_id};
    for (int i = 0; i < 20; ++i) {
        CHECK(!engine.reset_loudness(report.measurement_id, report));
        CHECK(ids.insert(report.measurement_id).second);
    }
    CHECK(engine.latency_generation() == generation);
    CHECK(shell->report().state == rmx::LoudnessReport::State::kStopped);
    shell->capture(nullptr, nullptr, 100); // Old graph remains readable, large resources already released.
    engine.release_graph();
    CHECK(!engine.track_set(input, {}, {}, .5f, {}));
    Sleep(300);
    CHECK(!engine.reset_loudness(report.measurement_id, report));
    auto half = warmed(engine, report.measurement_id);
    CHECK(std::abs(*half.momentary_lufs - *baseline.momentary_lufs + 6.0206) < .15);
    CHECK(!engine.start_loudness(output, report));
    auto bus = warmed(engine, report.measurement_id);
    CHECK(std::abs(*bus.momentary_lufs - *half.momentary_lufs) < .15);
    CHECK(!engine.track_set(input, {}, {}, {}, true));
    Sleep(300);
    CHECK(!engine.reset_loudness(report.measurement_id, report));
    auto muted = warmed(engine, report.measurement_id);
    CHECK(!muted.momentary_lufs && !muted.true_peak_dbtp);
    engine.clear_all_tracks();
    CHECK(engine.get_loudness_report(report.measurement_id, report)->code == rmx::Err::kBadCommand);
    engine.stop();
}
static void monitor_bus(const std::string& module) {
    rmx::AudioEngine engine;
    std::uint32_t input{}, aux{}, fx{}, monitor{}, stream{}, plugin{};
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "input", 0, input));
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "aux", 0, aux));
    CHECK(!engine.track_add(rmx::TrackKind::kFx, "fx", 0, fx));
    CHECK(!engine.track_add(rmx::TrackKind::kOutput, "monitor", 0, monitor));
    CHECK(!engine.track_add(rmx::TrackKind::kOutput, "stream", 0, stream));
    rmx::TrackSource sine; sine.type = rmx::TrackSource::kSine; sine.sine_freq = 1000;
    CHECK(!engine.track_set_source(input, sine));
    CHECK(!engine.track_set_source(aux, sine));
    CHECK(!engine.track_set_dests(input, {fx}));
    CHECK(!engine.track_set_dests(aux, {}));
    CHECK(!engine.track_set_dests(fx, {monitor, stream}));
    CHECK(!engine.track_set_sidechain(fx, {aux}));
    CHECK(!engine.track_set_output_latency_policy(monitor, rmx::OutputLatencyPolicy::kLowLatency));
    CHECK(!engine.add_plugin(fx, module, "", plugin));
    CHECK(!engine.set_monitor_bypass(plugin, true));
    CHECK(!engine.start("wasapi", {}, {}));
    rmx::LoudnessReport r;
    CHECK(!engine.start_loudness(stream, r));
    auto primary = warmed(engine, r.measurement_id);
    CHECK(!engine.start_loudness(monitor, r));
    auto low_latency = warmed(engine, r.measurement_id);
    CHECK(primary.momentary_lufs && low_latency.momentary_lufs);
    CHECK(std::abs(*primary.momentary_lufs - *low_latency.momentary_lufs - 6.0206) < .15);
    CHECK(!engine.start_loudness(fx, r));
    auto fx_primary = warmed(engine, r.measurement_id);
    CHECK(std::abs(*primary.momentary_lufs - *fx_primary.momentary_lufs) < .15);
    engine.stop();
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    router_lifecycle();
    graph_lifetime_and_audio();
    monitor_bus(argv[1]);
    CoUninitialize();
    std::puts("loudness engine: contract, stale IDs, revisions, restart/disconnect/removal, allocation rollback, held graphs, gain/mute/output passed");
}
