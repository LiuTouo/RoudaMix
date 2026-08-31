// track_graph 純邏輯單測(無音訊裝置):asio_channel_union、
// ensure_system_outputs 的確定性(migration/去重/補建)。
// CHECK 而非 assert:Release/NDEBUG 下 assert 是 no-op(M3 實測踩過)。
#include <cstdio>

#include "track_graph.hpp"

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

static rmx::TrackNode make_track(rmx::TrackKind kind, rmx::TrackSource src = {},
                                 rmx::TrackOutput out = {}) {
    rmx::TrackNode t;
    t.kind = kind;
    t.source = src;
    t.output = out;
    t.buf = std::make_shared<rmx::TrackRt>();
    return t;
}

int main() {
    // 1. asio_channel_union:pair 展開、排序去重;out 空 = fallback {0,1}
    {
        std::vector<rmx::TrackNode> tracks;
        rmx::TrackSource in;
        in.type = rmx::TrackSource::kAsioIn;
        in.asio_in_ch = 4;
        rmx::TrackOutput out;
        out.type = rmx::TrackOutput::kAsioOut;
        out.asio_out_ch = 2;
        tracks.push_back(make_track(rmx::TrackKind::kAudio, in, out));
        rmx::TrackSource in2 = in;
        in2.asio_in_ch = 0;
        tracks.push_back(make_track(rmx::TrackKind::kAudio, in2));
        std::vector<std::uint32_t> ins, outs;
        rmx::asio_channel_union(tracks, ins, outs);
        CHECK((ins == std::vector<std::uint32_t>{0, 1, 4, 5}));
        CHECK((outs == std::vector<std::uint32_t>{2, 3}));
        // out 聯集空 → {0,1}(createBuffers 至少一組 out)
        rmx::TrackSource sine;
        sine.type = rmx::TrackSource::kSine;
        tracks.clear();
        tracks.push_back(make_track(rmx::TrackKind::kAudio, sine));
        rmx::asio_channel_union(tracks, ins, outs);
        CHECK(ins.empty());
        CHECK((outs == std::vector<std::uint32_t>{0, 1}));
    }

    // 2. ensure_system_outputs:空場景 = 新建恰好一 monitor + 一 stream
    {
        std::vector<rmx::TrackNode> tracks;
        std::uint32_t next_id = 1;
        CHECK(rmx::ensure_system_outputs(tracks, next_id));
        CHECK(tracks.size() == 2);
        CHECK(tracks[0].system_role == rmx::SystemRole::kMonitor);
        CHECK(tracks[0].kind == rmx::TrackKind::kOutput);
        CHECK(tracks[0].output.type == rmx::TrackOutput::kAsioOut &&
              tracks[0].output.asio_out_ch == 0);
        CHECK(tracks[1].system_role == rmx::SystemRole::kStream);
        CHECK(tracks[1].output.type == rmx::TrackOutput::kNone);
        CHECK(next_id == 3);
        // 已齊 = no-op(冪等)
        CHECK(!rmx::ensure_system_outputs(tracks, next_id));
        CHECK(tracks.size() == 2);
    }

    // 3. 指派優先:無 role 的 output 軌先被指派,不夠才新建;audio 軌永不指派
    {
        std::vector<rmx::TrackNode> tracks;
        std::uint32_t next_id = 1;
        tracks.push_back(make_track(rmx::TrackKind::kAudio));   // 不參與指派
        tracks.push_back(make_track(rmx::TrackKind::kOutput));  // → monitor
        tracks.push_back(make_track(rmx::TrackKind::kOutput));  // → stream
        CHECK(rmx::ensure_system_outputs(tracks, next_id));
        CHECK(tracks.size() == 3);  // 不新建
        CHECK(tracks[0].system_role == rmx::SystemRole::kNone);
        CHECK(tracks[1].system_role == rmx::SystemRole::kMonitor);
        CHECK(tracks[2].system_role == rmx::SystemRole::kStream);
    }

    // 4. 重複 role:留第一個、其餘降級;降級後若缺另一 role,會補到/新建
    {
        std::vector<rmx::TrackNode> tracks;
        std::uint32_t next_id = 1;
        tracks.push_back(make_track(rmx::TrackKind::kOutput));
        tracks.push_back(make_track(rmx::TrackKind::kOutput));
        tracks.push_back(make_track(rmx::TrackKind::kOutput));
        tracks[0].system_role = rmx::SystemRole::kMonitor;
        tracks[1].system_role = rmx::SystemRole::kMonitor;
        CHECK(rmx::ensure_system_outputs(tracks, next_id));
        CHECK(tracks[0].system_role == rmx::SystemRole::kMonitor);
        CHECK(tracks[2].system_role == rmx::SystemRole::kNone);
        CHECK(tracks.size() == 3);
        int monitors = 0, streams = 0;
        for (const auto& t : tracks) {
            if (t.system_role == rmx::SystemRole::kMonitor) ++monitors;
            if (t.system_role == rmx::SystemRole::kStream) ++streams;
        }
        CHECK(monitors == 1 && streams == 1);
        // 降級的 B 應該被指派成 stream(優先指派現有軌,不新建)
        CHECK(tracks[1].system_role == rmx::SystemRole::kStream);
    }

    // 5. plan_telemetry_strips(P1-H):兩輪、可預測 — track 全數先領、剩餘才輪
    //    plugin;超出預算 = kNoStrip。前 N-1 條軌(master 序)必定有錶。
    {
        // 5a. 小場景:3 軌各 1 plugin,預算 8 → strip0 保留、每軌 track + plugin
        std::vector<rmx::TrackNode> tracks;
        for (int i = 0; i < 3; ++i) {
            rmx::TrackNode t = make_track(rmx::TrackKind::kAudio);
            t.chain.resize(1);
            tracks.push_back(std::move(t));
        }
        const auto plan = rmx::plan_telemetry_strips(tracks, 8);
        CHECK(plan.size() == 3);
        // 第一輪:track strips 連號 1..3(plugin 未動)
        CHECK(plan[0].track_strip == 1 && plan[1].track_strip == 2 && plan[2].track_strip == 3);
        // 第二輪:plugin strips 接在後面
        CHECK(plan[0].chain_strips[0] == 4);
        CHECK(plan[1].chain_strips[0] == 5);
        CHECK(plan[2].chain_strips[0] == 6);

        // 5b. 軌多於預算:100 軌、預算 64 → 前 63 軌有錶(strip 1..63)、後面無;
        //     plugin 全部無錶(track 優先於 plugin,不因陣列順序交錯誤導)
        std::vector<rmx::TrackNode> many;
        for (int i = 0; i < 100; ++i) {
            rmx::TrackNode t = make_track(rmx::TrackKind::kAudio);
            t.chain.resize(3);
            many.push_back(std::move(t));
        }
        const auto plan2 = rmx::plan_telemetry_strips(many, 64);
        CHECK(plan2.size() == 100);
        CHECK(plan2[0].track_strip == 1);
        CHECK(plan2[62].track_strip == 63);
        CHECK(plan2[63].track_strip == rmx::kNoStrip);
        CHECK(plan2[99].track_strip == rmx::kNoStrip);
        for (const auto& p : plan2) {
            for (const auto s : p.chain_strips) CHECK(s == rmx::kNoStrip);
        }

        // 5c. 預算內含 plugin:2 軌(2+1 plugin),預算 64 → 全拿到
        std::vector<rmx::TrackNode> few;
        for (int i = 0; i < 2; ++i) {
            rmx::TrackNode t = make_track(rmx::TrackKind::kAudio);
            t.chain.resize(2);
            few.push_back(std::move(t));
        }
        const auto plan3 = rmx::plan_telemetry_strips(few, 64);
        CHECK(plan3[0].track_strip == 1 && plan3[1].track_strip == 2);
        CHECK(plan3[0].chain_strips.size() == 2 && plan3[0].chain_strips[0] == 3 &&
              plan3[0].chain_strips[1] == 4);
        CHECK(plan3[1].chain_strips.size() == 2 && plan3[1].chain_strips[0] == 5 &&
              plan3[1].chain_strips[1] == 6);

        // 5d. 空場景:預算不動,strip 0 保留給 engine 輸出
        const auto plan4 = rmx::plan_telemetry_strips({}, 64);
        CHECK(plan4.empty());
    }

    std::printf("track_graph_test PASSED\n");
    return 0;
}
