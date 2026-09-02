// Track graph 的拓撲排序與反向可達性分析:pdc_planner 與 route_planner
// 共用的內部 seam(外部介面仍是兩個 planner 的 public 函式)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace rmx::graph {

struct TopoGraph {
    std::unordered_map<std::uint32_t, std::size_t> index;
    std::vector<std::vector<std::size_t>> incoming;
    std::vector<std::size_t> order;  // Kahn 序;短於節點數 = 有環

    [[nodiscard]] bool complete() const noexcept { return order.size() == incoming.size(); }
};

struct TopoBuild {
    TopoGraph graph;
    bool duplicate_id{};   // track_id 重複,已中止建構
    bool unknown_dest{};   // dests 指向未知 track_id,已中止建構
};

// 節點型別只需 track_id 與 dests 兩個成員(PdcNodeSpec / RouteTrackSpec 皆符合)。
template <typename Node>
TopoBuild build_topo(const std::vector<Node>& nodes) {
    TopoBuild build;
    auto& g = build.graph;
    const std::size_t count = nodes.size();
    g.index.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        if (!g.index.emplace(nodes[i].track_id, i).second) {
            build.duplicate_id = true;
            return build;
        }
    g.incoming.resize(count);
    std::vector<std::size_t> indegree(count, 0);
    for (std::size_t from = 0; from < count; ++from)
        for (const auto dest_id : nodes[from].dests) {
            const auto found = g.index.find(dest_id);
            if (found == g.index.end()) {
                build.unknown_dest = true;
                return build;
            }
            g.incoming[found->second].push_back(from);
            ++indegree[found->second];
        }
    g.order.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        if (indegree[i] == 0) g.order.push_back(i);
    for (std::size_t cursor = 0; cursor < g.order.size(); ++cursor)
        for (const auto dest_id : nodes[g.order[cursor]].dests) {
            const auto found = g.index.find(dest_id);
            if (found != g.index.end() && --indegree[found->second] == 0)
                g.order.push_back(found->second);
        }
    return build;
}

// 自 seeds 沿 incoming 邊反向傳播可達性;重複 seed 冪等。
inline std::vector<bool> reverse_reach(const TopoGraph& g,
                                       std::vector<std::size_t> seeds) {
    std::vector<bool> reach(g.incoming.size(), false);
    for (const auto seed : seeds) reach[seed] = true;
    for (std::size_t cursor = 0; cursor < seeds.size(); ++cursor)
        for (const auto from : g.incoming[seeds[cursor]]) {
            if (reach[from]) continue;
            reach[from] = true;
            seeds.push_back(from);
        }
    return reach;
}

}  // namespace rmx::graph
