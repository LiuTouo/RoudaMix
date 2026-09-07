#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "track_graph.hpp"

namespace rmx::router_request {

struct Empty {
    explicit Empty(const nlohmann::json&) {}
};

struct Start {
    std::string device_key;
    std::optional<std::uint32_t> sample_rate;
    std::optional<std::uint32_t> buffer_size;

    explicit Start(const nlohmann::json& payload)
        : device_key(payload.at("deviceKey").get<std::string>()),
          sample_rate(payload.at("sampleRate").is_null()
                          ? std::optional<std::uint32_t>{}
                          : std::optional<std::uint32_t>{
                                payload.at("sampleRate").get<std::uint32_t>()}),
          buffer_size(payload.contains("bufferSize") &&
                              payload.at("bufferSize").is_number_unsigned()
                          ? std::optional<std::uint32_t>{
                                payload.at("bufferSize").get<std::uint32_t>()}
                          : std::optional<std::uint32_t>{}) {}
};

struct TrackAdd {
    TrackKind kind{TrackKind::kAudio};
    std::string name;
    std::uint32_t color{};

    explicit TrackAdd(const nlohmann::json& payload) {
        const auto value = payload.at("kind").get<std::string>();
        if (value == "app")
            kind = TrackKind::kApp;
        else if (value == "fx")
            kind = TrackKind::kFx;
        else if (value == "output")
            kind = TrackKind::kOutput;
        if (payload.contains("name")) name = payload.at("name").get<std::string>();
        if (payload.contains("color")) color = payload.at("color").get<std::uint32_t>();
    }
};

struct TrackId {
    std::uint32_t track_id;
    explicit TrackId(const nlohmann::json& payload)
        : track_id(payload.at("trackId").get<std::uint32_t>()) {}
};

struct TrackSet {
    std::uint32_t track_id;
    std::optional<std::string> name;
    std::optional<std::uint32_t> color;
    std::optional<float> gain;
    std::optional<bool> mute;

    explicit TrackSet(const nlohmann::json& payload)
        : track_id(payload.at("trackId").get<std::uint32_t>()) {
        if (payload.contains("name")) name = payload.at("name").get<std::string>();
        if (payload.contains("color")) color = payload.at("color").get<std::uint32_t>();
        if (payload.contains("gain")) gain = payload.at("gain").get<float>();
        if (payload.contains("mute")) mute = payload.at("mute").get<bool>();
    }
};

struct TrackSourceSet {
    std::uint32_t track_id;
    TrackSource source;

    explicit TrackSourceSet(const nlohmann::json& payload)
        : track_id(payload.at("trackId").get<std::uint32_t>()),
          source(source_from_json(payload.at("source"))) {}
};

struct TrackDestsSet {
    std::uint32_t track_id;
    std::vector<std::uint32_t> dests;

    explicit TrackDestsSet(const nlohmann::json& payload)
        : track_id(payload.at("trackId").get<std::uint32_t>()) {
        for (const auto& destination : payload.at("dests"))
            dests.push_back(destination.get<std::uint32_t>());
    }
};

struct TrackOutputSet {
    std::uint32_t track_id;
    TrackOutput output;

    explicit TrackOutputSet(const nlohmann::json& payload)
        : track_id(payload.at("trackId").get<std::uint32_t>()),
          output(output_from_json(payload.at("output"))) {}
};

struct TrackOutputLatencyPolicySet {
    std::uint32_t track_id;
    OutputLatencyPolicy policy;

