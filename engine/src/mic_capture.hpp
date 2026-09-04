// Mic 軌 WASAPI capture(M6):一般 capture endpoint(eCapture;空 device_id =
// 系統預設麥克風)。create() 在控制面(main thread)阻塞做 activation/format
// 協商;pump thread 事件驅動收 buffer 寫 SPSC FIFO;RT callback 經 DriftReader
// 讀(fill-level 漂移校正,同 app 軌模式 —— ASIO master 下 mic 是漂移側)。
// 裝置失效(拔除/停用)= pump 錯誤 → failed 標記 + on_fail 回呼(engine 轉
// PostMessage 進 main thread 標軌 error 並廣播 status)。
// 啟用樣式刻意複製 RenderSink(endpoint-id 啟用)與 AppCapture(drain pump):
// 兩者 pump 各有專屬職責(無 loopback flag / 程序驗活),不抽共用 base。
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

class MicCapture final {
public:
    using FailCallback = std::function<void()>;

    // 啟動(阻塞;main thread STA COM)。device_id = MMDevice endpoint id,
    // 空 = 預設麥克風(eCapture/eMultimedia);失敗回 nullptr,failure 帶分類
    // (本層自行分類:裝置/format/啟動失敗 = device_busy)
    static std::shared_ptr<MicCapture> create(const std::string& device_id,
                                              std::uint32_t dst_rate, FailCallback on_fail,
                                              Failure& failure);
    ~MicCapture();
    void stop() noexcept;

    // RT(dst_rate = engine 現行率):無資料 = 靜音(underrun 重鎖)
    void read(float* l, float* r, std::uint32_t frames, std::uint32_t dst_rate) noexcept {
        reader_.read(fifo_, l, r, frames, src_rate_, dst_rate);
    }

    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& pump_error() const noexcept { return pump_error_; }

private:
    MicCapture() = default;
    void pump();

    SpscFifo fifo_{1u << 14};  // 16384 frames ≈ 340ms @48k
    DriftReader reader_;       // RT 端(讀 = 非 capture clock 側)做漂移
    std::uint32_t src_rate_{48000};
    std::uint32_t dst_rate_{48000};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::string pump_error_;
    FailCallback on_fail_;
    std::thread pump_thread_;
    // pump thread 專屬(create 建好後移交,quit 時 pump 自己收)
    void* audio_client_{nullptr};    // IAudioClient*
    void* capture_client_{nullptr};  // IAudioCaptureClient*
    void* event_{nullptr};           // HANDLE
    std::uint32_t channels_{2};      // 裝置 mix format 聲道數(取 ch0/1)
};

}  // namespace rmx
