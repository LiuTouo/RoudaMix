#include "pdc_planner.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace rmx {

PdcPlanResult plan_plugin_delay(const std::vector<PdcNodeSpec>& nodes,
                                const std::vector<PdcOutputSpec>& outputs,
                                const PdcLimits& limits) {
    PdcPlanResult result;
    const std::size_t count = nodes.size();
    std::unordered_map<std::uint32_t, std::size_t> index;
    index.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (!index.emplace(nodes[i].track_id, i).second) {
            result.error = PdcPlanError::kDuplicateTrackId;
            return result;
        }
    }

    std::vector<std::vector<std::size_t>> incoming(count);
    std::vector<std::size_t> indegree(count, 0);
    for (std::size_t from = 0; from < count; ++from) {
        for (const auto dest_id : nodes[from].dests) {
            const auto found = index.find(dest_id);
            if (found == index.end()) {
                result.error = PdcPlanError::kUnknownDestination;
                return result;
            }
            incoming[found->second].push_back(from);
            ++indegree[found->second];
        }
    }
    for (const auto& output : outputs) {
        if (index.find(output.track_id) == index.end()) {
            result.error = PdcPlanError::kUnknownOutput;
            return result;
        }
    }
    std::vector<std::size_t> order;
    order.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        if (indegree[i] == 0) order.push_back(i);
    for (std::size_t cursor = 0; cursor < order.size(); ++cursor) {
        const auto from = order[cursor];
        for (const auto dest_id : nodes[from].dests) {
            const auto found = index.find(dest_id);
            if (found != index.end() && --indegree[found->second] == 0)
                order.push_back(found->second);
        }
    }
    if (order.size() != count) {
        result.error = PdcPlanError::kCycle;
        return result;
    }

    std::vector<bool> reaches_full_pdc(count, false);
    std::vector<std::size_t> pending;
    for (const auto& output : outputs) {
        if (output.policy != OutputLatencyPolicy::kFullPdc) continue;
        const auto found = index.find(output.track_id);
        if (found == index.end() || reaches_full_pdc[found->second]) continue;
        reaches_full_pdc[found->second] = true;
        pending.push_back(found->second);
    }
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        for (const auto from : incoming[pending[cursor]]) {
            if (reaches_full_pdc[from]) continue;
            reaches_full_pdc[from] = true;
            pending.push_back(from);
        }
    }
    std::vector<std::uint64_t> primary_path_latency(count, 0);
    std::vector<std::uint64_t> monitor_path_latency(count, 0);
    std::vector<std::uint64_t> compensation_path(count, 0);
    for (const auto node_index : order) {
        std::uint64_t primary_input = 0;
        std::uint64_t monitor_input = 0;
        for (const auto from : incoming[node_index]) {
            primary_input = (std::max)(primary_input, primary_path_latency[from]);
            monitor_input = (std::max)(monitor_input, monitor_path_latency[from]);
        }
        if (reaches_full_pdc[node_index] && incoming[node_index].size() > 1) {
            for (const auto from : incoming[node_index]) {
                const auto delay = primary_input - primary_path_latency[from];
                const auto max_size = (std::numeric_limits<std::size_t>::max)();
                if (limits.channels != 0 && limits.bytes_per_sample > max_size / limits.channels) {
                    result.error = PdcPlanError::kArithmeticOverflow;
                    return result;
                }
                const std::size_t bytes_per_frame =
                    static_cast<std::size_t>(limits.channels) * limits.bytes_per_sample;
                if (limits.max_path_samples > max_size -
                                                      static_cast<std::size_t>(limits.max_block_frames) -
                                                      1u) {
                    result.error = PdcPlanError::kArithmeticOverflow;
                    return result;
                }
                const std::size_t capacity = static_cast<std::size_t>(limits.max_path_samples) +
                                             limits.max_block_frames + 1u;
                if (bytes_per_frame != 0 && capacity > max_size / bytes_per_frame) {
                    result.error = PdcPlanError::kArithmeticOverflow;
                    return result;
                }
                const std::size_t edge_bytes = capacity * bytes_per_frame;
                if (edge_bytes > max_size - result.buffer_bytes) {
                    result.error = PdcPlanError::kArithmeticOverflow;
                    return result;
                }
                result.edge_delays.push_back(
                    {nodes[from].track_id, nodes[node_index].track_id, delay});
                result.buffer_bytes += edge_bytes;
                if (result.buffer_bytes > limits.max_buffer_bytes) {
                    result.error = PdcPlanError::kMemoryLimitExceeded;
                    return result;
                }
            }
        }
        std::uint64_t max_compensation = 0;
        for (const auto from : incoming[node_index]) {
            const auto local = reaches_full_pdc[node_index]
                                   ? primary_input - primary_path_latency[from]
                                   : 0u;
            if (local > (std::numeric_limits<std::uint64_t>::max)() -
                            compensation_path[from]) {
                result.error = PdcPlanError::kArithmeticOverflow;
                return result;
            }
            max_compensation =
                (std::max)(max_compensation, compensation_path[from] + local);
        }
        compensation_path[node_index] = max_compensation;
        const auto max_samples = (std::numeric_limits<std::uint64_t>::max)();
        if (nodes[node_index].primary_latency_samples > max_samples - primary_input ||
            nodes[node_index].monitor_latency_samples > max_samples - monitor_input) {
            result.error = PdcPlanError::kArithmeticOverflow;
            return result;
        }
        primary_path_latency[node_index] =
            primary_input + nodes[node_index].primary_latency_samples;
        monitor_path_latency[node_index] =
            monitor_input + nodes[node_index].monitor_latency_samples;
    }

    for (const auto& output : outputs) {
        const auto found = index.find(output.track_id);
        if (found == index.end()) continue;
        const bool synchronized = output.policy == OutputLatencyPolicy::kFullPdc;
        const auto total = synchronized ? primary_path_latency[found->second]
                                        : monitor_path_latency[found->second];
        if (total > limits.max_path_samples) {
            result.error = PdcPlanError::kPathLimitExceeded;
            result.edge_delays.clear();
            result.outputs.clear();
            result.buffer_bytes = 0;
            return result;
        }
        result.outputs.push_back(
            {output.track_id, total, compensation_path[found->second], synchronized});
    }
    return result;
}

}  // namespace rmx
