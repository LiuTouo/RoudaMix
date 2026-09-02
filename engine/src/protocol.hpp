// RoudaMix engine protocol — control-plane 權威:contracts/command_contract.json
#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace rmx {

inline constexpr uint32_t kProtocolVersion = 2;
inline constexpr size_t kMaxFrameBytes = 1024 * 1024;
inline constexpr const char* kEngineVersion = "0.1.0";
inline constexpr const char* kPipeName = "\\\\.\\pipe\\roudamix-engine";
inline constexpr const wchar_t* kSingletonMutex = L"Local\\roudamix-engine-singleton";

struct ParseError : std::runtime_error {
    std::string code;

    ParseError(std::string code_value, const std::string& what)
        : std::runtime_error(what), code(std::move(code_value)) {}
};

struct ProtocolError {
    std::string code;
    std::string message;
};

struct Command {
    uint32_t protocol_version = 0;
    uint64_t id = 0;
    std::string kind;
    nlohmann::json payload;
};

struct Reply {
    uint64_t id = 0;
    bool ok = false;
    uint64_t epoch = 0;
    nlohmann::json result;                 // ok 時有效
    std::optional<ProtocolError> error;    // !ok 時有效
};

struct EngineEvent {
    std::string kind;
    nlohmann::json payload;
};

using Frame = std::variant<Command, Reply, EngineEvent>;

// strict=true:完全符合 schema(conformance 測試用)
// strict=false:結構解析,protocolVersion 不符仍回傳(呼叫端回 unsupported_version)
Frame parse_frame(const nlohmann::json& j, bool strict);

nlohmann::json make_reply_ok(uint64_t id, uint64_t epoch, nlohmann::json result);
nlohmann::json make_reply_err(uint64_t id, uint64_t epoch, std::string code, std::string message);
nlohmann::json make_event(std::string kind, nlohmann::json payload);

// §8 共用結構
nlohmann::json make_snapshot_json(uint64_t epoch, const nlohmann::json& status,
                                  const nlohmann::json& tracks);

}  // namespace rmx
