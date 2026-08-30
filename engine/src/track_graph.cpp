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

}  // namespace rmx
