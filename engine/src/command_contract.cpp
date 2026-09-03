#include "command_contract.hpp"

#include "command_contract_embed.hpp"

#include <cmath>
#include <cstddef>
#include <string_view>

namespace rmx::contract {

namespace {

[[noreturn]] void invalid(const std::string& path, const std::string& reason) {
    throw ValidationError(path + " " + reason);
}

const nlohmann::json& resolve_ref(const nlohmann::json& root, const std::string& ref) {
    constexpr std::string_view kPrefix = "#/definitions/";
    if (!ref.starts_with(kPrefix)) invalid("contract", "contains unsupported ref: " + ref);
    const auto name = ref.substr(kPrefix.size());
    const auto& definitions = root.at("definitions");
    if (!definitions.contains(name)) invalid("contract", "contains unknown ref: " + ref);
    return definitions.at(name);
}

void validate(const nlohmann::json& value, const nlohmann::json& schema,
              const nlohmann::json& root, const std::string& path);

bool matches(const nlohmann::json& value, const nlohmann::json& schema,
             const nlohmann::json& root, const std::string& path) {
    try {
        validate(value, schema, root, path);
        return true;
    } catch (const ValidationError&) {
        return false;
    }
}

void validate_number_bounds(const nlohmann::json& value, const nlohmann::json& schema,
                            const std::string& path) {
    if (!value.is_number()) return;
    const auto number = value.get<double>();
    if (!std::isfinite(number)) invalid(path, "must be finite");
    if (schema.contains("minimum") && number < schema.at("minimum").get<double>())
        invalid(path, "is below minimum");
    if (schema.contains("maximum") && number > schema.at("maximum").get<double>())
        invalid(path, "is above maximum");
}

void validate(const nlohmann::json& value, const nlohmann::json& schema,
              const nlohmann::json& root, const std::string& path) {
    if (schema.contains("$ref")) {
        validate(value, resolve_ref(root, schema.at("$ref").get<std::string>()), root, path);
        return;
    }
    if (schema.contains("oneOf")) {
        std::size_t count = 0;
        for (const auto& candidate : schema.at("oneOf"))
            if (matches(value, candidate, root, path)) ++count;
        if (count != 1) invalid(path, "must match exactly one allowed shape");
        return;
    }
    if (schema.contains("const") && value != schema.at("const"))
        invalid(path, "does not match the required constant");
    if (schema.contains("enum")) {
        bool found = false;
        for (const auto& candidate : schema.at("enum")) found = found || value == candidate;
        if (!found) invalid(path, "is not an allowed value");
    }

    if (!schema.contains("type")) return;
    const auto type = schema.at("type").get<std::string>();
    if (type == "object") {
        if (!value.is_object()) invalid(path, "must be object");
        if (schema.contains("maxProperties") &&
            value.size() > schema.at("maxProperties").get<std::size_t>())
            invalid(path, "has too many properties");
        if (schema.contains("required")) {
            for (const auto& key_json : schema.at("required")) {
                const auto key = key_json.get<std::string>();
                if (!value.contains(key)) invalid(path + "." + key, "is required");
            }
        }
        if (schema.contains("properties")) {
            for (auto it = schema.at("properties").begin(); it != schema.at("properties").end();
                 ++it) {
                if (value.contains(it.key()))
                    validate(value.at(it.key()), it.value(), root, path + "." + it.key());
            }
        }
        return;
    }
    if (type == "array") {
        if (!value.is_array()) invalid(path, "must be array");
        if (schema.contains("items")) {
            for (std::size_t i = 0; i < value.size(); ++i)
                validate(value.at(i), schema.at("items"), root,
                         path + "[" + std::to_string(i) + "]");
        }
        return;
    }
    if (type == "string" && !value.is_string()) invalid(path, "must be string");
    if (type == "boolean" && !value.is_boolean()) invalid(path, "must be boolean");
    if (type == "null" && !value.is_null()) invalid(path, "must be null");
    if (type == "number" && !value.is_number()) invalid(path, "must be number");
    if (type == "integer" && !value.is_number_integer() && !value.is_number_unsigned())
        invalid(path, "must be integer");
    validate_number_bounds(value, schema, path);
}

const nlohmann::json* find_kind(const char* collection, const std::string& kind) {
    for (const auto& entry : table().at(collection))
        if (entry.at("kind") == kind) return &entry;
    return nullptr;
}

void validate_command_member(const std::string& kind, const nlohmann::json& value,
                             const char* schema_key) {
    const auto* command = find_kind("commands", kind);
    if (command == nullptr) invalid("kind", "is unknown: " + kind);
    validate(value, command->at(schema_key), table(), schema_key);
}

}  // namespace

const nlohmann::json& table() {
    static const nlohmann::json value = [] {
        auto parsed = nlohmann::json::parse(kCommandContractJson, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
            throw std::runtime_error("embedded command contract is invalid JSON");
        return parsed;
    }();
    return value;
}

bool is_command_kind(const std::string& kind) { return find_kind("commands", kind) != nullptr; }

bool is_error_code(const std::string& code) {
    for (const auto& entry : table().at("errorCodes"))
        if (entry.at("code") == code) return true;
    return false;
}

bool is_event_kind(const std::string& kind) { return find_kind("events", kind) != nullptr; }

// 指令錯誤回覆的執法表:code 必須屬於該指令宣告的 errors 清單。
// 未知 kind(contract 之外,如 dispatch 前 unsupported_version 路徑)= 跳過。
bool is_declared_error(const std::string& kind, const std::string& code) {
    const auto* command = find_kind("commands", kind);
    if (command == nullptr) return true;
    for (const auto& declared : command->at("errors"))
        if (declared == code) return true;
    return false;
}

void validate_command_payload(const std::string& kind, const nlohmann::json& payload) {
    validate_command_member(kind, payload, "payload");
}

void validate_result(const std::string& kind, const nlohmann::json& result) {
    validate_command_member(kind, result, "result");
}

}  // namespace rmx::contract
