// Track graph:多軌 DAG。控制面持有 master(vector<TrackNode>),mutation 後
// build_graph() 深拷貝結構殼(chain 內 plugin/ring shared_ptr 共用、RT buffer
// shared_ptr 共用 → 不拷音訊記憶體)、atomic swap 進 RT;舊 graph 走 500ms grace
// 後回收(同舊 rack 模式,RT 絕不 delete 自己正讀的快照)。
// 契約:control 面保證 dests 無環(track_set_dests 先驗);RT 端不防環。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rack.hpp"

namespace rmx {

enum class TrackKind : std::uint8_t { kAudio, kApp, kFx, kOutput };

inline const char* track_kind_str(TrackKind k) noexcept {
    switch (k) {
        case TrackKind::kAudio: return "audio";
        case TrackKind::kApp: return "app";
        case TrackKind::kFx: return "fx";
        case TrackKind::kOutput: return "output";
    }
    return "audio";
}

// 來源:kNone = FX/output 軌(bus 已含上游 sum);kSine = 測試音;kAsioIn =
// 裝置輸入 pair(ch 與 ch+1);kApp = process loopback(M5b)。
struct TrackSource {
    enum Type : std::uint8_t { kNone, kSine, kAsioIn, kApp } type{kNone};
    float sine_freq{440.0F};
    std::uint32_t asio_in_ch{};  // pair 基底
    std::uint32_t pid{};         // kApp(M5b)
    std::string app_name;        // UI 顯示(M5b;session 存名不存 pid)
    bool operator==(const TrackSource&) const = default;
};

// Sink:kNone = 不落地;kAsioOut = 裝置輸出 pair;kWasapiRender = M5c。
struct TrackOutput {
    enum Type : std::uint8_t { kNone, kAsioOut, kWasapiRender } type{kNone};
    std::uint32_t asio_out_ch{};  // pair 基底
    std::string wasapi_id;        // M5c
    bool operator==(const TrackOutput&) const = default;
};

// 每軌 RT buffer:track 建立時配一次,mutation 淺拷貝只 bump refcount。
// gain_state = RT 端平滑增益現值(block 間線性斜坡,防推桿爆音)、sine_phase =
// 該軌 sine 產生器相位(1<<32 = 2π)—— 兩者跨 snapshot 存續(RT 專寫)。
struct TrackRt {
    float in[2][kMaxBlockFrames]{};
    float alt[2][kMaxBlockFrames]{};
    float gain_state{1.0F};
    std::uint64_t sine_phase{};
};

constexpr std::uint32_t kNoStrip = 0xFFFFFFFFu;  // 超出 telemetry 預算 = 沒有錶

// snapshot 節點(RT 唯讀)。master 同構但 src/out/strip 欄位只在 snapshot 填。
struct TrackNode {
    std::uint32_t track_id{};
    TrackKind kind{TrackKind::kAudio};
    std::string name;
    std::uint32_t color{};  // 0xRRGGBB
    std::vector<RackSlot> chain;  // plugin/ring shared_ptr 與 master 共用
    std::vector<std::uint32_t> dests;  // 下游 trackId(多選 = summing)
    TrackSource source;
    TrackOutput output;
    float gain{1.0F};  // 線性乘數 [0,4];post-fader(chain 後、dest sum 前)
    bool mute{};
    // build_graph 解析(control 面、拿 ASIO scratch map 比對):pair 兩聲道在
    // block.inputs/outputs 的位置;-1 = 該 channel 沒建(靜音/不送)
    std::int32_t src_l{-1}, src_r{-1};
    std::int32_t out_l{-1}, out_r{-1};
    std::uint32_t track_strip{kNoStrip};       // telemetry strip(軌)
    std::vector<std::uint32_t> chain_strips;   // 平行於 chain(plugin 錶)
    std::shared_ptr<TrackRt> buf;
};

struct TrackGraph {
    std::vector<TrackNode> nodes;       // master 順序 = UI 欄內順序
    std::vector<std::uint32_t> order;   // 拓撲序的 node index(RT 照跑);有環時 = master 序 fallback
    std::vector<std::uint32_t> id_index;  // trackId → node index(kNoStrip = 無此 id;RT dest sum 查表)
};

// 拓撲排序(Kahn,對 dests 數邊)。有環時回傳 fallback = master 序(防禦:
// control 面本來就擋環,這裡不讓 RT 掛)。回傳值為 node index 序。
std::vector<std::uint32_t> graph_topo_order(const std::vector<TrackNode>& nodes);

// 環偵測(control 面 track_set_dests 先驗再套;hypothetical 直接改一份驗)
bool graph_has_cycle(const std::vector<TrackNode>& nodes);

}  // namespace rmx
