#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>

#include "audio_engine.hpp"

namespace rmx::session {

nlohmann::json tracks_json(const AudioEngine& engine);
nlohmann::json status_json(const AudioEngine& engine, std::uint64_t revision);
nlohmann::json snapshot_json(const AudioEngine& engine, std::uint64_t epoch,
                             std::uint64_t revision, const nlohmann::json& last_scan);
nlohmann::json latency_report_json(const AudioEngine& engine);

}  // namespace rmx::session
