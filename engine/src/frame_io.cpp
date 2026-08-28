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

bool read_exact(HANDLE h, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        AsyncIo io;
        if (!io.valid()) return false;
        DWORD got = 0;
        if (!ReadFile(h, p, static_cast<DWORD>(n), &got, &io.ov)) {
            if (GetLastError() != ERROR_IO_PENDING ||
                !GetOverlappedResult(h, &io.ov, &got, TRUE))
                return false;
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
