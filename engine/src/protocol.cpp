#include "protocol.hpp"

#include <algorithm>
#include <set>

namespace rmx {

namespace {

const std::set<std::string>& command_kinds() {
    static const std::set<std::string> k = {
        "ping", "get_snapshot", "list_devices", "start", "stop", "set_source",
        "open_device_panel",
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
    } else if (kind == "set_source") {
        str("source");
        const auto src = p["source"].get<std::string>();
        if (src != "sine" && src != "passthrough")
            reject("payload.source must be sine|passthrough");
        if (!has("sineFreq") || !p["sineFreq"].is_number())
            reject("payload.sineFreq must be number");
    } else if (kind == "scan_plugins") {
        if (has("roots") && (!p["roots"].is_array() ||
                             !std::all_of(p["roots"].begin(), p["roots"].end(),
                                          [](const nlohmann::json& e) { return e.is_string(); })))
            reject("payload.roots must be string[] when present");
    } else if (kind == "add_plugin") {
        str("path");
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
                                  const nlohmann::json& rack) {
    return nlohmann::json{
        {"epoch", epoch}, {"engineVersion", kEngineVersion}, {"status", status}, {"rack", rack}, {"lastScan", nullptr}};
}

}  // namespace rmx
