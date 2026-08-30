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

namespace rmx {

constexpr std::size_t kParamRingSize = 256;  // 2 的冪
constexpr std::size_t kMaxParamEditsPerBlock = 64;
constexpr std::uint32_t kMaxBlockFrames = 8192;  // RT ping-pong bus 上限(ASIO 遠低於此)

// SPSC:control push(g_engine_mutex 序列化)、RT pop。
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

struct RackSlot {
    std::uint32_t instance_id{};
    std::string name;  // UI 顯示(class name)
    std::string module_path;
    std::string class_id;
    bool bypass{};
    std::shared_ptr<Vst3Plugin> plugin;                    // 鏈 snapshot 間共用
    std::shared_ptr<ParamRing> ring{std::make_shared<ParamRing>()};
    // host 端參數權威值(control 讀寫;VST3 host 設值不反映到 controller)
    std::vector<std::pair<std::uint32_t, double>> param_values;
};

// RT 讀的不可變鏈(slots 為 master 淺拷貝:plugin/ring shared,值欄位快照)
struct RackChain {
    std::vector<RackSlot> slots;
};

}  // namespace rmx
