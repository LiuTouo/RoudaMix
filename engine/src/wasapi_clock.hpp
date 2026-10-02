// WASAPI master clock(M6 step 2):無 ASIO 裝置時以系統預設 render endpoint
// (eRender/eMultimedia)當 RT 時脈。角色 = AsioDevice 的 WASAPI 替身:
// create() 在控制面(main thread)做 activation/format 協商(不 Start —
// start() 要先拿 block_size 初始化 plugin);pump thread 事件驅動,padding
// 補幀、chunk ≤ kMaxBlockFrames 驅動 engine callback、交錯寫入裝置。
// 監聽輸出 = 全部 kAsioOut 軌 bus_add 進本 clock 的 scratch(engine 側把
// kAsioOut 解析到 {0,1},見 audio_engine.cpp commit_graph_candidate)。
// ponytail: 不掛 IMMNotificationClient — 預設裝置被拔 = GetCurrentPadding/
// GetBuffer 報錯，事件停止時也會定期檢查 padding；失效通知控制面停止。
// xruns 只計 pump error，OS mixer 的一般 glitch 不在此統計。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include "asio_device.hpp"  // IAudioCallback
#include "failure.hpp"
#include "rack.hpp"  // kMaxBlockFrames

namespace rmx {

class WasapiClock final {
public:
    // 啟動準備(阻塞;main thread STA COM,不 Start)。rate_hint 有值且 ≠
    // 裝置 mix rate = 拒絕(shared mode 鎖 mix rate;UI 恆送 null)。
    // 失敗回 nullptr,failure = kDeviceOpenFailed + 原因
    static std::unique_ptr<WasapiClock> create(IAudioCallback* cb,
                                               std::optional<std::uint32_t> rate_hint,
                                               Failure& failure,
                                               std::function<void()> on_fail = {});
    ~WasapiClock();
    // Start + 起 pump thread;回 false = Start 失敗(err 帶原因)
    bool start(std::string& err);
    void stop() noexcept;  // always join, then release even if preparation/start failed
    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

    [[nodiscard]] std::uint32_t block_size() const noexcept { return buffer_frames_; }
    [[nodiscard]] std::uint32_t rate() const noexcept { return mix_rate_; }
    [[nodiscard]] std::uint32_t output_latency() const noexcept { return output_latency_; }
    [[nodiscard]] std::uint64_t callbacks() const noexcept {
        return callbacks_.load(std::memory_order_relaxed);
    }
    // shared mode 下 OS mixer 擁有 glitch，此數只計 pump 錯誤。
    [[nodiscard]] std::uint64_t xruns() const noexcept {
        return xruns_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    WasapiClock() = default;
    void pump();

    IAudioCallback* callback_{nullptr};
    float scratch_[2][kMaxBlockFrames]{};  // engine bus_add 目標
    std::uint32_t mix_rate_{48000};
    std::uint32_t buffer_frames_{0};
    std::uint32_t channels_{2};
    std::uint32_t output_latency_{0};  // frames(GetStreamLatency 換算)
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<std::uint64_t> xruns_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::function<void()> on_fail_;  // posts to control thread; must not destroy this clock inline
    std::thread pump_thread_;
    // Prepared on control thread, used by pump, released by stop() after join.
    void* audio_client_{nullptr};   // IAudioClient*
    void* render_client_{nullptr};  // IAudioRenderClient*
    void* event_{nullptr};          // HANDLE
};

}  // namespace rmx
