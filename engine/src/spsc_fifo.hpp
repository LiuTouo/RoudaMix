// Lock-free SPSC 立體聲 frame ring(clock domain 跨接用:app capture pump 寫、
// ASIO RT callback 讀;M5c render sink 反向用同一顆)。
// 冪 2 容量;size() 為 relaxed 估計(只准 drift 回饋用,不得當臨界判斷)。
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace rmx {

class SpscFifo final {
public:
    explicit SpscFifo(std::size_t capacity_pow2)
        : buf_(capacity_pow2 * 2), mask_(capacity_pow2 - 1) {}

    // writer(pump thread):交錯 L/R,frames 個 frame;回傳實際寫入數(滿 = 少寫)
    std::size_t write(const float* src, std::size_t frames) noexcept {
        const std::size_t r = rpos_.load(std::memory_order_acquire);
        std::size_t w = wpos_.load(std::memory_order_relaxed);
        const std::size_t used = w - r;  // 兩計數器都只在各自端前進,wrap 以無號算術吸收
        const std::size_t cap = mask_ + 1;
        const std::size_t free_space = cap - used;
        const std::size_t n = frames < free_space ? frames : free_space;
        float* base = buf_.data();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t idx = (w & mask_) * 2;
            base[idx] = src[i * 2];
            base[idx + 1] = src[i * 2 + 1];
            ++w;
        }
        wpos_.store(w, std::memory_order_release);
        return n;
    }

    // reader(RT thread):回傳實際讀出數
    std::size_t read(float* dst, std::size_t frames) noexcept {
        const std::size_t w = wpos_.load(std::memory_order_acquire);
        std::size_t r = rpos_.load(std::memory_order_relaxed);
        const std::size_t avail = w - r;
        const std::size_t n = frames < avail ? frames : avail;
        const float* base = buf_.data();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t idx = (r & mask_) * 2;
            dst[i * 2] = base[idx];
            dst[i * 2 + 1] = base[idx + 1];
            ++r;
        }
        rpos_.store(r, std::memory_order_release);
        return n;
    }

    // reader 專屬:丟掉最舊 n 個 frame(overrun 消化)
    void drop_oldest(std::size_t n) noexcept {
        rpos_.fetch_add(n, std::memory_order_release);
    }

    // drift 回饋用(relaxed 估計;SPSC 單方向計數,不會低估太多)
    std::size_t size() const noexcept {
        return wpos_.load(std::memory_order_relaxed) - rpos_.load(std::memory_order_relaxed);
    }

    std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    std::vector<float> buf_;  // 交錯 L/R
    std::size_t mask_;
    alignas(64) std::atomic<std::size_t> wpos_{0};  // writer 寫
    alignas(64) std::atomic<std::size_t> rpos_{0};  // reader 寫
};

}  // namespace rmx
