// session 單測(無 plugin 環境:軌道結構 roundtrip / 壞 slot 略過 / v1 拒載)。
// 真 plugin 的 save→load roundtrip 走 scripts/m5a-engine-tracks.ps1(pipe 層)。
// CHECK 而非 assert:Release/NDEBUG 下 assert 是 no-op,測試會空轉(M3 實測踩過)。
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "audio_engine.hpp"
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
        CHECK(j["roudamixSession"] == 2);
        CHECK(j["tracks"].is_array() && j["tracks"].empty());
        CHECK(j["deviceKey"].is_null() && j["sampleRate"].is_null());
    }

    // 2. 軌道結構 roundtrip:audio(sine)→fx→output 監聽,dests 鏈 + gain/mute
    {
        std::string err;
        std::uint32_t a = 0, fx = 0, o = 0;
        CHECK(e.track_add(rmx::TrackKind::kAudio, "Mic", 0x3ddc84, a, err));
        CHECK(e.track_add(rmx::TrackKind::kFx, "FX", 0, fx, err));
        CHECK(e.track_add(rmx::TrackKind::kOutput, "監聽", 0, o, err));
        rmx::TrackSource src;
        src.type = rmx::TrackSource::kSine;
        src.sine_freq = 880.0F;
        std::string err2, code;
        CHECK(e.track_set_source(a, src, err2, code));
        CHECK(e.track_set_dests(a, {fx}, err2, code));
        CHECK(e.track_set_dests(fx, {o}, err2, code));
        rmx::TrackOutput out;
        out.type = rmx::TrackOutput::kAsioOut;
        CHECK(e.track_set_output(o, out, err2, code));
        CHECK(e.track_set(a, std::nullopt, std::nullopt, 0.5F, true, err));
        CHECK(rmx::session::save(e, file, err));

        // 改掉 → load 復原
        std::string err3, code3;
        CHECK(e.track_set_dests(a, {}, err3, code3));
        CHECK(e.track_set(a, std::nullopt, std::nullopt, 1.0F, false, err3));
        nlohmann::json applied;
        CHECK(rmx::session::load(e, file, applied, err));
        CHECK(e.tracks().size() == 3);
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
        CHECK(j["roudamixSession"] == 2);
        CHECK(j["tracks"].is_array() && j["tracks"].size() == 3);
    }

    // 3b. save overrides:UI 帶 deviceKey/sampleRate 蓋寫(免 start 過)
    {
        const auto ov = tmp / "ov.rmsession";
        std::string err;
        const nlohmann::json overrides{{"deviceKey", "asio:dev1"}, {"sampleRate", 48000u}};
        CHECK(rmx::session::save(e, ov, err, overrides));
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
        std::string err;
        CHECK(!rmx::session::load(e, bad, applied, err));
    }
    // 5. v1 檔(舊 rack 格式)→ 一律拒載
    {
        const auto bad = tmp / "v1.rmsession";
        write_text(bad,
                   R"({"roudamixSession":1,"rack":[{"pluginPath":"C:\\nope\\x.vst3"}],)"
                   R"("sineFreq":220.0,"source":"sine"})");
        nlohmann::json applied;
        std::string err;
        CHECK(!rmx::session::load(e, bad, applied, err));
        // 拒載 = 狀態不動(第 2 案的 3 條軌還在)
        CHECK(e.tracks().size() == 3);
    }
    // 6. 壞 slot(不存在 module)略過、軌與路由不整體失敗
    {
        const auto f2 = tmp / "miss.rmsession";
        write_text(f2,
                   R"({"roudamixSession":2,"tracks":[{"trackId":7,"kind":"audio","name":"Mic",)"
                   R"("color":255,"source":{"type":"sine","freq":440},"dests":[],"output":null,)"
                   R"("gain":1,"mute":false,"plugins":[{"pluginPath":"C:\\nope\\x.vst3"}]}]})");
        nlohmann::json applied;
        std::string err;
        CHECK(rmx::session::load(e, f2, applied, err));
        CHECK(e.tracks().size() == 1);
        CHECK(e.tracks()[0].chain.empty());  // plugin 沒載入,軌還在
        CHECK(e.tracks()[0].name == "Mic");
        CHECK(e.tracks()[0].source.type == rmx::TrackSource::kSine);
    }

    std::filesystem::remove_all(tmp);
    std::printf("session_test PASSED\n");
    return 0;
}
