// session 單測(無 plugin 環境:rack 空 / 壞 slot 略過 / source roundtrip)。
// 真 plugin 的 save→load roundtrip 走 scripts/m3-session.ps1(pipe 層)。
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
        CHECK(j["roudamixSession"] == 1);
        CHECK(j["rack"].is_array() && j["rack"].empty());
        CHECK(j["deviceKey"].is_null() && j["sampleRate"].is_null());
        CHECK(j["source"] == "sine");
        CHECK(j["sineFreq"] == 440.0F);
    }

    // 2. set_source 改動 → save → 改回 → load 復原
    {
        std::string err;
        CHECK(e.set_source(/*passthrough=*/true, 880.0F, err));
        CHECK(rmx::session::save(e, file, err));
        CHECK(e.set_source(false, 440.0F, err));
        nlohmann::json applied;
        CHECK(rmx::session::load(e, file, applied, err));
        const auto st = e.status();
        CHECK(st.source == "passthrough");
        CHECK(st.sine_freq == 880.0F);
        CHECK(applied["deviceKey"].is_null() && applied["sampleRate"].is_null());
    }

    // 3. 檔案內容可讀 + 版本欄位正確
    {
        const auto j = nlohmann::json::parse(read_text(file), nullptr, false);
        CHECK(!j.is_discarded());
        CHECK(j["roudamixSession"] == 1);
        CHECK(j["source"] == "passthrough");
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
    // 5. 版本不符 → false
    {
        const auto bad = tmp / "v2.rmsession";
        write_text(bad, R"({"roudamixSession":2})");
        nlohmann::json applied;
        std::string err;
        CHECK(!rmx::session::load(e, bad, applied, err));
    }
    // 6. 壞 slot(不存在 module)略過、不整體失敗
    {
        const auto f2 = tmp / "miss.rmsession";
        write_text(f2,
                   R"({"roudamixSession":1,"rack":[{"pluginPath":"C:\\nope\\x.vst3"}],)"
                   R"("sineFreq":220.0,"source":"sine"})");
        nlohmann::json applied;
        std::string err;
        CHECK(rmx::session::load(e, f2, applied, err));
        CHECK(e.rack().empty());
        CHECK(e.status().sine_freq == 220.0F);
    }

    std::filesystem::remove_all(tmp);
    std::printf("session_test PASSED\n");
    return 0;
}