    explicit TrackOutputLatencyPolicySet(const nlohmann::json& payload)
        : track_id(payload.at("trackId").get<std::uint32_t>()),
          policy(payload.at("policy").get<std::string>() == "lowLatency"
                     ? OutputLatencyPolicy::kLowLatency
                     : OutputLatencyPolicy::kFullPdc) {}
};

struct TrackMove {
    std::uint32_t track_id;
    std::size_t new_index;
    explicit TrackMove(const nlohmann::json& payload)
        : track_id(payload.at("trackId").get<std::uint32_t>()),
          new_index(payload.at("newIndex").get<std::size_t>()) {}
};

struct StartScan {
    std::vector<std::filesystem::path> roots;
    explicit StartScan(const nlohmann::json& payload) {
        if (!payload.contains("roots")) return;
        for (const auto& root : payload.at("roots"))
            roots.emplace_back(root.get<std::string>());
    }
};

struct AddPlugin {
    std::uint32_t track_id;
    std::string path;
    std::string class_id;
    explicit AddPlugin(const nlohmann::json& payload)
        : track_id(payload.at("trackId").get<std::uint32_t>()),
          path(payload.at("path").get<std::string>()),
          class_id(payload.contains("classId")
                       ? payload.at("classId").get<std::string>()
                       : std::string{}) {}
};

struct Instance {
    std::uint32_t instance_id;
    explicit Instance(const nlohmann::json& payload)
        : instance_id(payload.at("instanceId").get<std::uint32_t>()) {}
};

struct MovePlugin {
    std::uint32_t instance_id;
    std::size_t new_index;
    explicit MovePlugin(const nlohmann::json& payload)
        : instance_id(payload.at("instanceId").get<std::uint32_t>()),
          new_index(payload.at("newIndex").get<std::size_t>()) {}
};

struct PastePlugin {
    std::string clipboard_id;
    std::uint32_t track_id;
    std::size_t new_index;
    explicit PastePlugin(const nlohmann::json& payload)
        : clipboard_id(payload.at("clipboardId").get<std::string>()),
          track_id(payload.at("trackId").get<std::uint32_t>()),
          new_index(payload.at("newIndex").get<std::size_t>()) {}
};

struct DuplicatePlugin {
    std::uint32_t instance_id, track_id;
    std::size_t new_index;
    explicit DuplicatePlugin(const nlohmann::json& payload)
        : instance_id(payload.at("instanceId").get<std::uint32_t>()),
          track_id(payload.at("trackId").get<std::uint32_t>()),
          new_index(payload.at("newIndex").get<std::size_t>()) {}
};

struct Bypass {
    std::uint32_t instance_id;
    bool bypassed;
    explicit Bypass(const nlohmann::json& payload)
        : instance_id(payload.at("instanceId").get<std::uint32_t>()),
          bypassed(payload.at("bypassed").get<bool>()) {}
};

struct RetryPlugin {
    std::uint32_t instance_id;
    std::optional<std::string> path;
    explicit RetryPlugin(const nlohmann::json& payload)
        : instance_id(payload.at("instanceId").get<std::uint32_t>()) {
        if (payload.contains("path")) path = payload.at("path").get<std::string>();
    }
};

struct SetParam {
    std::uint32_t instance_id;
    std::uint32_t param_id;
    double value;
    explicit SetParam(const nlohmann::json& payload)
        : instance_id(payload.at("instanceId").get<std::uint32_t>()),
          param_id(payload.at("paramId").get<std::uint32_t>()),
          value(payload.at("value").get<double>()) {}
};

struct Preset {
    std::uint32_t instance_id;
    std::string path;
    explicit Preset(const nlohmann::json& payload)
        : instance_id(payload.at("instanceId").get<std::uint32_t>()),
          path(payload.at("path").get<std::string>()) {}
};

struct SaveSession {
    std::optional<std::string> path;
    std::optional<std::string> device_key;
    std::optional<std::uint32_t> sample_rate;
    std::optional<std::uint32_t> buffer_size;

    explicit SaveSession(const nlohmann::json& payload) {
        if (payload.at("path").is_string()) path = payload.at("path").get<std::string>();
        if (payload.contains("deviceKey") && payload.at("deviceKey").is_string())
            device_key = payload.at("deviceKey").get<std::string>();
        if (payload.contains("sampleRate") && payload.at("sampleRate").is_number_unsigned())
            sample_rate = payload.at("sampleRate").get<std::uint32_t>();
        if (payload.contains("bufferSize") && payload.at("bufferSize").is_number_unsigned())
            buffer_size = payload.at("bufferSize").get<std::uint32_t>();
    }

    nlohmann::json overrides_json() const {
        auto result = nlohmann::json::object();
        if (device_key) result["deviceKey"] = *device_key;
        if (sample_rate) result["sampleRate"] = *sample_rate;
        if (buffer_size) result["bufferSize"] = *buffer_size;
        return result;
    }
};

struct LoadSession {
    std::filesystem::path path;
    explicit LoadSession(const nlohmann::json& payload)
        : path(payload.at("path").get<std::string>()) {}
};

struct EditorOwner {
    std::uint64_t hwnd;
    explicit EditorOwner(const nlohmann::json& payload)
        : hwnd(payload.at("hwnd").get<std::uint64_t>()) {}
};

}  // namespace rmx::router_request
