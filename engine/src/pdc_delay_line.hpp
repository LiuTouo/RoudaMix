// RT-safe stereo delay line。prepare/reset/set_delay 在 control thread；process_add
// 在 audio thread，不配置、不鎖、不呼叫系統 API。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rmx {

class PdcDelayLine final {
public:
    bool prepare(std::uint64_t max_delay_samples, std::uint32_t max_block_frames);
    void reset() noexcept;
    bool set_delay(std::uint64_t delay_samples, std::uint32_t transition_samples) noexcept;
    void process_add(const float* in_l, const float* in_r, float* out_l, float* out_r,
                     std::uint32_t frames) noexcept;

    [[nodiscard]] std::uint64_t delay_samples() const noexcept {
        return requested_delay_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t max_delay_samples() const noexcept { return max_delay_; }
    [[nodiscard]] std::size_t allocated_bytes() const noexcept {
        return (ring_l_.size() + ring_r_.size()) * sizeof(float);
    }

private:
    void apply_pending_request() noexcept;
    float read(const std::vector<float>& ring, std::uint64_t delay) const noexcept;

    std::vector<float> ring_l_, ring_r_;
    std::size_t write_{};
    std::uint64_t max_delay_{};
    std::uint64_t current_delay_{};
    std::uint64_t target_delay_{};
    std::uint32_t transition_total_{};
    std::uint32_t transition_remaining_{};
    // control thread 只發布 request；audio thread 只在 block boundary 套用。
    // 這個 seam 避免 graph rebuild 與 process_add 同時寫 transition 狀態。
    std::atomic<std::uint64_t> requested_delay_{};
    std::atomic<std::uint32_t> requested_transition_{};
    std::atomic<std::uint64_t> request_generation_{};
    std::uint64_t applied_generation_{};  // audio thread 專屬
};

}  // namespace rmx
