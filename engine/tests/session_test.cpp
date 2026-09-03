// session 單測(無 plugin 環境:v3 roundtrip / v2 migration / 壞 slot / v1 拒載)。
// 真 plugin 的 save→load roundtrip 走 scripts/m5a-engine-tracks.ps1(pipe 層)。
// CHECK 而非 assert:Release/NDEBUG 下 assert 是 no-op,測試會空轉(M3 實測踩過)。
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "audio_engine.hpp"
#include "sandbox.hpp"
#include "session.hpp"

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

static std::string read_text(const std::filesystem::path& p) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || f == nullptr) return {};
    std::string text;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    return text;
}
static void write_text(const std::filesystem::path& p, const char* s) {
    std::FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"wb");
    std::fputs(s, f);
    std::fclose(f);
}

int main() {
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "rmx-session-test";
    std::filesystem::create_directories(tmp);
    const auto file = tmp / "t.rmsession";

    rmx::AudioEngine e;

    // 1. 空 engine serialize:結構 + version
    {
        const nlohmann::json j = rmx::session::serialize(e);
        CHECK(j["roudamixSession"] == 3);
        CHECK(j["tracks"].is_array() && j["tracks"].empty());
        CHECK(j["deviceKey"].is_null() && j["sampleRate"].is_null());

        rmx::AudioEngine policies;
        CHECK(policies.ensure_system_outputs());
        const auto pj = rmx::session::serialize(policies);
        CHECK(pj["tracks"].size() == 2);
        CHECK(pj["tracks"][0]["systemRole"] == "monitor");
        CHECK(pj["tracks"][0]["latencyPolicy"] == "lowLatency");
        CHECK(pj["tracks"][1]["systemRole"] == "stream");
        CHECK(pj["tracks"][1]["latencyPolicy"] == "fullPdc");
    }

    // 2. 軌道結構 roundtrip:audio(sine)→fx→output 監聽,dests 鏈 + gain/mute
    {
        std::uint32_t a = 0, fx = 0, o = 0;
        CHECK(!e.track_add(rmx::TrackKind::kAudio, "Mic", 0x3ddc84, a));
        CHECK(!e.track_add(rmx::TrackKind::kFx, "FX", 0, fx));
        CHECK(!e.track_add(rmx::TrackKind::kOutput, "監聽", 0, o));
        rmx::TrackSource src;
        src.type = rmx::TrackSource::kSine;
        src.sine_freq = 880.0F;
        CHECK(!e.track_set_source(a, src));
        CHECK(!e.track_set_dests(a, {fx}));
        CHECK(!e.track_set_dests(fx, {o}));
        rmx::TrackOutput out;
        out.type = rmx::TrackOutput::kAsioOut;
        CHECK(!e.track_set_output(o, out));
        CHECK(!e.track_set(a, std::nullopt, std::nullopt, 0.5F, true));
        CHECK(!rmx::session::save(e, file));

        // 改掉 → load 復原
        CHECK(!e.track_set_dests(a, {}));
        CHECK(!e.track_set(a, std::nullopt, std::nullopt, 1.0F, false));
        nlohmann::json applied;
        CHECK(!rmx::session::load(e, file, applied));
        // 3 條(存檔內容)+ 自動補回的 stream 系統輸出 = 4
        CHECK(e.tracks().size() == 4);
        // 新 id != 舊 id,但 dests 鏈已重接;找 kind 對應驗
        const rmx::TrackNode* audio = nullptr;
        const rmx::TrackNode* fxt = nullptr;
        for (const auto& t : e.tracks()) {
            if (t.kind == rmx::TrackKind::kAudio) audio = &t;
            if (t.kind == rmx::TrackKind::kFx) fxt = &t;
        }
        CHECK(audio != nullptr && fxt != nullptr);
        CHECK(audio->source.type == rmx::TrackSource::kSine && audio->source.sine_freq == 880.0F);
        CHECK(audio->dests.size() == 1 && audio->dests[0] == fxt->track_id);
        CHECK(audio->mute && audio->gain == 0.5F);
        CHECK(applied["deviceKey"].is_null() && applied["sampleRate"].is_null());
    }

    // 3. 檔案內容可讀 + 版本欄位正確
    {
        const auto j = nlohmann::json::parse(read_text(file), nullptr, false);
        CHECK(!j.is_discarded());
        CHECK(j["roudamixSession"] == 3);
        CHECK(j["tracks"].is_array() && j["tracks"].size() == 3);
    }

    // 3b. save overrides:UI 帶 deviceKey/sampleRate 蓋寫(免 start 過)
    {
        const auto ov = tmp / "ov.rmsession";
        const nlohmann::json overrides{{"deviceKey", "asio:dev1"}, {"sampleRate", 48000u}};
        CHECK(!rmx::session::save(e, ov, overrides));
        const auto j = nlohmann::json::parse(read_text(ov), nullptr, false);
        CHECK(!j.is_discarded());
        CHECK(j["deviceKey"] == "asio:dev1");
        CHECK(j["sampleRate"] == 48000);
    }

    // 4. 非 session 檔 → load false
    {
        const auto bad = tmp / "bad.rmsession";
        write_text(bad, "not json{");
        nlohmann::json applied;
        CHECK(rmx::session::load(e, bad, applied));
    }
    // 5. v1 檔(舊 rack 格式)→ 一律拒載
    {
        const auto bad = tmp / "v1.rmsession";
        write_text(bad,
                   R"({"roudamixSession":1,"rack":[{"pluginPath":"C:\\nope\\x.vst3"}],)"
                   R"("sineFreq":220.0,"source":"sine"})");
        nlohmann::json applied;
        CHECK(rmx::session::load(e, bad, applied));
        // 拒載 = 狀態不動(第 2 案的 3+1 條軌還在)
        CHECK(e.tracks().size() == 4);
    }
    // 6. 壞 slot(不存在 module)= 原位置 placeholder:metadata/params 全存、
    //    diagnostics 帶結構化資訊、軌與路由不整體失敗
    {
        // fail-closed 的原因碼依測試環境:worker exe 在(與測試同目錄,實際試爆後
        // module 載不動)= plugin_load_failed;worker 不在 = sandbox_unavailable
        const bool has_worker = !rmx::sandbox::worker_path().empty();
        const char* expect_code = has_worker ? "plugin_load_failed" : "sandbox_unavailable";
        const auto f2 = tmp / "miss.rmsession";
        write_text(f2,
                   R"({"roudamixSession":3,"tracks":[{"trackId":7,"kind":"audio","name":"Mic",)"
                   R"("color":255,"source":{"type":"sine","freq":440},"dests":[],"output":null,)"
                   R"("gain":1,"mute":false,"plugins":[{"pluginPath":"C:\\nope\\x.vst3",)"
                   R"("classId":"ABCD","name":"XComp","bypassed":true,"monitorBypassed":true,)"
                   R"("params":[{"paramId":1,"normalized":0.75}]}]}]})");
        nlohmann::json applied;
        CHECK(!rmx::session::load(e, f2, applied));
        // 1(Mic)+ 自動補回 monitor/stream 系統輸出 = 3
        CHECK(e.tracks().size() == 3);
        CHECK(e.tracks()[0].chain.size() == 1);  // placeholder 佔住原鏈位
        const auto& slot = e.tracks()[0].chain[0];
        CHECK(slot.is_placeholder());
        CHECK(slot.plugin == nullptr);
        CHECK(slot.name == "XComp");
        CHECK(slot.module_path == "C:\\nope\\x.vst3");
        CHECK(slot.class_id == "ABCD");
        CHECK(slot.bypass);
        CHECK(slot.monitor_bypass);
        CHECK(slot.param_values.size() == 1 && slot.param_values[0].first == 1 &&
              slot.param_values[0].second == 0.75);
        CHECK(slot.load_error.empty() == false);
        // structured diagnostics:trackId(舊 id)/index/name/path/code
        CHECK(applied["missing"].is_array() && applied["missing"].size() == 1);
        const auto& m = applied["missing"][0];
        CHECK(m["trackId"] == 7);
        CHECK(m["index"] == 0);
        CHECK(m["name"] == "XComp");
        CHECK(m["pluginPath"] == "C:\\nope\\x.vst3");
        CHECK(m["classId"] == "ABCD");
        CHECK(m["message"].is_string());
        CHECK(m["code"] == expect_code);
        CHECK(e.tracks()[0].name == "Mic");
        CHECK(e.tracks()[0].source.type == rmx::TrackSource::kSine);
        // 系統輸出補回:placeholder 場景也必須有恰好一組
        int monitors = 0, streams = 0;
        for (const auto& t : e.tracks()) {
            if (t.system_role == rmx::SystemRole::kMonitor) ++monitors;
            if (t.system_role == rmx::SystemRole::kStream) ++streams;
        }
        CHECK(monitors == 1 && streams == 1);
    }

    // 6b. placeholder roundtrip:含 placeholder 的 session 存檔 → load,metadata
    //     與 params/availability/loadError 原樣保留(重新儲存不得丟 placeholder)
    {
        CHECK(!rmx::session::save(e, file));  // 上一步載入的 1 軌 + placeholder
        const auto j = nlohmann::json::parse(read_text(file), nullptr, false);
        CHECK(!j.is_discarded());
        CHECK(j["tracks"][0]["plugins"][0]["availability"] == "loadFailed");
        CHECK(j["tracks"][0]["plugins"][0]["monitorBypassed"] == true);
        nlohmann::json applied;
        CHECK(!rmx::session::load(e, file, applied));
        CHECK(e.tracks().size() == 3);  // Mic + monitor + stream(role 已寫進檔)
        CHECK(e.tracks()[0].chain.size() == 1);
        const auto& slot = e.tracks()[0].chain[0];
        CHECK(slot.is_placeholder());
        CHECK(slot.availability == rmx::RackSlot::Availability::kLoadFailed);
        CHECK(slot.param_values.size() == 1 && slot.param_values[0].second == 0.75);
        CHECK(slot.bypass);
        CHECK(slot.monitor_bypass);
        CHECK(applied["missing"].size() == 1);
        CHECK(applied["missing"][0]["code"] == "plugin_load_failed");
    }

    // 7. systemRole:檔案帶 role = 原樣;缺 role(舊 v2)= 確定性指派/補建;
    //    重複 role = 留第一個;存檔寫出 role
    {
        // 7a. 舊 v2(無 systemRole)、兩條 output 軌 → 第一條 monitor、第二條 stream
        const auto f3 = tmp / "legacy.rmsession";
        write_text(f3,
                   R"({"roudamixSession":2,"tracks":[)"
                   R"({"trackId":1,"kind":"output","name":"Out1","color":1,"source":null,)"
                   R"("dests":[],"output":null,"gain":1,"mute":false,"plugins":[]},)"
                   R"({"trackId":2,"kind":"output","name":"Out2","color":2,"source":null,)"
                   R"("dests":[],"output":null,"gain":1,"mute":false,"plugins":[]},)"
                   R"({"trackId":3,"kind":"audio","name":"Mic","color":3,"source":null,)"
                   R"("dests":[],"output":null,"gain":1,"mute":false,"plugins":[]}]})");
        nlohmann::json applied;
        CHECK(!rmx::session::load(e, f3, applied));
        CHECK(e.tracks().size() == 3);  // 不多建:兩條 output 軌剛好指派完
        CHECK(e.tracks()[0].name == "Out1" &&
              e.tracks()[0].system_role == rmx::SystemRole::kMonitor);
        CHECK(e.tracks()[0].latency_policy == rmx::OutputLatencyPolicy::kLowLatency);
        CHECK(e.tracks()[1].name == "Out2" &&
              e.tracks()[1].system_role == rmx::SystemRole::kStream);
        CHECK(e.tracks()[1].latency_policy == rmx::OutputLatencyPolicy::kFullPdc);
        CHECK(e.tracks()[2].system_role == rmx::SystemRole::kNone);

        // 7b. 重複 role:留第一個,第二個降級;缺 stream = 指派無 role 的 output 軌
        const auto f4 = tmp / "dup.rmsession";
        write_text(f4,
                   R"({"roudamixSession":2,"tracks":[)"
                   R"({"trackId":1,"kind":"output","name":"A","color":1,"source":null,)"
                   R"("dests":[],"output":null,"gain":1,"mute":false,"plugins":[],)"
                   R"("systemRole":"monitor"},)"
                   R"({"trackId":2,"kind":"output","name":"B","color":2,"source":null,)"
                   R"("dests":[],"output":null,"gain":1,"mute":false,"plugins":[],)"
                   R"("systemRole":"monitor"},)"
                   R"({"trackId":3,"kind":"audio","name":"C","color":3,"source":null,)"
                   R"("dests":[],"output":null,"gain":1,"mute":false,"plugins":[]}]})");
        CHECK(!rmx::session::load(e, f4, applied));
        // B 降級後被確定性指派成 stream(優先用現有軌,不新建)
        CHECK(e.tracks().size() == 3);
        int monitors = 0, streams = 0;
        for (const auto& t : e.tracks()) {
            if (t.system_role == rmx::SystemRole::kMonitor) ++monitors;
            if (t.system_role == rmx::SystemRole::kStream) ++streams;
        }
        CHECK(monitors == 1 && streams == 1);
        CHECK(e.tracks()[0].name == "A" &&
              e.tracks()[0].system_role == rmx::SystemRole::kMonitor);
        CHECK(e.tracks()[1].name == "B" &&
              e.tracks()[1].system_role == rmx::SystemRole::kStream);  // 降級後轉任 stream

        // 7c. 存檔寫出 role
        CHECK(!rmx::session::save(e, file));
        const auto j = nlohmann::json::parse(read_text(file), nullptr, false);
        CHECK(!j.is_discarded());
        int with_role = 0;
        for (const auto& t : j["tracks"]) if (!t["systemRole"].is_null()) ++with_role;
        CHECK(with_role == 2);

        // 7d. v3 明確 policy 覆蓋 role default。
        const auto f5 = tmp / "policy-v3.rmsession";
        write_text(f5,
                   R"({"roudamixSession":3,"tracks":[)"
                   R"({"trackId":1,"kind":"output","name":"M","color":1,"source":null,)"
                   R"("dests":[],"output":null,"gain":1,"mute":false,"plugins":[],)"
                   R"("systemRole":"monitor","latencyPolicy":"fullPdc"},)"
                   R"({"trackId":2,"kind":"output","name":"Aux","color":2,"source":null,)"
                   R"("dests":[],"output":null,"gain":1,"mute":false,"plugins":[],)"
                   R"("systemRole":"stream","latencyPolicy":"lowLatency"}]})");
        CHECK(!rmx::session::load(e, f5, applied));
        CHECK(e.tracks().size() == 2);
        CHECK(e.tracks()[0].latency_policy == rmx::OutputLatencyPolicy::kFullPdc);
        CHECK(e.tracks()[1].latency_policy == rmx::OutputLatencyPolicy::kLowLatency);
    }

    // 8. 系統輸出不可刪(engine 端權威;UI 只是第一道防線)
    {
        const rmx::TrackNode* sys = nullptr;
        for (const auto& t : e.tracks())
            if (t.system_role == rmx::SystemRole::kMonitor) sys = &t;
        CHECK(sys != nullptr);
        CHECK(e.track_remove(sys->track_id));
        // 一般軌照刪
        std::uint32_t plain = 0;
        CHECK(!e.track_add(rmx::TrackKind::kAudio, "Plain", 0, plain));
        CHECK(!e.track_remove(plain));
    }

    // 9. P1-F 原子寫入:temp → bak → replace;成功無 .tmp 殘留、保留一份 .bak;
    //    失敗(目錄不存在)原檔不動;.bak 可手動恢復(load 得回舊內容)
    {
        const auto f9 = tmp / "atomic.rmsession";
        const auto f9bak = tmp / "atomic.rmsession.bak";
        const auto f9tmp = tmp / "atomic.rmsession.tmp";
        std::filesystem::remove(f9);
        std::filesystem::remove(f9bak);
        std::filesystem::remove(f9tmp);
        CHECK(!rmx::session::save(e, f9));
        CHECK(std::filesystem::exists(f9));
        CHECK(!std::filesystem::exists(f9tmp));  // temp 已隨 rename 消失
        CHECK(!std::filesystem::exists(f9bak));  // 首次:無舊檔可備份
        const auto v1 = read_text(f9);

        // 改場景再存:.bak = 上一版(完整可恢復)
        std::uint32_t extra = 0;
        CHECK(!e.track_add(rmx::TrackKind::kAudio, "Extra", 0, extra));
        CHECK(!rmx::session::save(e, f9));
        CHECK(std::filesystem::exists(f9bak));
        CHECK(read_text(f9bak) == v1);  // .bak = 舊版內容
        CHECK(read_text(f9) != v1);     // 正式檔 = 新版

        // save 失敗(路徑指向不存在的目錄):原檔不動、err 帶原因
        const auto nowhere = tmp / "no_such_dir" / "x.rmsession";
        const auto save_fail = rmx::session::save(e, nowhere);
        CHECK(save_fail.has_value());
        CHECK(!save_fail->message.empty());
        CHECK(read_text(f9) != v1);

        // recovery:.bak 搬回正式檔位置 → load 得回舊場景(Extra 不在)
        std::filesystem::remove(f9);
        std::filesystem::rename(f9bak, f9);
        nlohmann::json applied;
        CHECK(!rmx::session::load(e, f9, applied));
        bool has_extra = false;
        for (const auto& t : e.tracks()) has_extra = has_extra || t.name == "Extra";
        CHECK(!has_extra);

        // 9b. 100 軌壓力(P1-H):add/dests/move/gain 正確性 + meter 預算降級
        //     (不需真 plugin/硬體:placeholder 由 load 路徑覆蓋,此處驗結構)
        std::vector<std::uint32_t> ids;
        for (int i = 0; i < 100; ++i) {
            std::uint32_t id = 0;
            CHECK(!e.track_add(rmx::TrackKind::kApp, "App" + std::to_string(i), 0, id));
            ids.push_back(id);
        }
        CHECK(e.tracks().size() >= 100);
        // 鏈狀 dests:id[i] → id[i+1](100 節點鏈,無環)
        {
            for (std::size_t i = 0; i + 1 < ids.size(); ++i)
                CHECK(!e.track_set_dests(ids[i], {ids[i + 1]}));
            // 環偵測:頭接到尾必須擋
            CHECK(e.track_set_dests(ids.back(), {ids.front()}));
        }
        // strip 預算:100 app 軌 + 系統輸出 → 只有前 63 條(master 序)有錶
        {
            const auto plan = rmx::plan_telemetry_strips(e.tracks(), rmx::kTelemetryStrips);
            CHECK(plan.tracks.size() == e.tracks().size());
            int metered = 0;
            for (const auto& p : plan.tracks) metered += p.track_strip != rmx::kNoStrip;
            CHECK(metered == 63);  // strip 0 = engine 輸出,預算剩 63 給 track
        }
        // move:把最後一條移到最前 → master 序反轉驗證
        {
            const auto last_id = ids.back();
            const auto was_first = e.tracks().front().track_id;
            CHECK(!e.track_move(last_id, 0));
            CHECK(e.tracks().front().track_id == last_id);
            CHECK(e.tracks()[1].track_id == was_first);
        }
        // 存檔/載入 100 軌 roundtrip(原子寫入路徑 + 大檔)
        {
            const auto f100 = tmp / "big100.rmsession";
            CHECK(!rmx::session::save(e, f100));
            nlohmann::json applied100;
            CHECK(!rmx::session::load(e, f100, applied100));
            CHECK(e.tracks().size() >= 100);
            int app_count = 0;
            for (const auto& t : e.tracks()) app_count += t.name.rfind("App", 0) == 0;
            CHECK(app_count == 100);
        }
    }

    std::filesystem::remove_all(tmp);
    std::printf("session_test PASSED\n");
    return 0;
}
