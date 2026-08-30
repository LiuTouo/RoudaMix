#include "session.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>

namespace rmx::session {

namespace {
constexpr int kSessionVersion = 2;

// ---- TrackSource/TrackOutput ↔ JSON(格式同 protocol §8)----

nlohmann::json source_to_json(const TrackSource& src) {
    switch (src.type) {
        case TrackSource::kSine:
            return nlohmann::json{{"type", "sine"}, {"freq", src.sine_freq}};
        case TrackSource::kAsioIn:
            return nlohmann::json{{"type", "asioIn"}, {"channel", src.asio_in_ch}};
        // app:存程序名不存 pid(pid 跨載入無意義;load 對不到 = 該軌靜音不 fail)
        case TrackSource::kApp:
            return src.app_name.empty()
                       ? nlohmann::json(nullptr)
                       : nlohmann::json{{"type", "app"}, {"name", src.app_name}};
        case TrackSource::kNone:
            return nullptr;
    }
    return nullptr;
}

TrackSource source_from_json(const nlohmann::json& j) {
    TrackSource src;
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return src;
    const auto t = j["type"].get<std::string>();
    if (t == "sine" && j.contains("freq") && j["freq"].is_number()) {
        src.type = TrackSource::kSine;
        src.sine_freq = j["freq"].get<float>();
    } else if (t == "asioIn" && j.contains("channel") && j["channel"].is_number_unsigned()) {
        src.type = TrackSource::kAsioIn;
        src.asio_in_ch = j["channel"].get<std::uint32_t>();
    } else if (t == "app" && j.contains("name") && j["name"].is_string()) {
        src.type = TrackSource::kApp;
        src.app_name = j["name"].get<std::string>();
    }
    return src;
}

nlohmann::json output_to_json(const TrackOutput& out) {
    switch (out.type) {
        case TrackOutput::kAsioOut:
            return nlohmann::json{{"type", "asioOut"}, {"channel", out.asio_out_ch}};
        case TrackOutput::kWasapiRender:
            return out.wasapi_id.empty()
                       ? nlohmann::json(nullptr)
                       : nlohmann::json{{"type", "wasapi"}, {"deviceId", out.wasapi_id}};
        case TrackOutput::kNone:
            return nullptr;
    }
    return nullptr;
}

TrackOutput output_from_json(const nlohmann::json& j) {
    TrackOutput out;
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return out;
    const auto t = j["type"].get<std::string>();
    if (t == "asioOut" && j.contains("channel") && j["channel"].is_number_unsigned()) {
        out.type = TrackOutput::kAsioOut;
        out.asio_out_ch = j["channel"].get<std::uint32_t>();
    } else if (t == "wasapi" && j.contains("deviceId") && j["deviceId"].is_string()) {
        out.type = TrackOutput::kWasapiRender;
        out.wasapi_id = j["deviceId"].get<std::string>();
    }
    return out;
}
}  // namespace

std::filesystem::path default_path() {
    const char* appdata = std::getenv("APPDATA");
    std::filesystem::path base = (appdata != nullptr && *appdata)
                                     ? std::filesystem::path(appdata) / "RoudaMix"
                                     : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(base, ec);  // 已存在不報錯
    return base / "default.rmsession";
}

nlohmann::json serialize(const AudioEngine& engine) {
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& t : engine.tracks()) {
        nlohmann::json plugins = nlohmann::json::array();
        for (const auto& s : t.chain) {
            nlohmann::json params = nlohmann::json::array();
            for (const auto& [id, v] : s.param_values)
                params.push_back({{"paramId", id}, {"normalized", v}});
            plugins.push_back({
                {"pluginPath", s.module_path},
                {"classId", s.class_id},
                {"name", s.name},
                {"bypassed", s.bypass},
                {"params", params},
            });
        }
        tracks.push_back({
            {"trackId", t.track_id},
            {"kind", track_kind_str(t.kind)},
            {"name", t.name},
            {"color", t.color},
            {"source", source_to_json(t.source)},
            {"dests", t.dests},
            {"output", output_to_json(t.output)},
            {"gain", t.gain},
            {"mute", t.mute},
            {"plugins", plugins},
        });
    }
    return nlohmann::json{
        {"roudamixSession", kSessionVersion},
        {"deviceKey", !engine.last_device_key().empty()
                          ? nlohmann::json(engine.last_device_key())
                          : nlohmann::json(nullptr)},
        {"sampleRate", engine.last_sample_rate() > 0 ? nlohmann::json(engine.last_sample_rate())
                                                     : nlohmann::json(nullptr)},
        {"bufferSize", engine.last_buffer_size() > 0
                           ? nlohmann::json(engine.last_buffer_size())
                           : nlohmann::json(nullptr)},
        {"tracks", tracks},
    };
}

bool save(const AudioEngine& engine, const std::filesystem::path& file, std::string& err,
          const nlohmann::json& overrides) {
    nlohmann::json j = serialize(engine);
    if (overrides.is_object()) {
        if (overrides.contains("deviceKey") && overrides["deviceKey"].is_string())
            j["deviceKey"] = overrides["deviceKey"];
        if (overrides.contains("sampleRate") && overrides["sampleRate"].is_number_unsigned())
            j["sampleRate"] = overrides["sampleRate"];
        if (overrides.contains("bufferSize") && overrides["bufferSize"].is_number_unsigned())
            j["bufferSize"] = overrides["bufferSize"];
    }
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || f == nullptr) {
        err = "cannot open session file for writing: " + file.string();
        return false;
    }
    const std::string text = j.dump(2);
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!wrote) {
        err = "session file write failed: " + file.string();
        return false;
    }
    return true;
}

