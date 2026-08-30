// 漂移校正讀頭:非 ASIO clock 那側(M5b app capture 的 RT 讀端;M5c render sink
// 反向寫端共用同一數學,屆時抽出)。ASIO 為 master clock:
//   - 讀頭以「來源樣本」為單位前進,每 dst frame 前進 srcRate/dstRate × (1+corr)
//   - corr = fill-level 回饋,clamp ±500ppm + 指數平滑(避免可聽 wow)
//   - 線性內插。
// ponytail: 線性內插在非整數率比時有高頻衰減/alias 天花板;升級路徑 = cubic
// Hermite(+20 行同介面)→ r8brain-free,實測有問題再升。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "spsc_fifo.hpp"

namespace rmx {

class DriftReader final {
public:
    void read(SpscFifo& fifo, float* l, float* r, std::uint32_t dst_frames,
              std::uint32_t src_rate, std::uint32_t dst_rate) noexcept {
        if (src_rate == 0) src_rate = dst_rate;

        // 漂移回饋:fill level 收斂到容量一半(只在有資料後才開始校正)
        const std::size_t avail = fifo.size();
        const double cap = static_cast<double>(fifo.capacity());
        if (avail > 0 || started_) {
            if (!started_ && avail >= cap / 4) started_ = true;  // 起播預填
            if (started_) {
                const double err = (static_cast<double>(avail) - cap * 0.5) / cap;
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
                prev_l_ = cur_l_;
                prev_r_ = cur_r_;
                float s[2];
                if (fifo.read(s, 1) == 1) {
                    cur_l_ = s[0];
                    cur_r_ = s[1];
                } else {
                    // underrun:輸出靜音、讀頭重鎖(不播垃圾;下一輪重新收斂)
                    pos_ = 0.0;
                    prev_l_ = prev_r_ = cur_l_ = cur_r_ = 0.0F;
                    l[i] = 0.0F;
                    r[i] = 0.0F;
                    underruns_ += 1;
                    for (std::uint32_t j = i + 1; j < dst_frames; ++j) {
                        l[j] = 0.0F;
                        r[j] = 0.0F;
                    }
                    return;
                }
                pos_ -= 1.0;
            }
            l[i] = prev_l_ + (cur_l_ - prev_l_) * static_cast<float>(pos_);
            r[i] = prev_r_ + (cur_r_ - prev_r_) * static_cast<float>(pos_);
        }
    }

    std::uint64_t underruns() const noexcept { return underruns_; }

private:
    double pos_{0.0};   // 小數 = 內插係數(以 src sample 為 1)
    double corr_{0.0};
    float prev_l_{0.0F}, prev_r_{0.0F};
    float cur_l_{0.0F}, cur_r_{0.0F};
    bool started_{false};
    std::uint64_t underruns_{0};
};

}  // namespace rmx
