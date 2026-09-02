// RT-safe stereo path crossfade。control thread 只發布長度；audio thread 在下一個
// block boundary 開始套用。blend 不配置、不鎖、不呼叫系統 API。
#pragma once

#include <atomic>
#include <cstdint>

namespace rmx {

class RtCrossfade final {
public:
    void request(std::uint32_t samples) noexcept;
    void blend(const float* old_l, const float* old_r, float* next_l, float* next_r,
               std::uint32_t frames) noexcept;
    [[nodiscard]] bool active() const noexcept { return remaining_ > 0; }

private:
    std::atomic<std::uint32_t> requested_samples_{};
    std::atomic<std::uint64_t> request_generation_{};
    std::uint64_t applied_generation_{};
    std::uint32_t total_{};
    std::uint32_t remaining_{};
};

}  // namespace rmx
