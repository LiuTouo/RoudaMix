// App 軌 process loopback capture(M5b):ActivateAudioInterfaceAsync + 程序
// loopback 虛擬裝置(Win10 2004+)。create() 在控制面(main thread)阻塞做
// activation(通常 <100ms、上限 2s);pump thread 事件驅動收 buffer 寫 SPSC FIFO;
// ASIO RT callback 經 DriftReader 讀(fill-level 漂移校正)。
// 目標程序結束/裝置失效 = pump 退出 + failed 標記 + on_fail 回呼(engine 轉
// PostMessage 進 main thread 標軌 error 並廣播 status)。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "resampler.hpp"
#include "spsc_fifo.hpp"

namespace rmx {

// 程序列舉(app 來源驗證/pid 名稱重解析;Toolhelp 免 COM)
[[nodiscard]] bool process_exists(std::uint32_t pid) noexcept;
[[nodiscard]] std::vector<std::pair<std::uint32_t, std::string>> list_process_full_paths();

class AppCapture final {
public:
    using FailCallback = std::function<void()>;

    // 啟動(阻塞);失敗回 nullptr,err 帶原因(錯誤碼由呼叫端對應:
    // "process loopback not supported" → unsupported_windows、"process not found"
    // → app_not_found,其餘 internal)
    static std::shared_ptr<AppCapture> create(std::uint32_t pid, std::uint32_t dst_rate,
                                              FailCallback on_fail, std::string& err);
    ~AppCapture();
    void stop() noexcept;

    // RT(ASIO clock):dst_rate = engine 現行率;無資料 = 靜音(underrun 重鎖)
    void read(float* l, float* r, std::uint32_t frames, std::uint32_t dst_rate) noexcept {
        reader_.read(fifo_, l, r, frames, src_rate_, dst_rate);
    }

    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& pump_error() const noexcept { return pump_error_; }

private:
    AppCapture() = default;
    void pump();

    SpscFifo fifo_{1u << 14};  // 16384 frames ≈ 340ms @48k
    DriftReader reader_;
    std::uint32_t src_rate_{48000};
    std::uint32_t dst_rate_{48000};
    // 目標程序存活判定:自開 SYNCHRONIZE handle(OpenProcess 對「死但被系統
    // 持 handle」的程序仍成功 —— loopback activation 自己就持有一個)
    void* target_process_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::string pump_error_;
    FailCallback on_fail_;
    std::thread pump_thread_;
    // 以下只屬 pump thread(create 建好後移交,quit 時 pump 自己收)
    void* audio_client_{nullptr};    // IAudioClient*
    void* capture_client_{nullptr};  // IAudioCaptureClient*
    void* event_{nullptr};           // HANDLE
    int sample_format_{0};           // 0 = f32、1 = s16
    std::uint32_t channels_{2};
    std::uint32_t channel_mask_{0};  // 0x1 = L、0x2 = R(交錯取第 1/2 聲道)
};

}  // namespace rmx
