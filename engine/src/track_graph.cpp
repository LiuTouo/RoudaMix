#include "track_graph.hpp"

#include <algorithm>

namespace rmx {

namespace {
// id → node index(每次呼叫重建:軌數小,mutation 頻率低)
std::vector<std::uint32_t> id_to_index(const std::vector<TrackNode>& nodes) {
    std::vector<std::uint32_t> m;
    m.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        // master 內 id 保證唯一(track_add 用 next_track_id_);防禦性 clamp
        const auto id = nodes[i].track_id;
        if (id >= m.size()) m.resize(static_cast<std::size_t>(id) + 1, kNoStrip);
        m[id] = static_cast<std::uint32_t>(i);
    }
    return m;
}
}  // namespace

std::vector<std::uint32_t> graph_topo_order(const std::vector<TrackNode>& nodes) {
    const auto index = id_to_index(nodes);
    const std::size_t n = nodes.size();
    std::vector<std::uint32_t> indegree(n, 0);
    for (const auto& nd : nodes) {
        for (const auto d : nd.dests) {
            if (d < index.size() && index[d] != kNoStrip) ++indegree[index[d]];
        }
    }
    std::vector<std::uint32_t> queue;  // Kahn(indegree 0 先進)
    for (std::size_t i = 0; i < n; ++i)
        if (indegree[i] == 0) queue.push_back(static_cast<std::uint32_t>(i));
    std::vector<std::uint32_t> order;
    order.reserve(n);
    for (std::size_t qi = 0; qi < queue.size(); ++qi) {
        const auto i = queue[qi];
        order.push_back(i);
        for (const auto d : nodes[i].dests) {
            if (d < index.size() && index[d] != kNoStrip) {
                if (--indegree[index[d]] == 0) queue.push_back(index[d]);
            }
        }
    }
    if (order.size() != n) return {};  // 環:呼叫端 fallback master 序
    return order;
}

bool graph_has_cycle(const std::vector<TrackNode>& nodes) {
    return nodes.empty() ? false : graph_topo_order(nodes).empty();
}

void asio_channel_union(const std::vector<TrackNode>& tracks,
                        std::vector<std::uint32_t>& in_chans,
                        std::vector<std::uint32_t>& out_chans) {
    in_chans.clear();
    out_chans.clear();
    for (const auto& t : tracks) {
        if (t.source.type == TrackSource::kAsioIn) {
            in_chans.push_back(t.source.asio_in_ch);
            in_chans.push_back(t.source.asio_in_ch + 1);
        }
        if (t.output.type == TrackOutput::kAsioOut) {
            out_chans.push_back(t.output.asio_out_ch);
            out_chans.push_back(t.output.asio_out_ch + 1);
        }
    }
    std::sort(in_chans.begin(), in_chans.end());
    in_chans.erase(std::unique(in_chans.begin(), in_chans.end()), in_chans.end());
    std::sort(out_chans.begin(), out_chans.end());
    out_chans.erase(std::unique(out_chans.begin(), out_chans.end()), out_chans.end());
    if (out_chans.empty()) out_chans = {0, 1};
}

std::vector<TrackStrips> plan_telemetry_strips(const std::vector<TrackNode>& nodes,
                                               std::size_t budget) {
    std::vector<TrackStrips> plan(nodes.size());
    // 純函式:swap_graph(填 snapshot)與 status_json(填 metered)共用同一分配,
    // 兩端看到的是同一份真相。第一輪 track 先領,第二輪剩餘給 plugin。
    std::size_t next = 1;  // strip 0 = engine 輸出
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (next < budget) plan[i].track_strip = static_cast<std::uint32_t>(next++);
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        plan[i].chain_strips.assign(nodes[i].chain.size(), kNoStrip);
        for (auto& s : plan[i].chain_strips) {
            if (next >= budget) return plan;
            s = static_cast<std::uint32_t>(next++);
        }
    }
    return plan;
}

bool ensure_system_outputs(std::vector<TrackNode>& tracks, std::uint32_t& next_track_id) {
    bool changed = false;
    // 每 role:持有者 >1 = 第一個以外降級;0 = 指派無 role 的 output 軌,再不行新建
    for (const auto role : {SystemRole::kMonitor, SystemRole::kStream}) {
        std::size_t holder = tracks.size();
        std::size_t seen = 0;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].system_role != role) continue;
            if (seen == 0) {
                holder = i;
            } else {
                tracks[i].system_role = SystemRole::kNone;  // 重複 role:留第一個
                tracks[i].latency_policy = OutputLatencyPolicy::kFullPdc;
                changed = true;
            }
            ++seen;
        }
        if (seen > 0) continue;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].kind != TrackKind::kOutput || tracks[i].system_role != SystemRole::kNone)
                continue;
            tracks[i].system_role = role;
            tracks[i].latency_policy =
                role == SystemRole::kMonitor ? OutputLatencyPolicy::kLowLatency
                                              : OutputLatencyPolicy::kFullPdc;
            holder = i;
            changed = true;
            break;
        }
        if (holder < tracks.size()) continue;
        // 沒得指派:新建(monitor 帶預設 ASIO 主輸出 pair 0;stream 無 sink)
        TrackNode t;
        t.kind = TrackKind::kOutput;
        t.system_role = role;
        t.latency_policy = role == SystemRole::kMonitor ? OutputLatencyPolicy::kLowLatency
                                                        : OutputLatencyPolicy::kFullPdc;
        t.track_id = next_track_id++;
        t.name = role == SystemRole::kMonitor ? "監聽" : "串流";
        static constexpr std::uint32_t kPalette[] = {0x4da3ff, 0x3ddc84, 0xffb454, 0xff5c5c,
                                                     0xb48cff, 0x4dd0e1, 0xf06292, 0xaed581};
        t.color = kPalette[(t.track_id - 1) % 8];
        t.buf = std::make_shared<TrackRt>();
        if (role == SystemRole::kMonitor) {
            t.output.type = TrackOutput::kAsioOut;
            t.output.asio_out_ch = 0;
        }
        tracks.push_back(std::move(t));
        changed = true;
    }
    return changed;
}

}  // namespace rmx
