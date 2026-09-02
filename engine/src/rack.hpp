// Rack:plugin 鏈。控制面持有 master(可變),mutation 後複製成 chain snapshot、
// atomic swap 進 RT;RT 絕不 delete 舊鏈(控制面 grace period 後回收)。
// ponytail: retired chain 只在 mutation/stop 時回收 —— 引擎長跑無 mutation 時
// 舊鏈滯留(共用 plugin shared_ptr,實際多佔的是 vector 殼),要省再掛定時清理。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vst3_host.hpp"
#include "pdc_delay_line.hpp"
#include "route_planner.hpp"

namespace rmx {

constexpr std::size_t kParamRingSize = 256;  // 2 的冪
constexpr std::size_t kMaxParamEditsPerBlock = 64;
constexpr std::uint32_t kMaxBlockFrames = 8192;  // RT ping-pong bus 上限(ASIO 遠低於此)

// SPSC:control push(Router 臨界區序列化)、RT pop。
// 滿 = drop —— 參數權威值在 host 端 map,UI 重送冪等,漏一筆無妨。
class ParamRing final {
public:
    bool push(const Vst3ParamEdit& edit) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t next = (t + 1) % kParamRingSize;
        if (next == h) return false;
        items_[t] = edit;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // RT:一次 drain 本 block 要套用的參數
    std::size_t pop_all(Vst3ParamEdit* out, std::size_t max) noexcept {
        std::size_t n = 0;
        std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        while (h != t && n < max) {
            out[n++] = items_[h];
            h = (h + 1) % kParamRingSize;
        }
        head_.store(h, std::memory_order_release);
        return n;
    }

private:
    Vst3ParamEdit items_[kParamRingSize]{};
    alignas(64) std::atomic<std::size_t> head_{};  // RT 寫
    alignas(64) std::atomic<std::size_t> tail_{};  // control 寫
};

// VST3 可在 RT process() 內同步通知 kLatencyChanged；callback 只能置位，
// 非 RT publish thread 再取走並通知 main thread，避免 RT 直接呼叫 Win32。
struct LatencyChangeMailbox {
    std::atomic<bool> primary{};
    std::atomic<bool> monitor{};
};

struct RackSlot {
    // placeholder 語意:plugin == nullptr 且 availability != kOk。session 載入時
    // module 消失/壞檔/worker 不在 → 保留原位置與 metadata(params/bypass),
    // 不參與 DSP(等同 bypass);可 retry 載回同一 instanceId。
    enum class Availability : std::uint8_t { kOk, kMissing, kLoadFailed };
    using RuntimeState = RouteRuntimeState;
    std::uint32_t instance_id{};
    std::string name;  // UI 顯示(class name;placeholder 時 = session 存的名字)
    std::string module_path;
    std::string class_id;
    bool bypass{};
    bool monitor_bypass{};  // 只略過 Low-Latency Outputs；Session v3 持久化
    std::uint64_t latency_samples{};  // processor runtime 宣告；samples 為權威
    bool latency_known{};             // placeholder/未載入 = false
    std::uint64_t monitor_latency_samples{};
    bool monitor_latency_known{};
    RuntimeState primary_state{RuntimeState::kActive};
    RuntimeState monitor_state{RuntimeState::kActive};
    std::uint32_t primary_cpu_index{0xFFFFFFFFu};
    std::uint32_t shadow_cpu_index{0xFFFFFFFFu};
    std::shared_ptr<PdcDelayLine> primary_dry_delay;
    std::shared_ptr<PdcDelayLine> shadow_dry_delay;
    std::shared_ptr<Vst3Plugin> plugin;                    // 鏈 snapshot 間共用;placeholder = null
    std::shared_ptr<ParamRing> ring{std::make_shared<ParamRing>()};
    std::shared_ptr<Vst3Plugin> monitor_shadow;            // low-latency 分岔後的獨立 processor
    std::shared_ptr<ParamRing> monitor_ring{std::make_shared<ParamRing>()};
    std::shared_ptr<LatencyChangeMailbox> latency_change_mailbox{
        std::make_shared<LatencyChangeMailbox>()};
    // host 端參數權威值(control 讀寫;VST3 host 設值不反映到 controller)
    std::vector<std::pair<std::uint32_t, double>> param_values;
    Availability availability{Availability::kOk};
    std::string load_error;  // placeholder 原因(UI 顯示;ok 時空)

    [[nodiscard]] bool is_placeholder() const noexcept {
        return plugin == nullptr && availability != Availability::kOk;
    }
};

inline const char* runtime_state_str(RackSlot::RuntimeState state) noexcept {
    switch (state) {
        case RackSlot::RuntimeState::kPreparing: return "preparing";
        case RackSlot::RuntimeState::kDegraded: return "degraded";
        case RackSlot::RuntimeState::kSuspended: return "suspended";
        case RackSlot::RuntimeState::kActive: return "active";
    }
    return "active";
}

// JSON 字串(protocol/session 共用;"ok"|"missing"|"loadFailed")
inline const char* availability_str(RackSlot::Availability a) noexcept {
    switch (a) {
        case RackSlot::Availability::kMissing: return "missing";
        case RackSlot::Availability::kLoadFailed: return "loadFailed";
        case RackSlot::Availability::kOk: break;
    }
    return "ok";
}

// RT 讀的不可變鏈(slots 為 master 淺拷貝:plugin/ring shared,值欄位快照)
struct RackChain {
    std::vector<RackSlot> slots;
};

}  // namespace rmx
