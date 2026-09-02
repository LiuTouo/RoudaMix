#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace rmx::contract {

class ValidationError final : public std::runtime_error {
public:
    explicit ValidationError(const std::string& message) : std::runtime_error(message) {}
};

const nlohmann::json& table();
bool is_command_kind(const std::string& kind);
bool is_error_code(const std::string& code);
bool is_event_kind(const std::string& kind);
void validate_command_payload(const std::string& kind, const nlohmann::json& payload);

}  // namespace rmx::contract
