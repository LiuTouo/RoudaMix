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

constexpr int kDelay = static_cast<int>(DriftReader::kDelay);

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

// 同率穩態:fill(64)高於收斂目標(cap/4=32)→ 校正啟動,corr 首輪僅
// 5e-8/frame 推進 —— 輸出 = 輸入延遲 kDelay 樣本(內插群延遲)加無聽感差的
// 微移,以 1e-3 容差斷言;跨 read() 呼叫 state 連續(第二輪接續第一輪樣本序)。
void passthrough_same_rate() {
    SpscFifo fifo(128);
    DriftReader reader;
    float l[64], r[64];

    fill(fifo, 64, 1.0F, 1.0F, 1000.0F, 2.0F);
    drain_into(reader, fifo, 48, l, r, 48000, 48000);
    for (std::uint32_t i = 0; i < 32; ++i) CHECK(std::fabs(l[i]) < 1e-3F);  // 歷史未滿 = 靜音
    for (std::uint32_t i = 32; i < 48; ++i) {
        const float in_l = 1.0F + static_cast<float>(i) - static_cast<float>(kDelay);
        const float in_r = 1000.0F + 2.0F * (static_cast<float>(i) - static_cast<float>(kDelay));
        CHECK(std::fabs(l[i] - in_l) < 1e-3F);
        CHECK(std::fabs(r[i] - in_r) < 1e-3F);
    }
    CHECK(fifo.size() == 16);
    CHECK(reader.underruns() == 0);

    fill(fifo, 16, 65.0F, 1.0F, 1128.0F, 2.0F);
    drain_into(reader, fifo, 16, l, r, 48000, 48000);
    for (std::uint32_t i = 0; i < 16; ++i) {
        const float want_l = 17.0F + static_cast<float>(i);  // 樣本 16+i
        const float want_r = 1032.0F + 2.0F * static_cast<float>(i);
        CHECK(std::fabs(l[i] - want_l) < 1e-3F);
        CHECK(std::fabs(r[i] - want_r) < 1e-3F);
    }
    CHECK(reader.underruns() == 0);
}

// 分離輸出契約:L 全 0、R 有訊號 → 任何 R 變化不得滲進 L,反之亦然。
void lr_separation() {
    SpscFifo fifo(128);
    DriftReader reader;
    float l[64], r[64];

    fill(fifo, 64, 0.0F, 0.0F, -500.0F, -1.0F);
    drain_into(reader, fifo, 48, l, r, 48000, 48000);
    for (std::uint32_t i = 32; i < 48; ++i) {
        CHECK(std::fabs(l[i]) < 1e-3F);
        CHECK(std::fabs(r[i] - (-500.0F - (static_cast<float>(i) - 32.0F))) < 1e-3F);
    }
    CHECK(reader.underruns() == 0);
}

// underrun:讀空 → 剩餘輸出靜音、underruns 計數、讀頭重鎖(pos 與樣本歸零);
// 之後的新資料從新樣本重新鎖定(不播殘留垃圾)。
void underrun_silence_and_relock() {
    SpscFifo fifo(128);
    DriftReader reader;
    float l[64], r[64];

    fill(fifo, 34, 1.0F, 1.0F, 1.0F, 1.0F);  // 樣本 j = 1+j;延遲 32 → 只有 2 個真樣本能輸出
    drain_into(reader, fifo, 40, l, r, 48000, 48000);
    for (std::uint32_t i = 0; i < 32; ++i) CHECK(std::fabs(l[i]) < 1e-6F);
    CHECK(std::fabs(l[32] - 1.0F) < 1e-3F);
    CHECK(std::fabs(l[33] - 2.0F) < 1e-3F);
    for (std::uint32_t i = 34; i < 40; ++i) {
        CHECK(l[i] == 0.0F);
        CHECK(r[i] == 0.0F);
    }
    CHECK(reader.underruns() == 1);

    // 重新鎖定:新 ramp 從 10 起、歷史已歸零 → 重跑 kDelay 樣本後才吐真樣本
    fill(fifo, 40, 10.0F, 1.0F, 10.0F, 1.0F);
    drain_into(reader, fifo, 40, l, r, 48000, 48000);
    for (std::uint32_t i = 0; i < 32; ++i) CHECK(std::fabs(l[i]) < 1e-6F);
    for (std::uint32_t i = 32; i < 40; ++i) {
        CHECK(std::fabs(l[i] - (10.0F + static_cast<float>(i) - 32.0F)) < 1e-3F);
        CHECK(std::fabs(r[i] - (10.0F + static_cast<float>(i) - 32.0F)) < 1e-3F);
    }
    CHECK(reader.underruns() == 1);  // 重鎖後未再 underrun
}

// overrun:avail > 3/4 容量 → 丟最舊一半;輸出從保留側接上,fifo 依約縮減。
void overrun_drop_oldest() {
    SpscFifo fifo(16);
    DriftReader reader;
    float l[8], r[8];

    // 先填滿歷史(kDelay 樣本),否則延遲窗內全是初始 0,看不出丟棄
    float w = 0.0F;
    for (int k = 0; k < 40; ++k) {
        float buf[2]{w, w};
        CHECK(fifo.write(buf, 1) == 1);
        w += 1.0F;
        drain_into(reader, fifo, 1, l, r, 48000, 48000);
    }
    CHECK(fifo.size() == 0);

    fill(fifo, 14, w, 1.0F, w, 1.0F);  // 14 > 12(3/4 容量)
    drain_into(reader, fifo, 4, l, r, 48000, 48000);
    for (std::uint32_t i = 0; i < 4; ++i) {
        CHECK(std::isfinite(l[i]));
        CHECK(l[i] >= 0.0F && l[i] < w + 14.0F);
        if (i > 0) CHECK(l[i] >= l[i - 1]);  // ramp 單調:丟的是最舊,不是中間
    }
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
        // 輸出 = 輸入延遲 kDelay(in[j]=j):前 kDelay 次輸出為初始歷史(0)
        const float expected = (step < kDelay) ? 0.0F : static_cast<float>(step - kDelay);
        CHECK(std::fabs(l[0] - expected) < 1e-2F);
        CHECK(std::fabs(r[0] - expected) < 1e-2F);
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
    SpscFifo fifo(128);
    DriftReader reader;
    float l[48], r[48];

    fill(fifo, 64, 5.0F, 1.0F, 5.0F, 1.0F);
    drain_into(reader, fifo, 48, l, r, 0, 48000);
    for (std::uint32_t i = 32; i < 48; ++i) {
        CHECK(std::fabs(l[i] - (5.0F + static_cast<float>(i) - 32.0F)) < 1e-3F);
    }
    CHECK(reader.underruns() == 0);
}

