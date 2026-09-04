#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <io.h>  // _commit(原子寫入:temp 落盤後才 replace)
#include <map>

#include "sandbox.hpp"
#include "vst_registry.hpp"

#include <windows.h>

namespace rmx::session {

namespace {
constexpr int kSessionVersion = 3;

// #14 資源預算(CWE-400/770):外部 session 檔在動 live 狀態「前」的整檔/結構
// 總量上限。超限 = 拒載(結構超限則略過超量項),live 狀態不動。
constexpr std::size_t kMaxSessionBytes = 4u * 1024 * 1024;  // 整檔 byte cap
constexpr int kMaxJsonDepth = 64;  // nlohmann 無深度上限,深巢狀 = 遞迴爆棧
// 每 plugin preflight 20s(sandbox.cpp);session 載入整體再加聚合上限,
// 防大量獨立 module 把 engine dispatch 卡住數小時。
constexpr int kMaxPreflightSeconds = 60;

// 檔案內最大 { [ 括號深度(略過字串/逸出)。nlohmann parse 是遞迴下降且無
// 深度限制,幾百 KB 的深巢狀就能爆 stack;session 結構深度 ≤ 8,64 已極寬鬆。
int max_json_depth(const std::string& text) {
    int depth = 0, max = 0;
    bool in_string = false, escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' || c == '[') max = (std::max)(max, ++depth);
        else if (c == '}' || c == ']') --depth;
    }
    return max;
}

// availability ↔ JSON:字串實作共用 rack.hpp availability_str;舊檔無此欄 = ok
using rmx::availability_str;

RackSlot::Availability availability_from(const nlohmann::json& j) {
    if (!j.is_string()) return RackSlot::Availability::kOk;
    const auto s = j.get<std::string>();
    if (s == "missing") return RackSlot::Availability::kMissing;
    if (s == "loadFailed") return RackSlot::Availability::kLoadFailed;
    return RackSlot::Availability::kOk;
}

// TrackSource/TrackOutput ↔ JSON 走 rmx::source_to_json 等(track_graph.hpp;
// 與 status 投影、router 請求解碼共用的唯一 codec)

// #13 session restore 信任閘門:檔案內的 pluginPath 是外部輸入,不信任。
// 載入 native module 前需有信任決策 = 本機絕對路徑 + (有 registry 時)掃描核准過
// 且內容未變的 registry 成員。未過閘 = placeholder,使用者按 retry 明確核准才載。
// 回 true = 可進 preflight;false = 未核准,*reason 給 missing diagnostics。
bool restore_allowed(const std::string& module_path, bool enforce_registry,
                     const vst_registry::Registry& approved, std::string& reason) {
    const auto path = vst_registry::path_from_utf8(module_path);
    // 本機磁碟路徑才收:X:\...(延伸長度前綴 \\?\X:\... 視為本機);
    // UNC(\\server)、device(\\.\)、相對路徑一律拒。
    const std::wstring wide = path.wstring();
    std::size_t i = 0;
    if (wide.rfind(L"\\\\?\\", 0) == 0) i = 4;
    const bool local = wide.size() >= i + 3 &&
                       ((wide[i] >= L'A' && wide[i] <= L'Z') ||
                        (wide[i] >= L'a' && wide[i] <= L'z')) &&
                       wide[i + 1] == L':' && (wide[i + 2] == L'\\' || wide[i + 2] == L'/');
    if (!local) {
        reason = "plugin path is not a local absolute path (UNC/remote/device paths are "
                 "rejected on session restore): " + module_path;
        return false;
    }
    // 映射遠端磁碟(net use 的 SMB/WebDAV)語法上是碟號、實際是遠端 — 一併拒
    const std::wstring drive_root{wide[i], L':', L'\\'};
    if (GetDriveTypeW(drive_root.c_str()) == DRIVE_REMOTE) {
        reason = "plugin path is on a mapped remote drive: " + module_path;
        return false;
    }
    if (!enforce_registry) return true;

    // registry 成員 + 內容未變才放行。檔案不存在(遺失)不算「未核准」,交給原
    // preflight 流程回報載不動的原因;檔案存在但 fingerprint 算不出來(bundle
    // 列舉錯誤等)= 無法證明內容未變 → 未核准(fail closed)。
    vst_registry::Fingerprint fp;
    std::string fp_err;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return true;
    if (!vst_registry::fingerprint(path, fp, fp_err)) {
        reason = "cannot fingerprint approved plugin (contents may have changed): " +
                 module_path;
        return false;
    }
    const auto key = vst_registry::path_key(path);
    for (const auto& e : approved.entries) {
        if (vst_registry::path_key(e.path) != key) continue;
        if (e.fingerprint == fp) return true;
        reason = "plugin changed on disk since it was approved — press retry to "
                 "re-approve: " + module_path;
        return false;
    }
    reason = "plugin is not in the approved VST registry (scan it, or press retry to "
             "approve): " + module_path;
    return false;
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
std::optional<Failure> save(const AudioEngine& engine, const std::filesystem::path& file,
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
        if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || f == nullptr)
            return failure(Err::kSessionIo,
                           "cannot open session temp file for writing: " + tmp.string());
        const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        const bool flushed = wrote && std::fflush(f) == 0 && _commit(_fileno(f)) == 0;
        std::fclose(f);
        if (!flushed) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);  // 寫/落盤失敗:temp 清掉,原檔不動
            return failure(Err::kSessionIo,
                           "session temp file write/flush failed: " + tmp.string());
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
            return failure(Err::kSessionIo,
                           "cannot move previous session to backup: " + bak.string() + ": " +
                               ec.message());
        }
    }

    // 3. temp → 正式檔;失敗 = .bak 搬回復原
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::error_code rb;
        if (had_old) std::filesystem::rename(bak, file, rb);
        return failure(Err::kSessionIo,
                       "cannot replace session file: " + file.string() + ": " + ec.message());
    }
    return std::nullopt;
}

