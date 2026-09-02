#include "protocol.hpp"

#include "command_contract.hpp"

#include <utility>

namespace rmx {

namespace {

bool is_u64(const nlohmann::json& value) {
    return value.is_number_unsigned() && value.get<uint64_t>() <= UINT64_MAX;
}

[[noreturn]] void reject(const std::string& why, std::string code = "bad_frame") {
    throw ParseError(std::move(code), why);
}

void require(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key))
        reject(std::string("missing key: ") + key);
}

Frame parse_command(const nlohmann::json& json, bool strict) {
    Command command;
    const auto& version = json.at("protocolVersion");
    if (!version.is_number_unsigned()) reject("protocolVersion must be u32");
    command.protocol_version = version.get<uint32_t>();
    if (strict && command.protocol_version != kProtocolVersion)
        reject("unsupported protocolVersion", "unsupported_version");
    if (!is_u64(json.at("id"))) reject("id must be u64");
    command.id = json.at("id").get<uint64_t>();
    command.kind = json.at("kind").get<std::string>();
    if (!contract::is_command_kind(command.kind))
        reject("unknown command kind: " + command.kind, "bad_command");
    command.payload = json.at("payload");
    try {
        contract::validate_command_payload(command.kind, command.payload);
    } catch (const contract::ValidationError& error) {
        reject(error.what(), "bad_command");
    }
    return command;
}

Frame parse_reply(const nlohmann::json& json) {
    Reply reply;
    if (!is_u64(json.at("id"))) reject("id must be u64");
    reply.id = json.at("id").get<uint64_t>();
    reply.ok = json.at("ok").get<bool>();
    if (!is_u64(json.at("epoch"))) reject("epoch must be u64");
    reply.epoch = json.at("epoch").get<uint64_t>();
    if (reply.ok) {
        require(json, "result");
        if (json.contains("error")) reject("ok reply must not carry error");
        reply.result = json.at("result");
    } else {
        require(json, "error");
        if (json.contains("result")) reject("error reply must not carry result");
        const auto& error = json.at("error");
        const auto code = error.at("code").get<std::string>();
        if (!contract::is_error_code(code)) reject("unknown error code: " + code);
        reply.error = ProtocolError{code, error.at("message").get<std::string>()};
    }
    return reply;
}

Frame parse_event(const nlohmann::json& json) {
    EngineEvent event;
    event.kind = json.at("kind").get<std::string>();
    if (!contract::is_event_kind(event.kind)) reject("unknown event kind: " + event.kind);
    event.payload = json.at("payload");
    if (!event.payload.is_object()) reject("event payload must be object");
    return event;
}

}  // namespace

Frame parse_frame(const nlohmann::json& json, bool strict) {
    try {
        if (!json.is_object()) reject("frame must be a JSON object");
        if (json.contains("protocolVersion") && json.contains("id") && json.contains("kind") &&
            json.contains("payload")) {
            return parse_command(json, strict);
        }
        if (json.contains("id") && json.contains("ok") && json.contains("epoch")) {
            require(json, "id");
            if (!json.at("ok").is_boolean()) reject("ok must be bool");
            return parse_reply(json);
        }
        if (json.contains("kind") && json.contains("payload")) return parse_event(json);
        reject("frame matches no schema variant");
    } catch (const ParseError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        reject(error.what());
    }
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
    return nlohmann::json{{"epoch", epoch},
                          {"engineVersion", kEngineVersion},
                          {"status", status},
                          {"tracks", tracks},
                          {"lastScan", nullptr}};
}

}  // namespace rmx