// 微率差(48048 → 48000):step>1,消耗快於供給;輸出落在已供樣本界內、
// 無 underrun、fill 依比率方向緩降。
void slight_rate_mismatch() {
    SpscFifo fifo(256);
    DriftReader reader;
    float l[16], r[16];

    // fill 40 < cap/4 = 64 → 起播預填門檻未達,corr 恆 0,位置可精確預測
    fill(fifo, 40, 0.0F, 1.0F, 0.0F, 1.0F);
    float w = 40.0F;
    std::uint32_t g = 0;  // 全域輸出 frame 序號
    for (int round = 0; round < 100; ++round) {
        for (int k = 0; k < 16; ++k) {
            float buf[2]{w, w};
            CHECK(fifo.write(buf, 1) == 1);
            w += 1.0F;
        }
        drain_into(reader, fifo, 16, l, r, 48048, 48000);
        for (std::uint32_t i = 0; i < 16; ++i, ++g) {
            CHECK(l[i] > -0.1F * w && l[i] < 1.1F * w);  // 界:sinc 在階梯邊可微幅過衝
            if (g < static_cast<std::uint32_t>(kDelay)) continue;  // 歷史窗未滿
            // 透行追蹤:第 g 個輸出 ≈ 採樣位置 g×1.001 減 kDelay 群延遲
            const double want = static_cast<double>(g) * (48048.0 / 48000.0) - kDelay;
            CHECK(std::fabs(static_cast<double>(l[i]) - want) <= 1.5);
        }
    }
    CHECK(reader.underruns() == 0);
    CHECK(fifo.size() < 40 && fifo.size() > 30);  // 消耗>供給:fill 下降
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

// 通帶平整(內插品質的硬指標):48k → 44.1k 降率下餵 15kHz 正弦,
// 穩態 RMS 必須接近輸入(線性內插會是 -2.8dB ≈ 0.72 倍,會紅)。
void passband_flat_on_downsample() {
    SpscFifo fifo(16384);
    DriftReader reader;
    float l[441], r[441];

    const double tone = 15000.0;
    const double amp = 0.5;
    double phase = 0.0;
    double sum_sq = 0.0;
    std::uint64_t counted = 0;
    for (int cb = 0; cb < 200; ++cb) {
        float buf[2 * 480];
        for (std::uint32_t i = 0; i < 480; ++i) {
            const float v = static_cast<float>(amp * std::sin(phase));
            phase += 2.0 * 3.14159265358979323846 * tone / 48000.0;
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        CHECK(fifo.write(buf, 480) == 480);
        reader.read(fifo, l, r, 441, 48000, 44100);
        if (cb >= 100) {
            for (std::uint32_t i = 0; i < 441; ++i) {
                sum_sq += static_cast<double>(l[i]) * static_cast<double>(l[i]);
                ++counted;
            }
        }
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(counted));
    const double want = amp / std::sqrt(2.0);
    const double db = 20.0 * std::log10(rms / want);
    std::printf("  15kHz rms=%.5f want=%.5f (%.2f dB)\n", rms, want, db);
    CHECK(db > -0.5);  // 線性內插 = -2.8 dB
    CHECK(db < 0.5);   // 不得增益漂移
    CHECK(reader.underruns() == 0);
}

// 阻帶衰減:23kHz(> dst Nyquist 22.05k)降率後只能折回,不得原樣穿過。
void stopband_rejects_above_dst_nyquist() {
    SpscFifo fifo(16384);
    DriftReader reader;
    float l[441], r[441];

    const double tone = 23000.0;
    double phase = 0.0;
    double sum_sq = 0.0;
    std::uint64_t counted = 0;
    for (int cb = 0; cb < 200; ++cb) {
        float buf[2 * 480];
        for (std::uint32_t i = 0; i < 480; ++i) {
            const float v = static_cast<float>(0.5 * std::sin(phase));
            phase += 2.0 * 3.14159265358979323846 * tone / 48000.0;
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        CHECK(fifo.write(buf, 480) == 480);
        reader.read(fifo, l, r, 441, 48000, 44100);
        if (cb >= 100) {
            for (std::uint32_t i = 0; i < 441; ++i) {
                sum_sq += static_cast<double>(l[i]) * static_cast<double>(l[i]);
                ++counted;
            }
        }
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(counted));
    const double db = 20.0 * std::log10(rms / (0.5 / std::sqrt(2.0)));
    std::printf("  23kHz leak = %.2f dB\n", db);
    CHECK(db < -20.0);  // 線性內插 ≈ -1 dB
    CHECK(reader.underruns() == 0);
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
    {"passband_flat_on_downsample", passband_flat_on_downsample},
    {"stopband_rejects_above_dst_nyquist", stopband_rejects_above_dst_nyquist},
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
