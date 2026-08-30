#include "protocol.hpp"

#include <algorithm>
#include <set>

namespace rmx {

namespace {

const std::set<std::string>& command_kinds() {
    static const std::set<std::string> k = {
        "ping", "get_snapshot", "list_devices", "list_audio_apps", "list_render_devices",
        "start", "stop", "open_device_panel",
        "track_add", "track_remove", "track_set", "track_set_source",
        "track_set_dests", "track_set_output", "track_move",
        "scan_plugins", "add_plugin", "remove_plugin", "move_plugin",
        "set_bypass", "set_param", "get_params", "open_editor", "close_editor",
        "save_preset", "load_preset",
        "save_session", "load_session", "shutdown_engine",
        "set_editor_owner",
    };
    return k;
}

const std::set<std::string>& event_kinds() {
    static const std::set<std::string> k = {
        "snapshot", "status", "scan_done", "rack_changed", "plugin_event",
        "devices_changed",
    };
    return k;
}

const std::set<std::string>& error_codes() {
    static const std::set<std::string> k = {
        "unsupported_version", "bad_frame", "bad_command", "not_running",
        "already_running", "device_open_failed", "device_lost",
        "track_not_found", "cycle_detected", "device_busy",
        "app_not_found", "unsupported_windows",
        "plugin_not_found", "plugin_load_failed", "plugin_no_editor",
        "param_not_found", "session_io", "preset_io", "plugin_state_failed",
        "internal",
    };
    return k;
}

bool is_u64(const nlohmann::json& j) {
    return j.is_number_unsigned() && j.get<uint64_t>() <= UINT64_MAX;
}
bool is_u32(const nlohmann::json& j) {
    return j.is_number_unsigned() && j.get<uint64_t>() <= 4294967295ULL;
}

[[noreturn]] void reject(const std::string& why) { throw ParseError(why); }

void require(const nlohmann::json& o, const char* key) {
    if (!o.is_object() || !o.contains(key)) reject(std::string("missing key: ") + key);
}

// TrackSource:null | {type:"sine",freq} | {type:"asioIn",channel} | {type:"app",pid,name?}
void validate_track_source(const nlohmann::json& s) {
    if (s.is_null()) return;
    if (!s.is_object()) reject("payload.source must be null or object");
    require(s, "type");
    if (!s["type"].is_string()) reject("payload.source.type must be string");
    const auto t = s["type"].get<std::string>();
    if (t == "sine") {
        require(s, "freq");
        if (!s["freq"].is_number()) reject("payload.source.freq must be number");
    } else if (t == "asioIn") {
        require(s, "channel");
        if (!is_u32(s["channel"])) reject("payload.source.channel must be u32");
    } else if (t == "app") {
        require(s, "pid");
        if (!is_u32(s["pid"])) reject("payload.source.pid must be u32");
        if (s.contains("name") && !s["name"].is_string())
            reject("payload.source.name must be string when present");
    } else {
        reject("payload.source.type must be sine|asioIn|app");
    }
}

// TrackOutput:null | {type:"asioOut",channel} | {type:"wasapi",deviceId}
void validate_track_output(const nlohmann::json& o) {
    if (o.is_null()) return;
    if (!o.is_object()) reject("payload.output must be null or object");
    require(o, "type");
    if (!o["type"].is_string()) reject("payload.output.type must be string");
    const auto t = o["type"].get<std::string>();
    if (t == "asioOut") {
        require(o, "channel");
        if (!is_u32(o["channel"])) reject("payload.output.channel must be u32");
    } else if (t == "wasapi") {
        require(o, "deviceId");
        if (!o["deviceId"].is_string()) reject("payload.output.deviceId must be string");
    } else {
        reject("payload.output.type must be asioOut|wasapi");
    }
}

// 檢查 payload 语义(schema per-kind payload 規則)
void validate_command_payload(const std::string& kind, const nlohmann::json& p) {
    if (!p.is_object()) reject("payload must be object");
    auto has = [&](const char* k) { return p.contains(k); };
    auto str = [&](const char* k) {
        if (!has(k) || !p[k].is_string()) reject(std::string("payload.") + k + " must be string");
    };
    auto u32 = [&](const char* k) {
        if (!has(k) || !is_u32(p[k])) reject(std::string("payload.") + k + " must be u32");
    };

    if (kind == "start") {
        str("deviceKey");
        if (!has("sampleRate") || !(p["sampleRate"].is_null() || is_u32(p["sampleRate"])))
            reject("payload.sampleRate must be u32 or null");
        if (has("bufferSize") && !(p["bufferSize"].is_null() || is_u32(p["bufferSize"])))
            reject("payload.bufferSize must be u32 or null");
    } else if (kind == "track_add") {
        str("kind");
        const auto k = p["kind"].get<std::string>();
        if (k != "audio" && k != "app" && k != "fx" && k != "output")
            reject("payload.kind must be audio|app|fx|output");
        if (has("name") && !p["name"].is_string())
            reject("payload.name must be string when present");
        if (has("color") && !is_u32(p["color"]))
            reject("payload.color must be u32 when present");
    } else if (kind == "track_remove" || kind == "track_move") {
        u32("trackId");
        if (kind == "track_move") {
            u32("newIndex");
        }
    } else if (kind == "track_set") {
        u32("trackId");
        if (has("name") && !p["name"].is_string())
            reject("payload.name must be string when present");
        if (has("color") && !is_u32(p["color"]))
            reject("payload.color must be u32 when present");
        if (has("gain") && (!p["gain"].is_number() || p["gain"].get<double>() < 0.0 ||
                            p["gain"].get<double>() > 4.0))
            reject("payload.gain must be number in [0,4]");
        if (has("mute") && !p["mute"].is_boolean())
            reject("payload.mute must be bool when present");
    } else if (kind == "track_set_source") {
        u32("trackId");
        if (!has("source")) reject("missing key: payload.source");
        validate_track_source(p["source"]);
    } else if (kind == "track_set_dests") {
        u32("trackId");
        if (!has("dests") || !p["dests"].is_array() ||
            !std::all_of(p["dests"].begin(), p["dests"].end(), is_u32))
            reject("payload.dests must be u32[]");
    } else if (kind == "track_set_output") {
        u32("trackId");
        if (!has("output")) reject("missing key: payload.output");
        validate_track_output(p["output"]);
    } else if (kind == "scan_plugins") {
        if (has("roots") && (!p["roots"].is_array() ||
                             !std::all_of(p["roots"].begin(), p["roots"].end(),
                                          [](const nlohmann::json& e) { return e.is_string(); })))
            reject("payload.roots must be string[] when present");
    } else if (kind == "add_plugin") {
        u32("trackId");
        str("path");
        if (has("classId") && !p["classId"].is_string())
            reject("payload.classId must be string when present");
    } else if (kind == "remove_plugin" || kind == "get_params" || kind == "open_editor" ||
               kind == "close_editor") {
        u32("instanceId");
    } else if (kind == "move_plugin") {
        u32("instanceId");
        u32("newIndex");
    } else if (kind == "set_bypass") {
        u32("instanceId");
        if (!has("bypassed") || !p["bypassed"].is_boolean())
            reject("payload.bypassed must be bool");
    } else if (kind == "set_param") {
        u32("instanceId");
        u32("paramId");
        if (!has("value") || !p["value"].is_number() || p["value"].get<double>() < 0.0 ||
            p["value"].get<double>() > 1.0)
            reject("payload.value must be number in [0,1]");
    } else if (kind == "save_preset" || kind == "load_preset") {
        u32("instanceId");
        str("path");
    } else if (kind == "save_session") {
        if (!has("path") || !(p["path"].is_string() || p["path"].is_null()))
            reject("payload.path must be string or null");
    } else if (kind == "load_session") {
        str("path");
    } else if (kind == "set_editor_owner") {
        // 主視窗 HWND(engine 是背景 process,owner 讓 editor host 變 owned
        // 浮動視窗:無工作列項、隨主程式最小化)。u64:HWND 64-bit 上 8 bytes
        if (!has("hwnd") || !is_u64(p["hwnd"])) reject("payload.hwnd must be u64");
    } else if (kind == "ping" || kind == "get_snapshot" || kind == "list_devices" ||
               kind == "list_audio_apps" || kind == "list_render_devices" ||
               kind == "stop" || kind == "shutdown_engine" || kind == "open_device_panel") {
        if (!p.empty()) reject("payload must be empty object");
    }
}

Frame parse_command(const nlohmann::json& j, bool strict) {
    Command c;
    const auto& v = j.at("protocolVersion");
    if (!v.is_number_unsigned()) reject("protocolVersion must be u32");
    c.protocol_version = v.get<uint32_t>();
    if (strict && c.protocol_version != kProtocolVersion) reject("unsupported protocolVersion");
    if (!is_u64(j.at("id"))) reject("id must be u64");
    c.id = j.at("id").get<uint64_t>();
    c.kind = j.at("kind").get<std::string>();
    if (command_kinds().count(c.kind) == 0) reject("unknown command kind: " + c.kind);
    c.payload = j.at("payload");
    validate_command_payload(c.kind, c.payload);
    return c;
}

Frame parse_reply(const nlohmann::json& j) {
    Reply r;
    if (!is_u64(j.at("id"))) reject("id must be u64");
    r.id = j.at("id").get<uint64_t>();
    r.ok = j.at("ok").get<bool>();
    if (!is_u64(j.at("epoch"))) reject("epoch must be u64");
    r.epoch = j.at("epoch").get<uint64_t>();
    if (r.ok) {
        require(j, "result");
        if (j.contains("error")) reject("ok reply must not carry error");
        r.result = j.at("result");
    } else {
        require(j, "error");
        if (j.contains("result")) reject("error reply must not carry result");
        const auto& e = j.at("error");
        std::string code = e.at("code").get<std::string>();
        if (error_codes().count(code) == 0) reject("unknown error code: " + code);
        r.error = ProtocolError{code, e.at("message").get<std::string>()};
    }
    return r;
}

Frame parse_event(const nlohmann::json& j) {
    EngineEvent ev;
    ev.kind = j.at("kind").get<std::string>();
    if (event_kinds().count(ev.kind) == 0) reject("unknown event kind: " + ev.kind);
    ev.payload = j.at("payload");
    if (!ev.payload.is_object()) reject("event payload must be object");
    return ev;
}

}  // namespace

Frame parse_frame(const nlohmann::json& j, bool strict) {
    if (!j.is_object()) reject("frame must be a JSON object");
    if (j.contains("protocolVersion") && j.contains("id") && j.contains("kind") &&
        j.contains("payload")) {
        return parse_command(j, strict);
    }
    if (j.contains("id") && j.contains("ok") && j.contains("epoch")) {
        require(j, "id");
        if (!j.at("ok").is_boolean()) reject("ok must be bool");
        return parse_reply(j);
    }
    if (j.contains("kind") && j.contains("payload")) {
        return parse_event(j);
    }
    reject("frame matches no schema variant");
}

nlohmann::json make_reply_ok(uint64_t id, uint64_t epoch, nlohmann::json result) {
    return nlohmann::json{{"id", id}, {"ok", true}, {"epoch", epoch}, {"result", std::move(result)}};
}

nlohmann::json make_reply_err(uint64_t id, uint64_t epoch, std::string code,
                              std::string message) {
    return nlohmann::json{{"id", id},
                          {"ok", false},
                          {"epoch", epoch},
                          {"error", {{"code", std::move(code)}, {"message", std::move(message)}}}};
}

nlohmann::json make_event(std::string kind, nlohmann::json payload) {
    return nlohmann::json{{"kind", std::move(kind)}, {"payload", std::move(payload)}};
}

nlohmann::json make_snapshot_json(uint64_t epoch, const nlohmann::json& status,
                                  const nlohmann::json& tracks) {
    return nlohmann::json{
        {"epoch", epoch}, {"engineVersion", kEngineVersion}, {"status", status}, {"tracks", tracks}, {"lastScan", nullptr}};
}

}  // namespace rmx
