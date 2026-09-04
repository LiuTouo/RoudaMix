// Track graph:多軌 DAG。控制面持有 master(vector<TrackNode>),mutation 後
// build_graph() 深拷貝結構殼(chain 內 plugin/ring shared_ptr 共用、RT buffer
// shared_ptr 共用 → 不拷音訊記憶體)、atomic swap 進 RT;舊 graph 走 500ms grace
// 且 reader 歸零後才回收(GraphRetireQueue,#15 — reader 卡超過 grace 也只是
// 晚刪,不會被使用中釋放)。
// 契約:control 面保證 dests 無環(track_set_dests 先驗);RT 端不防環。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "rack.hpp"
#include "pdc_delay_line.hpp"
#include "route_planner.hpp"
#include "rt_crossfade.hpp"
#include "telemetry.hpp"

namespace rmx {

class AppCapture;  // M5b:process loopback capture(app 軌;shared_ptr 跨 snapshot)
class RenderSink;  // M5c:wasapi render sink(串流軌;shared_ptr 跨 snapshot)

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

// 來源軌:每個處理週期用自己的來源覆寫輸入匯流,路由進去的訊號會被丟棄,
// 不得為路由目的地(#11)。未來新增軌種時在此同步決定可否作為目的地。
inline bool source_kind(TrackKind k) noexcept {
    return k == TrackKind::kAudio || k == TrackKind::kApp;
}

// 系統輸出角色:每個 session 恰好一條 monitor(監聽)+ 一條 stream(串流)。
// 穩定 ID,不靠名稱;使用者可改名/改 sink/routing,但不可刪除(engine track_remove 擋)。
enum class SystemRole : std::uint8_t { kNone, kMonitor, kStream };

inline const char* system_role_str(SystemRole r) noexcept {
    switch (r) {
        case SystemRole::kMonitor: return "monitor";
        case SystemRole::kStream: return "stream";
        case SystemRole::kNone: break;
    }
    return nullptr;
}

// 來源:kNone = FX/output 軌(bus 已含上游 sum);kSine = 測試音;kAsioIn =
// 裝置輸入 pair(ch 與 ch+1);kApp = process loopback(M5b)。
struct TrackSource {
    enum Type : std::uint8_t { kNone, kSine, kAsioIn, kApp } type{kNone};
    float sine_freq{440.0F};
    std::uint32_t asio_in_ch{};  // pair 基底
    std::uint32_t pid{};         // kApp(M5b)
    std::string app_name;        // UI 顯示(M5b;session 存名不存 pid)
    bool mono{};                 // kAsioIn:單聲道來源(asio_in_ch 複製到 L/R;Studio One 式輸入格式)
    bool operator==(const TrackSource&) const = default;
};

// Sink:kNone = 不落地;kAsioOut = 裝置輸出 pair;kWasapiRender = M5c。
struct TrackOutput {
    enum Type : std::uint8_t { kNone, kAsioOut, kWasapiRender } type{kNone};
    std::uint32_t asio_out_ch{};  // pair 基底
    std::string wasapi_id;        // M5c
    bool operator==(const TrackOutput&) const = default;
};

// ---- TrackSource/TrackOutput ↔ JSON 的唯一 codec(格式 = contracts/protocol.md §8)----
// 新增來源/輸出類型只需在此補 case + contract 一列;session 存取、status 投影、
// router 請求解碼共用,不再各持一份欄位搬運。
// 持久形(session 檔 §8 SessionTrack):app 存名不存 pid(pid 跨載入無意義);
//   空名/空 deviceId 編碼為 null(載入即 kNone,該軌靜音不 fail)。
nlohmann::json source_to_json(const TrackSource& src);
nlohmann::json output_to_json(const TrackOutput& out);
// 狀態形(status.tracks §8 Track):app 帶 pid(pid 0 = needsRebind,UI 走程序
// 選擇器)、wasapi 一律帶 deviceId;其餘類型與持久形相同。
nlohmann::json source_to_status_json(const TrackSource& src);
nlohmann::json output_to_status_json(const TrackOutput& out);
// 解碼(session 載入與 router 請求共用,失敗語義 = session 載入:缺欄/型別不符/
// 未知 type 回 kNone 預設值,不丟例外;未知類型維持拒絕不採用)。app 帶 pid 時
// 採用(指令路徑 contract 已驗 u32),session 檔無 pid = 0(needsRebind)。
TrackSource source_from_json(const nlohmann::json& j);
TrackOutput output_from_json(const nlohmann::json& j);

// 每軌 RT buffer:track 建立時配一次,mutation 淺拷貝只 bump refcount。
// gain_state = RT 端平滑增益現值(block 間線性斜坡,防推桿爆音)、sine_phase =
// 該軌 sine 產生器相位(1<<32 = 2π)—— 兩者跨 snapshot 存續(RT 專寫)。
struct TrackRt {
    float in[2][kMaxBlockFrames]{};
    float alt[2][kMaxBlockFrames]{};
    float monitor_in[2][kMaxBlockFrames]{};
    float monitor_alt[2][kMaxBlockFrames]{};
    float dry[2][kMaxBlockFrames]{};
    float monitor_dry[2][kMaxBlockFrames]{};
    float gain_state{1.0F};
    float monitor_gain_state{1.0F};
    std::uint64_t sine_phase{};
    RtCrossfade monitor_crossfade;
};

constexpr std::uint32_t kNoStrip = 0xFFFFFFFFu;  // 超出 telemetry 預算 = 沒有錶

// snapshot 節點(RT 唯讀)。master 同構但 src/out/strip 欄位只在 snapshot 填。
struct TrackNode {
    std::uint32_t track_id{};
    TrackKind kind{TrackKind::kAudio};
    SystemRole system_role{SystemRole::kNone};  // 系統輸出角色(monitor/stream 軌不可刪)
    OutputLatencyPolicy latency_policy{OutputLatencyPolicy::kFullPdc};
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
    std::vector<std::shared_ptr<PdcDelayLine>> dest_pdc;  // 平行於 dests；primary only
    std::shared_ptr<TrackRt> buf;
    std::shared_ptr<AppCapture> capture;  // M5b:app 軌的 loopback pump(master 與 snapshot 共用)
    std::shared_ptr<RenderSink> render;   // M5c:串流軌的 wasapi render pump
    std::string track_error;              // control 面:capture/render 失敗等原因(UI 顯示;空 = 無)
};

struct TrackStrips {
    std::uint32_t track_strip{kNoStrip};
    std::vector<std::uint32_t> chain_strips;  // 平行於 chain
};
struct TelemetryStripPlan {
    std::uint32_t engine_strip{kNoStrip};
    std::vector<TrackStrips> tracks;
    std::vector<TelemetryStripIdentity> table;
};

struct TrackGraph {
    std::vector<TrackNode> nodes;       // master 順序 = UI 欄內順序
    std::vector<std::uint32_t> order;   // 拓撲序的 node index(RT 照跑);有環時 = master 序 fallback
    std::vector<std::uint32_t> id_index;  // trackId → node index(kNoStrip = 無此 id;RT dest sum 查表)
    TelemetryStripPlan strip_plan;
    RoutePlan route_plan;                 // control thread 規劃；RT 只讀
};

// 拓撲排序(Kahn,對 dests 數邊)。有環時回傳 fallback = master 序(防禦:
// control 面本來就擋環,這裡不讓 RT 掛)。回傳值為 node index 序。
std::vector<std::uint32_t> graph_topo_order(const std::vector<TrackNode>& nodes);

// 環偵測(control 面 track_set_dests 先驗再套;hypothetical 直接改一份驗)
bool graph_has_cycle(const std::vector<TrackNode>& nodes);

// 從所有軌的 source/output 收集 ASIO channel 聯集(start 與
// rebuild_asio_channels 共用;pair 基底展開 ch/ch+1,排序去重;
// out 聯集空 = fallback {0,1},createBuffers 至少要一組 out)。
void asio_channel_union(const std::vector<TrackNode>& tracks,
                        std::vector<std::uint32_t>& in_chans,
                        std::vector<std::uint32_t>& out_chans);

// telemetry strip 預算規劃(P1-H,kTelemetryStrips=64,strip 0 = engine 輸出)。
// 兩輪、可預測:第一輪所有軌先各拿一個 track strip(master 序,最多 63 條軌有錶);
// 第二輪剩餘預算依 master 序、鏈序配給 plugin。超出預算 = kNoStrip(該節點沒錶,
// 不影響音訊)。UI 以 status.tracks[].metered 辨認「無錶」而非當成靜音。
TelemetryStripPlan plan_telemetry_strips(const std::vector<TrackNode>& nodes,
                                         std::size_t budget);

// 系統輸出補齊/去重(確定性;migration + 新 session 共用):
// 每個 role 保留第一個持有者(其餘降級 kNone),沒有持有者時優先指派給
// 「尚無 role 的 output 軌」(monitor 取第一條、stream 取下一條),不夠才新建
// (monitor = asioOut ch0、stream = 無 sink)。回傳是否有變動。
bool ensure_system_outputs(std::vector<TrackNode>& tracks, std::uint32_t& next_track_id);

}  // namespace rmx
