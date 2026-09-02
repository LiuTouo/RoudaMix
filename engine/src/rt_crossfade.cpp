#include "rt_crossfade.hpp"

namespace rmx {

void RtCrossfade::request(std::uint32_t samples) noexcept {
    requested_samples_.store(samples, std::memory_order_relaxed);
    request_generation_.fetch_add(1, std::memory_order_release);
}

void RtCrossfade::blend(const float* old_l, const float* old_r, float* next_l,
                        float* next_r, std::uint32_t frames) noexcept {
    const auto generation = request_generation_.load(std::memory_order_acquire);
    if (generation != applied_generation_) {
        applied_generation_ = generation;
        total_ = remaining_ = requested_samples_.load(std::memory_order_relaxed);
    }
    if (remaining_ == 0 || total_ == 0 || old_l == nullptr || old_r == nullptr ||
        next_l == nullptr || next_r == nullptr)
        return;
    for (std::uint32_t i = 0; i < frames && remaining_ > 0; ++i) {
        const float alpha = 1.0F -
                            static_cast<float>(remaining_) / static_cast<float>(total_);
        next_l[i] = old_l[i] + (next_l[i] - old_l[i]) * alpha;
        next_r[i] = old_r[i] + (next_r[i] - old_r[i]) * alpha;
        --remaining_;
    }
}

}  // namespace rmx
