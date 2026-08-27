#include "frame_io.hpp"

#include <algorithm>

#include "protocol.hpp"

namespace rmx {

bool read_exact(HANDLE h, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        DWORD got = 0;
        if (!ReadFile(h, p, static_cast<DWORD>(n), &got, nullptr) || got == 0) return false;
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
    DWORD written = 0;
    return WriteFile(h, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) &&
           written == frame.size();
}

}  // namespace rmx
