// 漂移校正讀頭:非 ASIO clock 那側(app capture / mic 的 RT 讀端、render sink
// 的 pump 讀端共用)。ASIO 為 master clock:
//   - 讀頭以「來源樣本」為單位前進,每 dst frame 前進 srcRate/dstRate × (1+corr)
//   - corr = fill-level 回饋,clamp ±500ppm + 指數平滑(避免可聽 wow)
//   - 多相窗 sinc 內插(Blackman 窗、kTaps 分接、kPhases 相位)
//
// 表在控制面建立(create() 呼叫 prepare()),read() 只在率變動時重建 —— 重建含
// 16k 次 sin,不得出現在穩定態 RT callback。
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "spsc_fifo.hpp"

namespace rmx {

class DriftReader final {
public:
    static constexpr std::uint32_t kTaps = 64;
    static constexpr std::uint32_t kDelay = kTaps / 2;  // 輸出 = 輸入延遲 kDelay 樣本
    static constexpr std::uint32_t kPhases = 256;

    // 控制面預建內插表(非必要:read() 也會在率不符時自行建)
    void prepare(std::uint32_t src_rate, std::uint32_t dst_rate) noexcept {
        if (src_rate == 0) src_rate = dst_rate;
        if (src_rate != kernel_src_ || dst_rate != kernel_dst_)
            rebuild_kernel(src_rate, dst_rate);
    }

    void read(SpscFifo& fifo, float* l, float* r, std::uint32_t dst_frames,
              std::uint32_t src_rate, std::uint32_t dst_rate) noexcept {
        if (src_rate == 0) src_rate = dst_rate;
        if (src_rate != kernel_src_ || dst_rate != kernel_dst_)
            rebuild_kernel(src_rate, dst_rate);

        // 漂移回饋:fill level 收斂到 20ms 目標(蓋過共享模式 10ms 封包 +
        // 排程抖動;上限 cap/4 保小 fifo 測試)—— 收斂點若是容量比例(cap/2
        // = 170ms@48k),fill 會在數十分鐘內緩升到該點,聽感延遲持續累積。
        const std::size_t target =
            std::min<std::size_t>(fifo.capacity() / 4, src_rate * 20u / 1000u);
        const std::size_t avail = fifo.size();
        const double cap = static_cast<double>(fifo.capacity());
        if (avail > 0 || started_) {
            if (!started_ && avail >= target) started_ = true;  // 起播預填
            if (started_) {
                const double err = (static_cast<double>(avail) - static_cast<double>(target)) / cap;
                const double corr_target = std::clamp(err * 0.02, -5e-4, 5e-4);
                corr_ += 1e-4 * (corr_target - corr_);  // 慢平滑
            }
        }

        // overrun 消化:超過 3/4 容量 → 丟最舊到一半(reader 專屬操作)
        if (avail > cap * 0.75) fifo.drop_oldest(avail / 2);

        const double step =
            static_cast<double>(src_rate) / static_cast<double>(dst_rate) * (1.0 + corr_);
        for (std::uint32_t i = 0; i < dst_frames; ++i) {
            pos_ += step;
            while (pos_ >= 1.0) {
                float s[2];
                if (fifo.read(s, 1) != 1) {
                    // underrun:輸出靜音、讀頭與歷史重鎖(不播殘留垃圾)
                    pos_ = 0.0;
                    head_ = 0;
                    hist_l_.fill(0.0F);
                    hist_r_.fill(0.0F);
                    underruns_ += 1;
                    for (std::uint32_t j = i; j < dst_frames; ++j) {
                        l[j] = 0.0F;
                        r[j] = 0.0F;
                    }
                    return;
                }
                hist_l_[head_] = s[0];
                hist_r_[head_] = s[1];
                head_ = (head_ + 1) & (kTaps - 1);
                pos_ -= 1.0;
            }
            const float* w = kernel_[static_cast<std::size_t>(pos_ * kPhases)].data();
            float acc_l = 0.0F;
            float acc_r = 0.0F;
            std::uint32_t idx = head_;
            for (std::uint32_t j = 0; j < kTaps; ++j) {
                idx = (idx - 1) & (kTaps - 1);
                acc_l += w[j] * hist_l_[idx];
                acc_r += w[j] * hist_r_[idx];
            }
            l[i] = acc_l;
            r[i] = acc_r;
        }
    }

    std::uint64_t underruns() const noexcept { return underruns_; }

private:
    void rebuild_kernel(std::uint32_t src_rate, std::uint32_t dst_rate) noexcept {
        constexpr double kPi = 3.14159265358979323846;
        // 降率時截止在 dst Nyquist(擋 alias),升率時截止在 src Nyquist(全通)
        const double fc =
            0.5 * (std::min)(1.0, static_cast<double>(dst_rate) / static_cast<double>(src_rate));
        for (std::uint32_t phase = 0; phase < kPhases; ++phase) {
            const double p = static_cast<double>(phase) / static_cast<double>(kPhases);
            auto& row = kernel_[phase];
            double sum = 0.0;
            for (std::uint32_t j = 0; j < kTaps; ++j) {
                const double off = static_cast<double>(j) - static_cast<double>(kDelay) + p;
                const double sinc = off == 0.0 ? 2.0 * fc
                                               : std::sin(2.0 * kPi * fc * off) / (kPi * off);
                const double u = off / static_cast<double>(kDelay);
                const double window =
                    0.42 + 0.5 * std::cos(kPi * u) + 0.08 * std::cos(2.0 * kPi * u);
                row[j] = static_cast<float>(sinc * window);
                sum += row[j];
            }
            const float norm = static_cast<float>(1.0 / sum);  // 每相位 DC 增益歸一
            for (std::uint32_t j = 0; j < kTaps; ++j) row[j] *= norm;
        }
        kernel_src_ = src_rate;
        kernel_dst_ = dst_rate;
    }

    std::array<std::array<float, kTaps>, kPhases> kernel_{};
    std::uint32_t kernel_src_{0};
    std::uint32_t kernel_dst_{0};
    std::array<float, kTaps> hist_l_{};
    std::array<float, kTaps> hist_r_{};
    std::uint32_t head_{0};
    double pos_{0.0};   // 小數 = 內插相位(以 src sample 為 1)
    double corr_{0.0};
    bool started_{false};
    std::uint64_t underruns_{0};
};

}  // namespace rmx
