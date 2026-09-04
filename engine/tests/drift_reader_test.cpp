// DriftReader 漂移校正讀頭單元測試(spec #10):以腳本化的 SpscFifo 事件序列
// (write/drop)驅動,透過公開介面 read()/underruns() 斷言輸出。純值物件,
// 無裝置、無執行緒、無 sleep。
// 時間源:read() 本身即時間 —— 每次呼叫 = 一個 dst_frames 的 RT callback,
// 呼叫序列與 rate 參數完全決定數學路徑,無內部時計可取得,故 spec 的
// 「時間源參數化」不需要;production 行為零改變。
// 案例對照漂移數學分支:同率透行(含 1 樣本內插延遲)、L/R 分離、underrun
// 靜音+重鎖、overrun 丟最舊、fill-level 回饋方向、起播預填門檻前純透行、
// 空佇列端點、src_rate=0 防禦、微率差。
// 對 spec 案例類別的取捨:「初始鎖定」由 underrun 重鎖 + 起播預填門檻兩案例
// 合併覆蓋;「佇列關閉」半邊 —— SpscFifo 無 close 語意(spec 禁新增
// production seam),僅測空佇列端點(sustained_underrun)。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include "resampler.hpp"

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                         \
        }                                                                         \
    } while (0)

namespace {

using rmx::DriftReader;
using rmx::SpscFifo;

// 交錯寫入 n 個 frame:L 從 l0 每步 +dl,R 從 r0 每步 +dr(小整數/半值,float 精確)
void fill(SpscFifo& fifo, std::uint32_t n, float l0, float dl, float r0, float dr) {
    float buf[2 * 4096];
    CHECK(n <= 4096);
    for (std::uint32_t i = 0; i < n; ++i) {
        buf[i * 2] = l0 + dl * static_cast<float>(i);
        buf[i * 2 + 1] = r0 + dr * static_cast<float>(i);
    }
    CHECK(fifo.write(buf, n) == n);
}

void drain_into(DriftReader& reader, SpscFifo& fifo, std::uint32_t frames, float* l, float* r,
                std::uint32_t src_rate, std::uint32_t dst_rate) {
    reader.read(fifo, l, r, frames, src_rate, dst_rate);
    for (std::uint32_t i = 0; i < frames; ++i) {
        CHECK(std::isfinite(l[i]));
        CHECK(std::isfinite(r[i]));
    }
}

// 同率穩態:fill(32)高於收斂目標(cap/4=16)→ 校正啟動,corr 首輪僅
// 5e-8/frame 推進 —— 輸出 = 輸入延遲 1 樣本(內插固有)加無聽感差的微移,
// 以 1e-3 容差斷言;跨 read() 呼叫 state 連續(第二輪 out[0] = 第一輪最後輸入)。
void passthrough_same_rate() {
    SpscFifo fifo(64);
    DriftReader reader;
    float l[32], r[32];

    fill(fifo, 32, 1.0F, 1.0F, 1000.0F, 2.0F);
    drain_into(reader, fifo, 16, l, r, 48000, 48000);
    CHECK(std::fabs(l[0]) < 1e-3F);  // 首樣 ≈ 初始 prev(0),1 樣本延遲由此而來
    for (std::uint32_t i = 1; i < 16; ++i) {
        CHECK(std::fabs(l[i] - static_cast<float>(i)) < 1e-3F);  // in_L[i-1] = 1+(i-1)
        CHECK(std::fabs(r[i] - (1000.0F + 2.0F * static_cast<float>(i - 1))) < 1e-3F);  // R 獨立 ramp
    }
    CHECK(fifo.size() == 16);
    CHECK(reader.underruns() == 0);

    fill(fifo, 16, 17.0F, 1.0F, 1032.0F, 2.0F);
    drain_into(reader, fifo, 16, l, r, 48000, 48000);
    CHECK(std::fabs(l[0] - 16.0F) < 1e-3F);  // 跨呼叫連續:上一輪 cur(樣本16)現為 prev
    for (std::uint32_t i = 1; i < 16; ++i) {
        CHECK(std::fabs(l[i] - (16.0F + static_cast<float>(i))) < 1e-3F);
        CHECK(std::fabs(r[i] - (1030.0F + 2.0F * static_cast<float>(i))) < 1e-3F);
    }
    CHECK(reader.underruns() == 0);
}

// 分離輸出契約:L 全 0、R 有訊號 → 任何 R 變化不得滲進 L,反之亦然。
void lr_separation() {
    SpscFifo fifo(64);
    DriftReader reader;
    float l[16], r[16];

    fill(fifo, 32, 0.0F, 0.0F, -500.0F, -1.0F);
    drain_into(reader, fifo, 16, l, r, 48000, 48000);
    for (std::uint32_t i = 1; i < 16; ++i) {
        CHECK(std::fabs(l[i]) < 1e-3F);
        CHECK(std::fabs(r[i] - (-500.0F - static_cast<float>(i - 1))) < 1e-3F);
    }
    CHECK(reader.underruns() == 0);
}

// underrun:讀空 → 剩餘輸出靜音、underruns 計數、讀頭重鎖(pos 與樣本歸零);
// 之後的新資料從新樣本重新鎖定(不播殘留垃圾)。
void underrun_silence_and_relock() {
    SpscFifo fifo(64);
    DriftReader reader;
    float l[16], r[16];

    fill(fifo, 2, 1.0F, 1.0F, 1.0F, 1.0F);
    drain_into(reader, fifo, 16, l, r, 48000, 48000);
    CHECK(l[0] == 0.0F);
    CHECK(l[1] == 1.0F);  // 唯二真樣本
    for (std::uint32_t i = 2; i < 16; ++i) {
        CHECK(l[i] == 0.0F);
        CHECK(r[i] == 0.0F);
    }
    CHECK(reader.underruns() == 1);

    // 重新鎖定:新 ramp 從 10 起,輸出應精確接上(舊 state 已歸零)
    fill(fifo, 5, 10.0F, 1.0F, 10.0F, 1.0F);
    drain_into(reader, fifo, 4, l, r, 48000, 48000);
    CHECK(l[0] == 0.0F);
    CHECK(l[1] == 10.0F);
    CHECK(l[2] == 11.0F);
    CHECK(l[3] == 12.0F);
    CHECK(r[3] == 12.0F);
    CHECK(reader.underruns() == 1);  // 重鎖後未再 underrun
}

// overrun:avail > 3/4 容量 → 丟最舊一半;輸出從保留側接上,fifo 依約縮減。
void overrun_drop_oldest() {
    SpscFifo fifo(16);
    DriftReader reader;
    float l[8], r[8];

    fill(fifo, 14, 0.0F, 1.0F, 0.0F, 1.0F);  // 14 > 12(3/4 容量)
    drain_into(reader, fifo, 4, l, r, 48000, 48000);
    CHECK(std::fabs(l[0]) < 1e-3F);            // 首樣 ≈ prev(全新 reader 恆 0):非垃圾
    CHECK(std::fabs(l[1] - 7.0F) < 1e-3F);     // 樣本 0..6 已被丟棄
    CHECK(std::fabs(l[2] - 8.0F) < 1e-3F);
    CHECK(std::fabs(l[3] - 9.0F) < 1e-3F);
    CHECK(fifo.size() == 3);  // 14 - 7(丟棄)- 4(讀)= 3;未丟則為 10
}

// fill-level 回饋方向:過滿(err>0)→ corr>0 → 讀快於同率寫 → fill 收斂下降,
// 且不震盪到 underrun。
void feedback_speeds_up_when_overfilled() {
    SpscFifo fifo(16);
    DriftReader reader;
    float l[2], r[2];

    fill(fifo, 12, 0.0F, 1.0F, 0.0F, 1.0F);  // 過滿但不觸發 overrun 丟棄(12 ≯ 12)
    float w = 13.0F;
    for (int step = 0; step < 30000; ++step) {
        float buf[2]{w, w};
        CHECK(fifo.write(buf, 1) == 1);
        w += 1.0F;
        drain_into(reader, fifo, 1, l, r, 48000, 48000);
    }
    CHECK(reader.underruns() == 0);
    CHECK(fifo.size() > 0 && fifo.size() < 12);  // 向 cap/2 收斂,方向正確
}

// 起播預填門檻:avail < cap/4 期間回饋不啟動 → corr 恆 0 → 平衡寫讀下 fill
// 恆定、輸出精確透行(初期 fill 噪聲不得推動校正)。
void prefills_below_quarter_no_correction() {
    SpscFifo fifo(16);
    DriftReader reader;
    float l[2], r[2];

    fill(fifo, 2, 0.0F, 1.0F, 0.0F, 1.0F);  // 寫入後 avail 恆 3 < 4 = cap/4
    float w = 2.0F;  // 已寫值 0,1;下一個寫入 = 2(序號連續,in[j]=j)
    for (int step = 0; step < 30000; ++step) {
        float buf[2]{w, w};
        CHECK(fifo.write(buf, 1) == 1);
        w += 1.0F;
        drain_into(reader, fifo, 1, l, r, 48000, 48000);
        // 輸出 = 輸入延遲 1(in[j]=j):第 0 次輸出為初始 prev(0),之後 = step-1
        const float expected = (step == 0) ? 0.0F : static_cast<float>(step - 1);
        CHECK(l[0] == expected);
        CHECK(r[0] == expected);
    }
    CHECK(fifo.size() == 2);  // 寫讀平衡、無校正 → fill 恆定
    CHECK(reader.underruns() == 0);
}

// 端點:空佇列(從未餵)持續讀 → 恆靜音、underruns 累加、fill 不變負。
void sustained_underrun() {
    SpscFifo fifo(16);
    DriftReader reader;
    float l[8], r[8];

    for (int round = 0; round < 3; ++round) {
        drain_into(reader, fifo, 8, l, r, 48000, 48000);
        for (int i = 0; i < 8; ++i) {
            CHECK(l[i] == 0.0F && r[i] == 0.0F);
        }
    }
    CHECK(reader.underruns() == 3);
    CHECK(fifo.size() == 0);
}

// src_rate=0 防禦:視同 dst_rate(不除零),照常透行。
void zero_src_rate_guard() {
    SpscFifo fifo(64);
    DriftReader reader;
    float l[16], r[16];

    fill(fifo, 32, 5.0F, 1.0F, 5.0F, 1.0F);
    drain_into(reader, fifo, 16, l, r, 0, 48000);
    for (std::uint32_t i = 1; i < 16; ++i) {
        CHECK(std::fabs(l[i] - (4.0F + static_cast<float>(i))) < 1e-3F);
    }
    CHECK(reader.underruns() == 0);
}

// 微率差(48048 → 48000):step>1,消耗快於供給;輸出落在已供樣本界內、
// 無 underrun、fill 依比率方向緩降。
void slight_rate_mismatch() {
    SpscFifo fifo(64);
    DriftReader reader;
    float l[16], r[16];

    fill(fifo, 30, 0.0F, 1.0F, 0.0F, 1.0F);  // 峰值(寫16後)= 46 < 48 = 3/4 容量
    float w = 30.0F;
    std::uint32_t g = 0;  // 全域輸出 frame 序號
    for (int round = 0; round < 100; ++round) {
        for (int k = 0; k < 16; ++k) {
            float buf[2]{w, w};
            CHECK(fifo.write(buf, 1) == 1);
            w += 1.0F;
        }
        drain_into(reader, fifo, 16, l, r, 48048, 48000);
        CHECK(l[5] != 0.0F);  // 有訊號(非靜音)
        for (std::uint32_t i = 0; i < 16; ++i, ++g) {
            CHECK(l[i] >= 0.0F && l[i] <= w - 1.0F);  // 內插界:不超過最新供給樣本
            // 透行追蹤:第 g 個輸出 ≈ 採樣位置 g×1.001 減 1 樣本內插延遲
            // (100 次呼叫 corr ≤ 5e-6,位置偏差可忽略;餘量 1.5 容 float 誤差)
            const double want =
                static_cast<double>(g) * (48048.0 / 48000.0) - 1.0;
            CHECK(std::fabs(static_cast<double>(l[i]) - want) <= 1.5);
        }
    }
    CHECK(reader.underruns() == 0);
    CHECK(fifo.size() > 24 && fifo.size() < 30);  // 消耗>供給:fill 下降
}

// 延遲上界(時間久了延遲累積的迴歸):真實 clock drift(+100ppm,晶體容差
// 內)長時運行,fill level = 聽感延遲,收斂後不得超過延遲預算。回饋收斂點
// 在 cap/2(170ms)的行為必紅 —— 100ppm 累積 0.048 frame/10ms,爬到 cap/4
// 起播門檻約 14 分鐘、之後滑向 fixed point,模擬視窗必須蓋過此時間尺度。
void latency_bounded_under_drift() {
    SpscFifo fifo(16384);  // production 尺寸(app_capture.hpp)
    DriftReader reader;
    float l[480], r[480];

    const std::uint32_t rate = 48000;
    const std::uint32_t packet = 480;  // 10ms 共享模式封包
    const double drift = 100e-6;       // 來源快 100ppm
    const std::size_t budget = 1440;   // 30ms @48k
    double frac = 0.0;
    float w = 0.0F;
    std::size_t max_fill_after_warmup = 0;
    for (int cb = 0; cb < 120000; ++cb) {  // 120000 × 10ms = 20 分鐘
        frac += packet * drift;
        const std::uint32_t n = packet + static_cast<std::uint32_t>(frac);
        frac -= static_cast<double>(n - packet);
        float buf[2 * 512];
        for (std::uint32_t i = 0; i < n; ++i) {
            buf[i * 2] = w;
            buf[i * 2 + 1] = w;
            w += 1.0F;
        }
        CHECK(fifo.write(buf, n) == n);  // 不得觸頂
        reader.read(fifo, l, r, packet, rate, rate);
        if (cb >= 60000) max_fill_after_warmup = std::max(max_fill_after_warmup, fifo.size());
    }
    std::printf("  max fill after 10min = %zu frames (%.1f ms)\n",
                max_fill_after_warmup, static_cast<double>(max_fill_after_warmup) / 48.0);
    CHECK(max_fill_after_warmup <= budget);
    CHECK(reader.underruns() < 100);  // 起播鎖定後不得持續 underrun
}

struct Case {
    const char* name;
    void (*fn)();
};

const Case cases[] = {
    {"passthrough_same_rate", passthrough_same_rate},
    {"lr_separation", lr_separation},
    {"underrun_silence_and_relock", underrun_silence_and_relock},
    {"overrun_drop_oldest", overrun_drop_oldest},
    {"feedback_speeds_up_when_overfilled", feedback_speeds_up_when_overfilled},
    {"prefills_below_quarter_no_correction", prefills_below_quarter_no_correction},
    {"sustained_underrun", sustained_underrun},
    {"zero_src_rate_guard", zero_src_rate_guard},
    {"slight_rate_mismatch", slight_rate_mismatch},
    {"latency_bounded_under_drift", latency_bounded_under_drift},
};

}  // namespace

int main() {
    for (const Case& c : cases) {
        c.fn();
        std::printf("[PASS] %s\n", c.name);
    }
    std::printf("drift_reader_test PASSED\n");
    return 0;
}
