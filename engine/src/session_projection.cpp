#include "session_projection.hpp"

#include <algorithm>

#include "protocol.hpp"

namespace rmx::session {

namespace {

nlohmann::json source_json(const TrackSource& source) {
    switch (source.type) {
        case TrackSource::kSine:
            return nlohmann::json{{"type", "sine"}, {"freq", source.sine_freq}};
        case TrackSource::kAsioIn:
            return nlohmann::json{{"type", "asioIn"},
                                  {"channel", source.asio_in_ch},
                                  {"mono", source.mono}};
        case TrackSource::kApp:
            return nlohmann::json{{"type", "app"},
                                  {"pid", source.pid},
                                  {"name", source.app_name.empty()
                                               ? nlohmann::json(nullptr)
                                               : nlohmann::json(source.app_name)}};
        case TrackSource::kNone: return nullptr;
    }
    return nullptr;
}

nlohmann::json output_json(const TrackOutput& output) {
    switch (output.type) {
        case TrackOutput::kAsioOut:
            return nlohmann::json{{"type", "asioOut"}, {"channel", output.asio_out_ch}};
        case TrackOutput::kWasapiRender:
            return nlohmann::json{{"type", "wasapi"}, {"deviceId", output.wasapi_id}};
        case TrackOutput::kNone: return nullptr;
    }
    return nullptr;
}

const char* pdc_error_string(PdcPlanError error) {
    switch (error) {
        case PdcPlanError::kNone: return "none";
        case PdcPlanError::kDuplicateTrackId: return "duplicateTrackId";
        case PdcPlanError::kUnknownDestination: return "unknownDestination";
        case PdcPlanError::kUnknownOutput: return "unknownOutput";
        case PdcPlanError::kCycle: return "cycle";
        case PdcPlanError::kPathLimitExceeded: return "pathLimitExceeded";
        case PdcPlanError::kMemoryLimitExceeded: return "memoryLimitExceeded";
        case PdcPlanError::kArithmeticOverflow: return "arithmeticOverflow";
    }
    return "unknown";
}

const char* telemetry_strip_kind_string(TelemetryStripKind kind) {
    switch (kind) {
        case TelemetryStripKind::kPlugin: return "plugin";
        case TelemetryStripKind::kTrack: return "track";
        case TelemetryStripKind::kEngineOutput: return "engineOutput";
    }
    return "plugin";
}

nlohmann::json telemetry_strips_json(const TelemetryStripPlan& plan) {
    auto table = nlohmann::json::array();
    for (const auto& strip : plan.table) {
        table.push_back({
            {"id", strip.id},
            {"kind", telemetry_strip_kind_string(strip.kind)},
            {"trackId", strip.track_id == kNoTelemetryOwner
                            ? nlohmann::json(nullptr)
                            : nlohmann::json(strip.track_id)},
            {"instanceId", strip.instance_id == kNoTelemetryOwner
                               ? nlohmann::json(nullptr)
                               : nlohmann::json(strip.instance_id)},
        });
    }
    return table;
}

}  // namespace

nlohmann::json tracks_json(const AudioEngine& engine) {
    auto tracks = nlohmann::json::array();
    const auto strips = plan_telemetry_strips(engine.tracks(), kTelemetryStrips);
    std::size_t track_index = 0;
    for (const auto& track : engine.tracks()) {
        auto plugins = nlohmann::json::array();
        for (const auto& slot : track.chain) {
            auto params = nlohmann::json::array();
            for (const auto& [id, value] : slot.param_values)
                params.push_back({{"paramId", id}, {"normalized", value}});
            plugins.push_back({
                {"instanceId", slot.instance_id},
                {"name", slot.name},
                {"pluginPath", slot.module_path},
                {"classId", slot.class_id},
                {"bypassed", slot.bypass},
                {"monitorBypassed", slot.monitor_bypass},
                {"latencySamples", slot.latency_known
                                       ? nlohmann::json(slot.latency_samples)
                                       : nlohmann::json(nullptr)},
                {"effectiveLatencySamples",
                 slot.latency_known
                     ? nlohmann::json(engine.primary_route_processes(slot.instance_id)
                                          ? slot.latency_samples
                                          : 0u)
                     : nlohmann::json(nullptr)},
                {"monitorLatencySamples", slot.monitor_latency_known
                                              ? nlohmann::json(slot.monitor_latency_samples)
                                              : nlohmann::json(nullptr)},
                {"runtimeState", runtime_state_str(slot.primary_state)},
                {"monitorState", runtime_state_str(slot.monitor_state)},
                {"params", std::move(params)},
                {"availability", availability_str(slot.availability)},
                {"loadError", slot.load_error.empty() ? nlohmann::json(nullptr)
                                                        : nlohmann::json(slot.load_error)},
            });
        }
        const char* role = system_role_str(track.system_role);
        const bool metered = track_index < strips.tracks.size() &&
                             strips.tracks[track_index].track_strip != kNoStrip;
        tracks.push_back({
            {"trackId", track.track_id},
            {"kind", track_kind_str(track.kind)},
            {"systemRole", role != nullptr ? nlohmann::json(role) : nlohmann::json(nullptr)},
            {"latencyPolicy", output_latency_policy_str(track.latency_policy)},
            {"name", track.name},
            {"color", track.color},
            {"source", source_json(track.source)},
            {"dests", track.dests},
            {"output", output_json(track.output)},
            {"gain", track.gain},
            {"mute", track.mute},
            {"plugins", std::move(plugins)},
            {"metered", metered},
            {"error", track.track_error.empty() ? nlohmann::json(nullptr)
                                                  : nlohmann::json(track.track_error)},
        });
        ++track_index;
    }
    return tracks;
}

nlohmann::json status_json(const AudioEngine& engine, std::uint64_t revision) {
    const auto status = engine.status();
    const auto strips = plan_telemetry_strips(engine.tracks(), kTelemetryStrips);
    nlohmann::json monitor_delay = nullptr;
    nlohmann::json stream_delay = nullptr;
    if (status.running) {
        const auto plan = engine.latency_plan();
        for (const auto& output : plan.outputs) {
            const auto found = std::find_if(
                engine.tracks().begin(), engine.tracks().end(),
                [&](const TrackNode& track) { return track.track_id == output.track_id; });
            if (found == engine.tracks().end()) continue;
            if (found->system_role == SystemRole::kMonitor)
                monitor_delay = output.total_plugin_delay_samples;
            else if (found->system_role == SystemRole::kStream)
                stream_delay = output.total_plugin_delay_samples;
        }
    }
    return {
        {"running", status.running},
        {"deviceKey", status.running ? nlohmann::json(status.device_key)
                                      : nlohmann::json(nullptr)},
        {"sampleRate", status.sample_rate},
        {"bufferSize", status.running && status.buffer_size
                           ? nlohmann::json(status.buffer_size)
                           : nlohmann::json(nullptr)},
        {"inputLatency", status.input_latency ? nlohmann::json(status.input_latency)
                                               : nlohmann::json(nullptr)},
        {"outputLatency", status.output_latency ? nlohmann::json(status.output_latency)
                                                 : nlohmann::json(nullptr)},
        {"xruns", status.xruns},
        {"trackCount", status.track_count},
        {"pluginFails", status.plugin_fails},
        {"revision", revision},
        {"latencyGeneration", engine.latency_generation()},
        {"pluginDelay", {{"monitorSamples", monitor_delay}, {"streamSamples", stream_delay}}},
        {"telemetryStrips", telemetry_strips_json(strips)},
        {"tracks", tracks_json(engine)},
        {"error", status.error.empty() ? nlohmann::json(nullptr)
                                         : nlohmann::json(status.error)},
    };
}

nlohmann::json snapshot_json(const AudioEngine& engine, std::uint64_t epoch,
                             std::uint64_t revision, const nlohmann::json& last_scan) {
    const auto status = status_json(engine, revision);
    auto snapshot = make_snapshot_json(epoch, status, status.at("tracks"));
    snapshot["capabilities"] = nlohmann::json::array({"pluginLatencyPdcV1"});
    snapshot["telemetryStrips"] = status.at("telemetryStrips");
    snapshot["lastScan"] = last_scan;
    return snapshot;
}

nlohmann::json latency_report_json(const AudioEngine& engine) {
    const auto plan = engine.latency_plan();
    auto outputs = nlohmann::json::array();
    for (const auto& output : plan.outputs)
        outputs.push_back({{"trackId", output.track_id},
                           {"totalPluginDelaySamples", output.total_plugin_delay_samples},
                           {"compensationDelaySamples", output.compensation_delay_samples},
                           {"synchronized", output.synchronized}});
    auto edges = nlohmann::json::array();
    for (const auto& edge : plan.edge_delays)
        edges.push_back({{"fromTrackId", edge.from_track_id},
                         {"toTrackId", edge.to_track_id},
                         {"compensationDelaySamples", edge.delay_samples}});
    return {{"generation", engine.latency_generation()},
            {"ok", plan.ok()},
            {"error", pdc_error_string(plan.error)},
            {"bufferBytes", plan.buffer_bytes},
            {"outputs", std::move(outputs)},
            {"edges", std::move(edges)},
            {"tracks", tracks_json(engine)}};
}

}  // namespace rmx::session
