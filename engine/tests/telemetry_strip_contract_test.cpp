#include <cstdio>
#include <cstdlib>

#include "audio_engine.hpp"
#include "session_projection.hpp"

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                         \
        }                                                                         \
    } while (0)

static void check_input_names() {
    rmx::AudioEngine engine;
    std::uint32_t first = 0, second = 0, next = 0;
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "", 0, first));
    CHECK(engine.tracks().back().name == "輸入1");
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "", 0, second));
    CHECK(engine.tracks().back().name == "輸入2");
    CHECK(!engine.track_set(first, "麥克風", {}, {}, {}));
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "", 0, next));
    CHECK(engine.tracks().back().name == "輸入3");
    CHECK(!engine.track_remove(next));
    CHECK(!engine.track_set(second, "吉他", {}, {}, {}));
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "", 0, next));
    CHECK(engine.tracks().back().name == "輸入1");
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "輸入9", 0, next));
    CHECK(engine.tracks().back().name == "輸入9");
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "輸入12備用", 0, next));
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "輸入099", 0, next));
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "", 0, next));
    CHECK(engine.tracks().back().name == "輸入10");
    CHECK(!engine.track_add(rmx::TrackKind::kApp, "輸入20", 0, next));
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "", 0, next));
    CHECK(engine.tracks().back().name == "輸入21");
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "輸入99999999999999999999", 0, next));
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "", 0, next));
    CHECK(engine.tracks().back().name == "輸入100000000000000000000");
    CHECK(!engine.track_add(rmx::TrackKind::kApp, "", 0, next));
    CHECK(engine.tracks().back().name == "app " + std::to_string(next));
}

int main() {
    check_input_names();
    rmx::AudioEngine engine;
    std::uint32_t track_id = 0;
    CHECK(!engine.track_add(rmx::TrackKind::kAudio, "Metered", 0, track_id));
    std::uint32_t instance_id = 0;
    CHECK(!engine.add_placeholder_plugin(
        track_id, "missing.vst3", "fixture", "Fixture", false,
        rmx::RackSlot::Availability::kMissing, "missing", {}, instance_id));

    // 公開 seam 取自 swap_graph 已提交的 active graph；publisher 也讀此 table。
    const auto plan = engine.telemetry_strip_plan();
    const auto snapshot =
        rmx::session::snapshot_json(engine, 1, 1, nlohmann::json::array());
    const auto& table = snapshot.at("telemetryStrips");
    CHECK(table == snapshot.at("status").at("telemetryStrips"));

    rmx::MeterAccumulator meters;
    rmx::TelemetryBlockShm block{};
    meters.publish(block, 0, plan.table.data(), plan.table.size(), nullptr, nullptr, 0,
                   false);
    CHECK(table.size() == block.strip_count);
    for (const auto& entry : table) {
        const auto id = entry.at("id").get<std::size_t>();
        CHECK(id < block.strip_count);
        CHECK(entry.at("kind") == static_cast<std::uint32_t>(plan.table[id].kind));
        CHECK(block.strips[id].kind == entry.at("kind"));
        if (entry.at("trackId").is_null()) {
            CHECK(entry.at("instanceId").is_null());
            CHECK(block.strips[id].instance_id == rmx::kNoTelemetryOwner);
        } else if (entry.at("instanceId").is_null()) {
            CHECK(entry.at("trackId") == track_id);
            CHECK(block.strips[id].instance_id == track_id);
        } else {
            CHECK(entry.at("trackId") == track_id && entry.at("instanceId") == instance_id);
            CHECK(block.strips[id].instance_id == instance_id);
        }
    }

    std::puts("telemetry_strip_contract_test PASSED");
    return 0;
}
