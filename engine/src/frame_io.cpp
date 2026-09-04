#include "frame_io.hpp"

#include <algorithm>

#include "protocol.hpp"

namespace rmx {

namespace {
// pipe 是 FILE_FLAG_OVERLAPPED 開的:讀(pipe thread)與寫(main thread)
// 跨 thread 並行 IO。同步 handle 上這不可靠(read pending 時他 thread 的
// write 行為未定義 —— M0 在 client 端踩過同款,解都是非同步 IO)。
// ov.hEvent 必須指派:不設時完成訊號走 handle 本身,併發 read/write 共用
// handle 會互相喚醒/漏喚醒(等錯 IO、byte 錯亂)。
struct AsyncIo {
    OVERLAPPED ov{};
    HANDLE event{};
    AsyncIo() {
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ov.hEvent = event;
    }
    ~AsyncIo() {
        if (event != nullptr) CloseHandle(event);
    }
    bool valid() const noexcept { return event != nullptr; }
};
}  // namespace

// read_exact 的 bounded 版:每個讀取區段最多等 timeout_ms,逾時 CancelIoEx。
// 認證 prelude 用 —— 未認證 client 卡住不能無限佔住唯一 pipe 槽。
bool read_exact_bounded(HANDLE h, void* buf, size_t n, DWORD timeout_ms);

bool read_exact(HANDLE h, void* buf, size_t n) {
    return read_exact_bounded(h, buf, n, INFINITE);
}

bool secret_matches(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len) {
    if (a_len != b_len) return false;
    // 常數時間:逐 byte 累積差異,單一分支在尾端
    uint8_t diff = 0;
    for (size_t i = 0; i < a_len; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

bool auth_client(HANDLE h, const std::vector<uint8_t>& secret, DWORD timeout_ms) {
    if (secret.empty()) return false;
    std::vector<uint8_t> provided(secret.size());
    if (!read_exact_bounded(h, provided.data(), provided.size(), timeout_ms)) return false;
    const bool ok = secret_matches(provided.data(), provided.size(), secret.data(),
                                   secret.size());
    // ack 一定回(1 = ok,0 = 不符);寫不出 = 呼叫端也會因讀失敗而斷
    AsyncIo io;
    if (io.valid()) {
        const uint8_t ack = ok ? 1 : 0;
        DWORD written = 0;
        if (!WriteFile(h, &ack, 1, &written, &io.ov) &&
            GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(io.ov.hEvent, 1000) == WAIT_OBJECT_0)
                GetOverlappedResult(h, &io.ov, &written, FALSE);
        }
    }
    return ok;
}

std::optional<std::vector<uint8_t>> hex_decode(const std::string& hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<uint8_t> out(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

bool read_exact_bounded(HANDLE h, void* buf, size_t n, DWORD timeout_ms) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        AsyncIo io;
        if (!io.valid()) return false;
        DWORD got = 0;
        if (!ReadFile(h, p, static_cast<DWORD>(n), &got, &io.ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(io.ov.hEvent, timeout_ms) != WAIT_OBJECT_0) {
                CancelIoEx(h, &io.ov);
                return false;
            }
            if (!GetOverlappedResult(h, &io.ov, &got, FALSE)) return false;
        }
        if (got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

std::optional<std::vector<uint8_t>> read_frame(HANDLE h) {
    uint8_t len_buf[4];
    if (!read_exact(h, len_buf, sizeof(len_buf))) return std::nullopt;
    uint32_t len = len_buf[0] | (len_buf[1] << 8) | (uint32_t(len_buf[2]) << 16) |
                   (uint32_t(len_buf[3]) << 24);
    if (len > kMaxFrameBytes) throw FrameIoError("frame exceeds 1 MiB cap");
    std::vector<uint8_t> payload(len);
    if (!read_exact(h, payload.data(), len)) return std::nullopt;
    return payload;
}

bool write_frame(HANDLE h, const nlohmann::json& j) {
    std::string s = j.dump();
    if (s.size() > kMaxFrameBytes) return false;
    std::vector<uint8_t> frame(4 + s.size());
    uint32_t len = static_cast<uint32_t>(s.size());
    frame[0] = len & 0xFF;
    frame[1] = (len >> 8) & 0xFF;
    frame[2] = (len >> 16) & 0xFF;
    frame[3] = (len >> 24) & 0xFF;
    std::copy(s.begin(), s.end(), frame.begin() + 4);
    AsyncIo io;
    if (!io.valid()) return false;
    DWORD written = 0;
    if (!WriteFile(h, frame.data(), static_cast<DWORD>(frame.size()), &written, &io.ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        // bounded:client 崩潰不讀時 pipe buffer 滿 = write 永等 —— 5s 放棄
        // (GetOverlappedResult TRUE 版無限等,main thread 不可被拖死)
        if (WaitForSingleObject(io.ov.hEvent, 5000) != WAIT_OBJECT_0) {
            CancelIoEx(h, &io.ov);
            return false;
        }
        if (!GetOverlappedResult(h, &io.ov, &written, FALSE)) return false;
    }
    return written == frame.size();
}

}  // namespace rmx