sandbox::PreflightFailure preflight_plugin(const AudioEngine& engine,
                                           const std::string& module_path,
                                           const std::string& class_id, std::string& error) {
    const auto st = engine.status();
    const double rate = st.running ? static_cast<double>(st.sample_rate) : 48000.0;
    const std::uint32_t block = st.running && st.buffer_size > 0 ? st.buffer_size : 512u;
    return rmx::sandbox::preflight_module(module_path, class_id, rate, block, error);
}

std::optional<Failure> load(AudioEngine& engine, const std::filesystem::path& file,
                            nlohmann::json& applied) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || f == nullptr)
        return failure(Err::kSessionIo, "cannot open session file: " + file.string());
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
        // #14:整檔 byte cap 在 parse/驗證之前 —— 巨檔連讀都不吃完
        if (text.size() > kMaxSessionBytes) {
            std::fclose(f);
            return failure(Err::kSessionIo,
                           "session file too large (limit is 4 MiB): " + file.string());
        }
    }
    std::fclose(f);

    // 深度檢查也先於 parse:nlohmann 無深度限制,深巢狀會在 parse 內爆 stack
    if (max_json_depth(text) > kMaxJsonDepth)
        return failure(Err::kSessionIo, "session file too deeply nested: " + file.string());

    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("roudamixSession") ||
        !j["roudamixSession"].is_number_integer())
        return failure(Err::kSessionIo, "not a RoudaMix session file");
    const int file_version = j["roudamixSession"].get<int>();
    if (file_version != 2 && file_version != kSessionVersion)
        return failure(Err::kSessionIo,
                       "unsupported RoudaMix session version (expected v2 or v3)");

    // #14:軌數超上限 = 整檔拒載、狀態不動(極小 track 物件可放大成每軌 384KiB
    // RT buffer;合法檔遠低於此 — telemetry 也只有 64 strip)。clear 之前擋。
    if (j.contains("tracks") && j["tracks"].is_array() &&
        j["tracks"].size() > kMaxTracks)
        return failure(Err::kSessionIo,
                       "session has too many tracks (limit " + std::to_string(kMaxTracks) +
                           "): " + file.string());

    // 舊全軌清空(trackId/instanceId 不保留 — load 後全部重發;含系統輸出軌,
    // 載入後 ensure_system_outputs 會依檔案重建/補齊)
    engine.clear_all_tracks();

    // #13 信任閘門環境:registry 由 app spawn 時以 env 指定(spawn.rs),有 env =
    // 成員檢查開啟;env 有設但檔案壞 = 無核准清單 → fail closed(全部視為未核准)。
    // 沒設 env(直接跑 engine 的開發/探針流程)= 只擋非本機路徑,不做成員檢查。
    vst_registry::Registry approved_registry;
    bool enforce_registry = false;
    if (const auto reg_path = vst_registry::cache_path_from_env(); !reg_path.empty()) {
        enforce_registry = true;
        std::string reg_err;
        // 檔案壞:load 回 false 並清空 registry = 空核准清單,fail closed
        if (!vst_registry::load(reg_path, approved_registry, reg_err))
            std::fprintf(stderr,
                         "[engine] session restore: VST registry unusable (%s) — plugins "
                         "require explicit retry approval\n",
                         reg_err.c_str());
    }

    // #14 preflight 去重 + 聚合預算:同一 module+class 只送一次 worker(每驗一次
    // 最多 20s);整檔載入共用 kMaxPreflightSeconds 秒 worker 時間,用完後其餘
    // plugin 記 placeholder(code=preflight_budget),使用者按 retry 逐個載。
    struct PreflightVerdict {
        sandbox::PreflightFailure code{};
        std::string error;
        bool budget_exhausted{};
    };
    std::map<std::pair<std::string, std::string>, PreflightVerdict> preflights;
    const auto load_started = std::chrono::steady_clock::now();
    const auto run_preflight = [&](const std::string& p,
                                   const std::string& cid) -> PreflightVerdict {
        const auto key = std::make_pair(p, cid);
        if (const auto it = preflights.find(key); it != preflights.end()) return it->second;
        if (std::chrono::steady_clock::now() - load_started >
            std::chrono::seconds(kMaxPreflightSeconds))
            return {sandbox::PreflightFailure::kWorkerUnavailable, {}, true};
        PreflightVerdict v;
        v.code = preflight_plugin(engine, p, cid, v.error);
        return preflights.emplace(key, v).first->second;
    };

    // 逐軌重建:track_add(新 id 依序重發)→ 屬性 → plugins → dests(map 重接)
    std::map<std::uint32_t, std::uint32_t> id_map;  // 舊 id → 新 id
    std::map<std::uint32_t, TrackKind> kind_map;    // 舊 id → 檔案軌種(#11 dest 過濾)
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
            if (engine.track_add(kind, name, color, new_id)) continue;
            if (st.contains("trackId") && st["trackId"].is_number_unsigned()) {
                id_map[st["trackId"].get<std::uint32_t>()] = new_id;
                kind_map[st["trackId"].get<std::uint32_t>()] = kind;
            }
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
                (void)engine.track_set_output_latency_policy(new_id, policy);
            }

            if (st.contains("source"))
                (void)engine.track_set_source(new_id, source_from_json(st["source"]));
            if (st.contains("output"))
                (void)engine.track_set_output(new_id, output_from_json(st["output"]));
            if (st.contains("gain") && st["gain"].is_number() && st.contains("mute") &&
                st["mute"].is_boolean())
                (void)engine.track_set(new_id, std::nullopt, std::nullopt,
                                       st["gain"].get<float>(), st["mute"].get<bool>());

            // plugins:best-effort——module 消失/壞檔/worker 不在 = 原位置保留
            // placeholder(metadata/params/bypass 全存),不參與 DSP;載入走與手動
            // add_plugin 相同的 worker sandbox preflight,worker 不在 = fail closed
            if (st.contains("plugins") && st["plugins"].is_array()) {
                std::size_t chain_index = 0;
                for (const auto& sp : st["plugins"]) {
                    // #14:每軌 plugin 鏈上限(kMaxChain);超量項整個略過
                    if (chain_index >= kMaxChain) break;
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
                            // #14:每 plugin 參數表上限(kMaxParams)
                            if (params.size() >= kMaxParams) break;
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
                        (void)engine.add_placeholder_plugin(new_id, path, class_id, plug_name,
                                                            bypassed, stored, stored_err, params,
                                                            instance_id);
                        if (monitor_bypassed)
                            (void)engine.set_monitor_bypass(instance_id, true);
                        ++chain_index;
                        continue;
                    }

                    std::uint32_t instance_id = 0;
                    bool loaded = false;
                    std::string verr;
                    if (!restore_allowed(path, enforce_registry, approved_registry, verr)) {
                        // 未核准:fail closed,不送 preflight、不進 engine
                        note_missing("plugin_unapproved", verr);
                    } else if (const auto pf = run_preflight(path, class_id);
                               pf.budget_exhausted) {
                        note_missing("preflight_budget",
                                     "session restore worker budget exceeded — press "
                                     "retry to load this plugin");
                    } else if (pf.code != rmx::sandbox::PreflightFailure::kNone) {
                        note_missing(pf.code ==
                                             rmx::sandbox::PreflightFailure::kWorkerUnavailable
                                         ? "sandbox_unavailable"
                                         : "plugin_load_failed",
                                     pf.error);
                    } else if (auto fail = engine.add_plugin(new_id, path, class_id,
                                                             instance_id)) {
                        note_missing("plugin_load_failed", fail->message);
                    } else {
                        loaded = true;
                    }
                    if (loaded) {
                        if (bypassed) (void)engine.set_bypass(instance_id, true);
                        if (monitor_bypassed)
                            (void)engine.set_monitor_bypass(instance_id, true);
                        for (const auto& [pid, v] : params)
                            (void)engine.set_param(instance_id, pid, v);
                        // set_param 只餵 RT;controller 也推,開 plugin GUI 才會顯示場景值
                        engine.sync_controller_params(instance_id);
                    } else {
                        // fail closed:placeholder 佔住原鏈位,metadata/params/bypass 全存
                        std::uint32_t ph_id = 0;
                        (void)engine.add_placeholder_plugin(
                            new_id, path, class_id, plug_name, bypassed,
                            RackSlot::Availability::kLoadFailed,
                            missing.back().is_object() && missing.back().contains("message")
                                ? missing.back()["message"].get<std::string>()
                                : std::string("load failed"),
                            params, ph_id);
                        if (monitor_bypassed)
                            (void)engine.set_monitor_bypass(ph_id, true);
                    }
                    ++chain_index;
                }
            }
        }

        // dests 重接:全部軌建好後跑(舊 id → 新 id;map 不到 = 該 dest 丟棄;
        // #11:指到來源軌(audio/app)的 dest 先剔除,同軌其餘路由照常恢復)
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
                if (it == id_map.end()) continue;
                const auto kt = kind_map.find(d.get<std::uint32_t>());
                if (kt != kind_map.end() && source_kind(kt->second))
                    continue;  // 來源軌不可為目的地
                dests.push_back(it->second);
            }
            (void)engine.track_set_dests(mine->second, std::move(dests));
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
    return std::nullopt;
}

}  // namespace rmx::session
