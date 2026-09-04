// 退休圖回收佇列 + reader 門閂(#15,CWE-416)。原本固定 500ms 牆鐘一到就
// delete:RT callback / SHM publisher atomic-load 裸指標後長距離遍歷,高
// fan-out session 或系統排程可能讓 reader 停留超過 grace,之後的 swap 就會
// 釋放使用中的圖。規則:reader 取圖前先登記(計數本體永不釋放),reap 只在
// 「逾時(或 force)且 reader 歸零」時 delete;reader 未退 = 留在佇列下輪
// 再試。寧可晚刪,不可在使用中釋放。
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "track_graph.hpp"

namespace rmx {

class GraphRetireQueue {
public:
    static constexpr std::uint64_t kGraceMs{500};  // 同舊 rack 模式的牆鐘下限

    void retire(TrackGraph* g, std::uint64_t now) {
        if (g != nullptr) entries_.push_back({g, now});
    }

    // 回傳本次釋放的圖數。force = 忽略 grace,但仍受 reader 門閂:stop 路徑
    // publisher thread 還活著,可能恰在遍歷;引擎解構時 thread 已收,才真的全清。
    std::size_t reap(std::uint64_t now, bool force) {
        const bool readers = readers_.load(std::memory_order_acquire) != 0;
        std::size_t freed = 0;
        std::vector<Entry> still;
        still.reserve(entries_.size());
        for (auto& e : entries_) {
            if (!readers && (force || (now - e.tick) > kGraceMs)) {
                delete e.graph;
                ++freed;
            } else {
                still.push_back(std::move(e));
            }
        }
        entries_ = std::move(still);
        return freed;
    }

    void reader_enter() noexcept { readers_.fetch_add(1, std::memory_order_acquire); }
    void reader_exit() noexcept { readers_.fetch_sub(1, std::memory_order_release); }
    bool readers_active() const noexcept {
        return readers_.load(std::memory_order_acquire) != 0;
    }

private:
    struct Entry {
        TrackGraph* graph;
        std::uint64_t tick;  // GetTickCount64;reader 未退則留佇列(原 tick)下輪再試
    };
    std::vector<Entry> entries_;
    std::atomic<std::uint32_t> readers_{0};
};

}  // namespace rmx
