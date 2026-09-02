#include "session.hpp"

#include <cstdio>
#include <cstdlib>
#include <io.h>  // _commit(原子寫入:temp 落盤後才 replace)
#include <map>

#include "sandbox.hpp"

namespace rmx::session {

namespace {
constexpr int kSessionVersion = 3;

// availability ↔ JSON:字串實作共用 rack.hpp availability_str;舊檔無此欄 = ok
using rmx::availability_str;

RackSlot::Availability availability_from(const nlohmann::json& j) {
    if (!j.is_string()) return RackSlot::Availability::kOk;
    const auto s = j.get<std::string>();
    if (s == "missing") return RackSlot::Availability::kMissing;
    if (s == "loadFailed") return RackSlot::Availability::kLoadFailed;
    return RackSlot::Availability::kOk;
}

// ---- TrackSource/TrackOutput ↔ JSON(格式同 protocol §8)----

nlohmann::json source_to_json(const TrackSource& src) {
    switch (src.type) {
        case TrackSource::kSine:
            return nlohmann::json{{"type", "sine"}, {"freq", src.sine_freq}};
        case TrackSource::kAsioIn:
            return nlohmann::json{{"type", "asioIn"},
                                  {"channel", src.asio_in_ch},
                                  {"mono", src.mono}};
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
        if (j.contains("mono") && j["mono"].is_boolean()) src.mono = j["mono"].get<bool>();
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
                {"monitorBypassed", s.monitor_bypass},
                {"params", params},
                // placeholder 資訊:存了才不會「重新儲存把遺失 plugin 丟掉」
                {"availability", availability_str(s.availability)},
                {"loadError", s.load_error.empty() ? nlohmann::json(nullptr)
                                                   : nlohmann::json(s.load_error)},
            });
        }
        const char* role = system_role_str(t.system_role);
        tracks.push_back({
            {"trackId", t.track_id},
            {"kind", track_kind_str(t.kind)},
            {"systemRole", role != nullptr ? nlohmann::json(role) : nlohmann::json(nullptr)},
            {"latencyPolicy", output_latency_policy_str(t.latency_policy)},
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

// 原子寫入契約(P1-F):
// 1. 同目錄 <file>.tmp 完整寫入 + _commit(flush 到磁碟)→ fclose;任一步失敗 =
//    刪 temp、原檔不動(err 帶原因)。
// 2. 舊正式檔存在 = 先搬成 <file>.bak(覆蓋上輪 bak);temp → 正式檔 rename。
//    rename 失敗 = 把 .bak 搬回正式檔復原,仍失敗 = err(原檔可能遺失,.bak 還在)。
// 3. 成功:.tmp 已隨 rename 消失;保留一份 .bak(上一版,手動恢復用)。
// 效果:任何時刻中斷,正式檔要嘛完整舊版、要嘛完整新版,不會有截斷的半檔。
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
    const std::string text = j.dump(2);

    std::filesystem::path tmp = file;
    tmp += L".tmp";
    std::filesystem::path bak = file;
    bak += L".bak";

    // 1. temp 完整寫入 + 落盤
    {
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || f == nullptr) {
            err = "cannot open session temp file for writing: " + tmp.string();
            return false;
        }
        const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        const bool flushed = wrote && std::fflush(f) == 0 && _commit(_fileno(f)) == 0;
        std::fclose(f);
        if (!flushed) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);  // 寫/落盤失敗:temp 清掉,原檔不動
            err = "session temp file write/flush failed: " + tmp.string();
            return false;
        }
    }

    // 2. 舊檔 → .bak(沒有舊檔 = 首次存,免)
    std::error_code ec;
    const bool had_old = std::filesystem::exists(file, ec);
    if (had_old && !ec) {
        std::filesystem::remove(bak, ec);  // 上輪 .bak 讓位(只保一版)
        ec.clear();
        std::filesystem::rename(file, bak, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            err = "cannot move previous session to backup: " + bak.string() + ": " +
                  ec.message();
            return false;
        }
    }

    // 3. temp → 正式檔;失敗 = .bak 搬回復原
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::error_code rb;
        if (had_old) std::filesystem::rename(bak, file, rb);
        err = "cannot replace session file: " + file.string() + ": " + ec.message();
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
        !j["roudamixSession"].is_number_integer()) {
        err = "not a RoudaMix session file";
        return false;
    }
    const int file_version = j["roudamixSession"].get<int>();
    if (file_version != 2 && file_version != kSessionVersion) {
        err = "unsupported RoudaMix session version (expected v2 or v3)";
        return false;
    }

    // 舊全軌清空(trackId/instanceId 不保留 — load 後全部重發;含系統輸出軌,
    // 載入後 ensure_system_outputs 會依檔案重建/補齊)
    engine.clear_all_tracks();

    // 逐軌重建:track_add(新 id 依序重發)→ 屬性 → plugins → dests(map 重接)
    std::map<std::uint32_t, std::uint32_t> id_map;  // 舊 id → 新 id
    nlohmann::json missing = nlohmann::json::array();  // structured diagnostics
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
            // systemRole:值不合法 = kNone(ensure_system_outputs 之後會補齊)
            if (st.contains("systemRole") && st["systemRole"].is_string()) {
                const auto rs = st["systemRole"].get<std::string>();
                rmx::SystemRole role = rmx::SystemRole::kNone;
                if (rs == "monitor") role = rmx::SystemRole::kMonitor;
                else if (rs == "stream") role = rmx::SystemRole::kStream;
                engine.set_track_system_role(new_id, role);
            }
            if (file_version >= 3 && st.contains("latencyPolicy") &&
                st["latencyPolicy"].is_string()) {
                const auto ps = st["latencyPolicy"].get<std::string>();
                const auto policy = ps == "lowLatency" ? OutputLatencyPolicy::kLowLatency
                                                        : OutputLatencyPolicy::kFullPdc;
                std::string policy_err;
                (void)engine.track_set_latency_policy(new_id, policy, policy_err);
            }

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

            // plugins:best-effort——module 消失/壞檔/worker 不在 = 原位置保留
            // placeholder(metadata/params/bypass 全存),不參與 DSP;載入走與手動
            // add_plugin 相同的 worker sandbox preflight,worker 不在 = fail closed
            if (st.contains("plugins") && st["plugins"].is_array()) {
                std::size_t chain_index = 0;
                for (const auto& sp : st["plugins"]) {
                    if (!sp.is_object() || !sp.contains("pluginPath") ||
                        !sp["pluginPath"].is_string())
                        continue;
                    const std::string path = sp["pluginPath"].get<std::string>();
                    std::string class_id, plug_name;
                    if (sp.contains("classId") && sp["classId"].is_string())
                        class_id = sp["classId"].get<std::string>();
                    if (sp.contains("name") && sp["name"].is_string())
                        plug_name = sp["name"].get<std::string>();
                    const bool bypassed =
                        sp.contains("bypassed") && sp["bypassed"].is_boolean()
                            ? sp["bypassed"].get<bool>()
                            : false;
                    const bool monitor_bypassed =
                        file_version >= 3 && sp.contains("monitorBypassed") &&
                        sp["monitorBypassed"].is_boolean()
                            ? sp["monitorBypassed"].get<bool>()
                            : false;
                    // params 先收好:載入成功要重放;失敗也要跟 placeholder 一起留
                    std::vector<std::pair<std::uint32_t, double>> params;
                    if (sp.contains("params") && sp["params"].is_array()) {
                        for (const auto& p : sp["params"]) {
                            if (p.is_object() && p.contains("paramId") &&
                                p.contains("normalized") && p["paramId"].is_number_unsigned() &&
                                p["normalized"].is_number())
                                params.emplace_back(p["paramId"].get<std::uint32_t>(),
                                                    p["normalized"].get<double>());
                        }
                    }
                    const auto stored = availability_from(sp.contains("availability")
                                                              ? sp["availability"]
                                                              : nlohmann::json(nullptr));
                    const std::string stored_err =
                        sp.contains("loadError") && sp["loadError"].is_string()
                            ? sp["loadError"].get<std::string>()
                            : std::string();

                    // missing 診斷記錄器(trackId 用檔案內舊 id,對使用者有意義)
                    const auto old_tid =
                        st.contains("trackId") && st["trackId"].is_number_unsigned()
                            ? st["trackId"].get<std::uint32_t>()
                            : 0u;
                    auto note_missing = [&](const char* code, const std::string& message) {
                        missing.push_back({
                            {"trackId", old_tid},
                            {"trackName", name},
                            {"index", chain_index},
                            {"name", plug_name},
                            {"pluginPath", path},
                            {"classId", class_id},
                            {"code", code},
                            {"message", message},
                        });
                    };

                    if (stored != RackSlot::Availability::kOk) {
                        // 已知 placeholder:原樣重建,不重新試爆(檔案記過原因);
                        // 遺失清單也要列(對使用者來說它仍是壞的)
                        note_missing(stored == RackSlot::Availability::kMissing
                                         ? "plugin_missing"
                                         : "plugin_load_failed",
                                     stored_err.empty() ? "unavailable (from session file)"
                                                        : stored_err);
                        std::uint32_t instance_id = 0;
                        std::string ph_err;
                        (void)engine.add_placeholder_plugin(new_id, path, class_id, plug_name,
                                                            bypassed, stored, stored_err, params,
                                                            instance_id, ph_err);
                        if (monitor_bypassed)
                            (void)engine.set_monitor_bypass(instance_id, true, ph_err);
                        ++chain_index;
                        continue;
                    }

                    std::uint32_t instance_id = 0;
                    bool loaded = false;
                    if (rmx::sandbox::worker_path().empty()) {
                        note_missing("sandbox_unavailable",
                                     "sandbox worker (roudamix-worker.exe) unavailable; "
                                     "plugin kept as placeholder (fail closed)");
                    } else {
                        const auto st_now = engine.status();
                        const double rate =
                            st_now.running ? static_cast<double>(st_now.sample_rate) : 48000.0;
                        const std::uint32_t block =
                            st_now.running && st_now.buffer_size > 0 ? st_now.buffer_size : 512u;
                        std::string verr;
                        if (!rmx::sandbox::verify_module(path, class_id, rate, block, verr)) {
                            note_missing("plugin_load_failed", verr);
                        } else if (!engine.add_plugin(new_id, path, class_id, instance_id,
                                                      verr)) {
                            note_missing("plugin_load_failed", verr);
                        } else {
                            loaded = true;
                        }
                    }
                    if (loaded) {
                        std::string pl_err;
                        if (bypassed) (void)engine.set_bypass(instance_id, true, pl_err);
                        if (monitor_bypassed)
                            (void)engine.set_monitor_bypass(instance_id, true, pl_err);
                        for (const auto& [pid, v] : params) {
                            std::string perr;
                            (void)engine.set_param(instance_id, pid, v, perr);
                        }
                        // set_param 只餵 RT;controller 也推,開 plugin GUI 才會顯示場景值
                        engine.sync_controller_params(instance_id);
                    } else {
                        // fail closed:placeholder 佔住原鏈位,metadata/params/bypass 全存
                        std::uint32_t ph_id = 0;
                        std::string ph_err;
                        (void)engine.add_placeholder_plugin(
                            new_id, path, class_id, plug_name, bypassed,
                            RackSlot::Availability::kLoadFailed,
                            missing.back().is_object() && missing.back().contains("message")
                                ? missing.back()["message"].get<std::string>()
                                : std::string("load failed"),
                            params, ph_id, ph_err);
                        if (monitor_bypassed)
                            (void)engine.set_monitor_bypass(ph_id, true, ph_err);
                    }
                    ++chain_index;
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

        // 系統輸出(monitor/stream)唯一性 + 存在性:檔案缺 role(舊 v2)= 確定性
        // 指派/補建;重複 role = 留第一個。engine 端保證,不靠 UI
        (void)engine.ensure_system_outputs();
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
        {"missing", std::move(missing)},
    };
    return true;
}

}  // namespace rmx::session
