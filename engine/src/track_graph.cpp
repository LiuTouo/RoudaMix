#include "track_graph.hpp"

#include <algorithm>

namespace rmx {

// ---- TrackSource/TrackOutput ↔ JSON(唯一 codec;契約見 track_graph.hpp)----

nlohmann::json source_to_json(const TrackSource& src) {
    switch (src.type) {
        case TrackSource::kSine:
            return nlohmann::json{{"type", "sine"}, {"freq", src.sine_freq}};
        case TrackSource::kAsioIn:
            return nlohmann::json{{"type", "asioIn"},
                                  {"channel", src.asio_in_ch},
                                  {"mono", src.mono}};
        // app 存名不存 pid(pid 跨載入無意義;load 對不到 = 該軌靜音不 fail)
        case TrackSource::kApp:
            return src.app_name.empty()
                       ? nlohmann::json(nullptr)
                       : nlohmann::json{{"type", "app"}, {"name", src.app_name}};
        case TrackSource::kNone: return nullptr;
    }
    return nullptr;
}

nlohmann::json source_to_status_json(const TrackSource& src) {
    if (src.type == TrackSource::kApp)
        return nlohmann::json{{"type", "app"},
                              {"pid", src.pid},
                              {"name", src.app_name.empty()
                                           ? nlohmann::json(nullptr)
                                           : nlohmann::json(src.app_name)}};
    return source_to_json(src);
}

nlohmann::json output_to_json(const TrackOutput& out) {
    switch (out.type) {
        case TrackOutput::kAsioOut:
            return nlohmann::json{{"type", "asioOut"}, {"channel", out.asio_out_ch}};
        case TrackOutput::kWasapiRender:
            return out.wasapi_id.empty()
                       ? nlohmann::json(nullptr)
                       : nlohmann::json{{"type", "wasapi"}, {"deviceId", out.wasapi_id}};
        case TrackOutput::kNone: return nullptr;
    }
    return nullptr;
}

nlohmann::json output_to_status_json(const TrackOutput& out) {
    if (out.type == TrackOutput::kWasapiRender)  // 狀態形一律帶 deviceId(即使空)
        return nlohmann::json{{"type", "wasapi"}, {"deviceId", out.wasapi_id}};
    return output_to_json(out);
}

TrackSource source_from_json(const nlohmann::json& j) {
    TrackSource src;
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return src;
    const auto t = j["type"].get<std::string>();
    if (t == "sine" && j.contains("freq") && j["freq"].is_number()) {
        src.type = TrackSource::kSine;
        src.sine_freq = j["freq"].get<float>();
    } else if (t == "asioIn" && j.contains("channel") && j["channel"].is_number_unsigned()) {
        src.type = TrackSource::kAsioIn;
        src.asio_in_ch = j["channel"].get<std::uint32_t>();
        if (j.contains("mono") && j["mono"].is_boolean()) src.mono = j["mono"].get<bool>();
    } else if (t == "app") {
        // 指令路徑:pid 必有(contract 已驗 u32);session 檔:只存名(pid = 0 =
        // needsRebind)。兩者擇一成立即採用,皆缺 = 拒絕(kNone)
        const bool has_pid = j.contains("pid") && j["pid"].is_number_unsigned();
        const bool has_name = j.contains("name") && j["name"].is_string();
        if (has_pid || has_name) {
            src.type = TrackSource::kApp;
            if (has_pid) src.pid = j["pid"].get<std::uint32_t>();
            if (has_name) src.app_name = j["name"].get<std::string>();
        }
    }
    return src;
}

TrackOutput output_from_json(const nlohmann::json& j) {
    TrackOutput out;
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) return out;
    const auto t = j["type"].get<std::string>();
    if (t == "asioOut" && j.contains("channel") && j["channel"].is_number_unsigned()) {
        out.type = TrackOutput::kAsioOut;
        out.asio_out_ch = j["channel"].get<std::uint32_t>();
    } else if (t == "wasapi" && j.contains("deviceId") && j["deviceId"].is_string()) {
        out.type = TrackOutput::kWasapiRender;
        out.wasapi_id = j["deviceId"].get<std::string>();
    }
    return out;
}

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

TelemetryStripPlan plan_telemetry_strips(const std::vector<TrackNode>& nodes,
                                         std::size_t budget) {
    TelemetryStripPlan plan;
    plan.tracks.resize(nodes.size());
    // 純函式:graph、snapshot/status 與 SHM publisher 共用同一份 plan/table。
    if (budget == 0) return plan;
    plan.engine_strip = 0;
    plan.table.push_back({0, TelemetryStripKind::kEngineOutput, kNoTelemetryOwner,
                          kNoTelemetryOwner});
    std::size_t next = 1;  // strip 0 = engine 輸出
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (next >= budget) break;
        const auto id = static_cast<std::uint32_t>(next++);
        plan.tracks[i].track_strip = id;
        plan.table.push_back(
            {id, TelemetryStripKind::kTrack, nodes[i].track_id, kNoTelemetryOwner});
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        plan.tracks[i].chain_strips.assign(nodes[i].chain.size(), kNoStrip);
        for (std::size_t si = 0; si < plan.tracks[i].chain_strips.size(); ++si) {
            if (next >= budget) return plan;
            const auto id = static_cast<std::uint32_t>(next++);
            plan.tracks[i].chain_strips[si] = id;
            plan.table.push_back({id, TelemetryStripKind::kPlugin, nodes[i].track_id,
                                  nodes[i].chain[si].instance_id});
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