bool load(AudioEngine& engine, const std::filesystem::path& file, nlohmann::json& applied,
          std::string& err) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || f == nullptr) {
        err = "cannot open session file: " + file.string();
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("roudamixSession") ||
        !j["roudamixSession"].is_number_integer() ||
        j["roudamixSession"].get<int>() != kSessionVersion) {
        err = "not a RoudaMix session v2 file (roudamixSession != 2; v1 不支援,請重建)";
        return false;
    }

    // 舊全軌清空(trackId/instanceId 不保留 — load 後全部重發)
    while (!engine.tracks().empty()) {
        std::string drop_err;
        if (!engine.track_remove(engine.tracks().back().track_id, drop_err)) break;
    }

    // 逐軌重建:track_add(新 id 依序重發)→ 屬性 → plugins → dests(map 重接)
    std::map<std::uint32_t, std::uint32_t> id_map;  // 舊 id → 新 id
    if (j.contains("tracks") && j["tracks"].is_array()) {
        for (const auto& st : j["tracks"]) {
            if (!st.is_object() || !st.contains("kind") || !st["kind"].is_string())
                continue;  // 壞軌略過不整體失敗
            const auto ks = st["kind"].get<std::string>();
            rmx::TrackKind kind = rmx::TrackKind::kAudio;
            if (ks == "app") kind = rmx::TrackKind::kApp;
            else if (ks == "fx") kind = rmx::TrackKind::kFx;
            else if (ks == "output") kind = rmx::TrackKind::kOutput;
            std::string name =
                st.contains("name") && st["name"].is_string() ? st["name"].get<std::string>() : "";
            std::uint32_t color =
                st.contains("color") && st["color"].is_number_unsigned()
                    ? st["color"].get<std::uint32_t>()
                    : 0u;
            std::uint32_t new_id = 0;
            std::string add_err;
            if (!engine.track_add(kind, name, color, new_id, add_err)) continue;
            if (st.contains("trackId") && st["trackId"].is_number_unsigned())
                id_map[st["trackId"].get<std::uint32_t>()] = new_id;

            std::string op_err, op_code;
            if (st.contains("source"))
                (void)engine.track_set_source(new_id, source_from_json(st["source"]), op_err,
                                              op_code);
            if (st.contains("output"))
                (void)engine.track_set_output(new_id, output_from_json(st["output"]), op_err,
                                              op_code);
            if (st.contains("gain") && st["gain"].is_number() && st.contains("mute") &&
                st["mute"].is_boolean())
                (void)engine.track_set(new_id, std::nullopt, std::nullopt,
                                       st["gain"].get<float>(), st["mute"].get<bool>(), op_err);

            // plugins:module 消失/載入失敗 = 略過該 plugin(軌還在)
            if (st.contains("plugins") && st["plugins"].is_array()) {
                for (const auto& sp : st["plugins"]) {
                    if (!sp.is_object() || !sp.contains("pluginPath") ||
                        !sp["pluginPath"].is_string())
                        continue;
                    std::string class_id;
                    if (sp.contains("classId") && sp["classId"].is_string())
                        class_id = sp["classId"].get<std::string>();
                    std::uint32_t instance_id = 0;
                    std::string add_pl_err;
                    if (!engine.add_plugin(new_id, sp["pluginPath"].get<std::string>(), class_id,
                                           instance_id, add_pl_err))
                        continue;
                    std::string pl_err;
                    if (sp.contains("bypassed") && sp["bypassed"].is_boolean())
                        engine.set_bypass(instance_id, sp["bypassed"].get<bool>(), pl_err);
                    if (sp.contains("params") && sp["params"].is_array()) {
                        for (const auto& p : sp["params"]) {
                            if (p.is_object() && p.contains("paramId") &&
                                p.contains("normalized") && p["paramId"].is_number_unsigned() &&
                                p["normalized"].is_number())
                                engine.set_param(instance_id, p["paramId"].get<std::uint32_t>(),
                                                 p["normalized"].get<double>(), pl_err);
                        }
                        // set_param 只餵 RT;controller 也推,開 plugin GUI 才會顯示場景值
                        engine.sync_controller_params(instance_id);
                    }
                }
            }
        }

        // dests 重接:全部軌建好後跑(舊 id → 新 id;map 不到 = 該 dest 丟棄)
        for (const auto& st : j["tracks"]) {
            if (!st.is_object() || !st.contains("trackId") ||
                !st["trackId"].is_number_unsigned() || !st.contains("dests") ||
                !st["dests"].is_array())
                continue;
            const auto old_id = st["trackId"].get<std::uint32_t>();
            const auto mine = id_map.find(old_id);
            if (mine == id_map.end()) continue;
            std::vector<std::uint32_t> dests;
            for (const auto& d : st["dests"]) {
                if (!d.is_number_unsigned()) continue;
                const auto it = id_map.find(d.get<std::uint32_t>());
                if (it != id_map.end()) dests.push_back(it->second);
            }
            std::string d_err, d_code;
            (void)engine.track_set_dests(mine->second, std::move(dests), d_err, d_code);
        }
    }

    applied = nlohmann::json{
        {"deviceKey", j.contains("deviceKey") && j["deviceKey"].is_string()
                          ? j["deviceKey"]
                          : nlohmann::json(nullptr)},
        {"sampleRate", j.contains("sampleRate") && j["sampleRate"].is_number_unsigned()
                           ? j["sampleRate"]
                           : nlohmann::json(nullptr)},
        {"bufferSize", j.contains("bufferSize") && j["bufferSize"].is_number_unsigned()
                           ? j["bufferSize"]
                           : nlohmann::json(nullptr)},
    };
    return true;
}

}  // namespace rmx::session
