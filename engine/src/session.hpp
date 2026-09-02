// Session 檔案(.rmsession,UTF-8 JSON)— 契約:contracts/protocol.md §8 SessionFile。
// v3:Output Latency Policy + Monitor Bypass；v2 自動遷移，v1 拒載。load 逐軌重建(壞軌/消失 module
// 略過不整體失敗)、dests 以舊 id→新 id map 重接。
#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "audio_engine.hpp"

namespace rmx::session {

// 預設路徑:%APPDATA%\RoudaMix\default.rmsession(不存在 APPDATA 用目前目錄)
std::filesystem::path default_path();

// engine 現況 → session JSON 物件(deviceKey/sampleRate = 最近一次成功 start 的值)
nlohmann::json serialize(const AudioEngine& engine);

// serialize + 寫檔;overrides.deviceKey/sampleRate/bufferSize(UI 帶目前選的裝置)
// 存在時蓋寫;失敗回 false(err 帶原因)
bool save(const AudioEngine& engine, const std::filesystem::path& file, std::string& err,
          const nlohmann::json& overrides = nlohmann::json::object());

// 讀檔 + 重建:清空全部軌 → 逐軌 track_add → source/output/gain/mute → plugins →
// dests(舊→新 id map)。不動裝置(不自動 start);session 的 deviceKey/sampleRate/
// bufferSize 原樣放 applied 給 caller。v2 會遷移到 v3 defaults；v1 一律 false。
bool load(AudioEngine& engine, const std::filesystem::path& file, nlohmann::json& applied,
          std::string& err);

}  // namespace rmx::session
