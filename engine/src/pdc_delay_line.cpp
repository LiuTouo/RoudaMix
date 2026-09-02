#include "pdc_delay_line.hpp"

#include <algorithm>
#include <limits>

namespace rmx {

bool PdcDelayLine::prepare(std::uint64_t max_delay_samples,
                           std::uint32_t max_block_frames) {
    const auto max_size = (std::numeric_limits<std::size_t>::max)();
    if (max_delay_samples > max_size - static_cast<std::size_t>(max_block_frames) - 1u)
        return false;
    const auto capacity = static_cast<std::size_t>(max_delay_samples) + max_block_frames + 1u;
    try {
        ring_l_.assign(capacity, 0.0F);
        ring_r_.assign(capacity, 0.0F);
    } catch (...) {
        ring_l_.clear();
        ring_r_.clear();
        return false;
    }
    max_delay_ = max_delay_samples;
    reset();
    return true;
}

void PdcDelayLine::reset() noexcept {
    std::fill(ring_l_.begin(), ring_l_.end(), 0.0F);
    std::fill(ring_r_.begin(), ring_r_.end(), 0.0F);
    write_ = 0;
    current_delay_ = target_delay_ = 0;
    transition_total_ = transition_remaining_ = 0;
    requested_delay_.store(0, std::memory_order_relaxed);
    requested_transition_.store(0, std::memory_order_relaxed);
    request_generation_.store(0, std::memory_order_relaxed);
    applied_generation_ = 0;
}

bool PdcDelayLine::set_delay(std::uint64_t delay_samples,
                             std::uint32_t transition_samples) noexcept {
    if (delay_samples > max_delay_ || ring_l_.empty()) return false;
    requested_delay_.store(delay_samples, std::memory_order_relaxed);
    requested_transition_.store(transition_samples, std::memory_order_relaxed);
    request_generation_.fetch_add(1, std::memory_order_release);
    return true;
}

void PdcDelayLine::apply_pending_request() noexcept {
    const auto generation = request_generation_.load(std::memory_order_acquire);
    if (generation == applied_generation_) return;
    applied_generation_ = generation;
    const auto requested = requested_delay_.load(std::memory_order_relaxed);
    const auto transition = requested_transition_.load(std::memory_order_relaxed);
    if (transition == 0 || requested == current_delay_) {
        current_delay_ = target_delay_ = requested;
        transition_total_ = transition_remaining_ = 0;
        return;
    }
    // 重複通知於下一個 block 重新開始 transition；from 永遠是上一個已穩定
    // delay read，不從控制執行緒直接碰 RT 的 ring/transition 狀態。
    target_delay_ = requested;
    transition_total_ = transition_remaining_ = transition;
}

float PdcDelayLine::read(const std::vector<float>& ring, std::uint64_t delay) const noexcept {
    const std::size_t d = static_cast<std::size_t>(delay % ring.size());
    const std::size_t pos = (write_ + ring.size() - d) % ring.size();
    return ring[pos];
}

void PdcDelayLine::process_add(const float* in_l, const float* in_r, float* out_l,
                               float* out_r, std::uint32_t frames) noexcept {
    if (ring_l_.empty() || out_l == nullptr || out_r == nullptr) return;
    apply_pending_request();  // runtime latency change 僅在 block boundary 生效
    for (std::uint32_t i = 0; i < frames; ++i) {
        ring_l_[write_] = in_l != nullptr ? in_l[i] : 0.0F;
        ring_r_[write_] = in_r != nullptr ? in_r[i] : 0.0F;
        float l = read(ring_l_, current_delay_);
        float r = read(ring_r_, current_delay_);
        if (transition_remaining_ > 0) {
            const float alpha = 1.0F - static_cast<float>(transition_remaining_) /
                                           static_cast<float>(transition_total_);
            l += (read(ring_l_, target_delay_) - l) * alpha;
            r += (read(ring_r_, target_delay_) - r) * alpha;
            if (--transition_remaining_ == 0) current_delay_ = target_delay_;
        }
        out_l[i] += l;
        out_r[i] += r;
        write_ = (write_ + 1u) % ring_l_.size();
    }
}

}  // namespace rmx
