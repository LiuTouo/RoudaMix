// pipe framing:u32 LE length-prefix + JSON payload(單一 WriteFile)
#pragma once
#include <windows.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

namespace rmx {

struct FrameIoError : std::runtime_error {
    explicit FrameIoError(const std::string& what) : std::runtime_error(what) {}
};

bool read_exact(HANDLE h, void* buf, size_t n);

// EOF/斷線回 nullopt;超長或壞資料丟 FrameIoError
std::optional<std::vector<uint8_t>> read_frame(HANDLE h);

bool write_frame(HANDLE h, const nlohmann::json& j);

}  // namespace rmx
