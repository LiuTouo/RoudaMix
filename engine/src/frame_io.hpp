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

// ---- 連線認證 prelude(issue #16)----------------------------------------
// 連線建立後、任何 snapshot/dispatch 之前:client 先送秘密,server 常數時間
// 比較並回 1-byte ack(1 = ok,0 = 不符)。不符/逾時/斷線 = false,呼叫端必須
// 立即斷線,不得送 snapshot、不得 dispatch。

bool secret_matches(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len);

// server 端一次完整握手:讀秘密(逾時即敗)→ 比較 → 回 ack → 回報結果。
bool auth_client(HANDLE h, const std::vector<uint8_t>& secret, DWORD timeout_ms);

// 64 hex chars → 32 bytes(大小寫皆可;奇數長/非 hex 回 nullopt)。
std::optional<std::vector<uint8_t>> hex_decode(const std::string& hex);

}  // namespace rmx
