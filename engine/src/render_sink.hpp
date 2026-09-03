// 串流軌 WASAPI render sink(M5c):ASIO RT callback 寫 SPSC FIFO,render pump
// thread 事件驅動 drain → 裝置(反向漂移:非 ASIO clock 那側 = pump 讀端做
// fill-level 校正,同 DriftReader 數學)。shared mode、f32 mix format。
// 裝置失效(拔除/停用)= pump 錯誤 → failed 標記 + on_fail 回呼(engine 轉
// PostMessage 進 main thread 標軌 error 並廣播 status)。
// ponytail: 不掛 IMMNotificationClient —— 拔除/停用都會讓 GetCurrentPadding/
// GetBuffer 報錯,pump 錯誤路徑已覆蓋;預設裝置切換不影響「指定 endpoint」的
// render。要事件化再加。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "failure.hpp"
#include "resampler.hpp"
#include "spsc_fifo.hpp"

namespace rmx {

class RenderSink final {
public:
    using FailCallback = std::function<void()>;

    // 啟動(阻塞;main thread STA COM)。device_id = MMDevice endpoint id;
    // src_rate = ASIO 現行率(RT 寫端率);失敗回 nullptr,failure 帶分類
    // (本層自行分類:裝置/format/啟動失敗 = device_busy)
    static std::shared_ptr<RenderSink> create(const std::string& device_id,
                                              std::uint32_t src_rate, FailCallback on_fail,
                                              Failure& failure);
    ~RenderSink();
    void stop() noexcept;

    // RT(ASIO clock):寫 FIFO(滿 = 少寫 —— reader 端 drift 會把 fill 拉回)
    void write(const float* l, const float* r, std::uint32_t frames) noexcept {
        // 就地交錯進棧緩衝(成員、RT 專屬,無配置)
        for (std::uint32_t i = 0; i < frames; ++i) {
            inter_[i * 2] = l[i];
            inter_[i * 2 + 1] = r[i];
        }
        (void)fifo_.write(inter_, frames);
    }

    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& pump_error() const noexcept { return pump_error_; }
    [[nodiscard]] std::uint32_t device_rate() const noexcept { return dst_rate_; }

private:
    RenderSink() = default;
    void pump();

    SpscFifo fifo_{1u << 14};  // 16384 frames ≈ 340ms @48k
    DriftReader reader_;       // pump 端(讀 = 非 ASIO clock 側)做漂移
    float inter_[2 * 8192]{};  // RT 交錯緩衝(上限 = kMaxBlockFrames)
    std::uint32_t src_rate_{48000};
    std::uint32_t dst_rate_{48000};
    std::uint32_t channels_{2};  // 裝置 mix format 聲道數(pump 寫 ch0/1)
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::string pump_error_;
    FailCallback on_fail_;
    std::thread pump_thread_;
    // pump thread 專屬(create 建好後移交,quit 時 pump 自己收)
    void* audio_client_{nullptr};   // IAudioClient*
    void* render_client_{nullptr};  // IAudioRenderClient*
    void* event_{nullptr};          // HANDLE
    std::uint32_t buffer_frames_{0};
};

}  // namespace rmx
