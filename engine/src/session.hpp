// Session 檔案(.rmsession,UTF-8 JSON)— 契約:contracts/protocol.md §8 SessionFile。
// v3:Output Latency Policy + Monitor Bypass；v2 自動遷移，v1 拒載。load 逐軌重建(壞軌/消失 module
// 略過不整體失敗)、dests 以舊 id→新 id map 重接。
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "audio_engine.hpp"
#include "failure.hpp"
#include "sandbox.hpp"

namespace rmx::session {

// 預設路徑:%APPDATA%\RoudaMix\default.rmsession(不存在 APPDATA 用目前目錄)
std::filesystem::path default_path();

// engine 現況 → session JSON 物件(deviceKey/sampleRate = 最近一次成功 start 的值)
nlohmann::json serialize(const AudioEngine& engine);

// serialize + 寫檔;overrides.deviceKey/sampleRate/bufferSize(UI 帶目前選的裝置)
// 存在時蓋寫;失敗回 Failure(session_io)
std::optional<Failure> save(const AudioEngine& engine, const std::filesystem::path& file,
                            const nlohmann::json& overrides = nlohmann::json::object());

// 讀檔 + 重建:清空全部軌 → 逐軌 track_add → source/output/gain/mute → plugins →
// dests(舊→新 id map)。不動裝置(不自動 start);session 的 deviceKey/sampleRate/
// bufferSize 原樣放 applied 給 caller。v2 會遷移到 v3 defaults；v1 失敗(session_io)。
std::optional<Failure> load(AudioEngine& engine, const std::filesystem::path& file,
                            nlohmann::json& applied);

// Sandbox fail-closed preflight 的單一來源:依 engine 現況推導 rate/block 後送 worker
// preflight。回傳失敗類別(kNone = 通過);error 帶 worker 訊息。Router 的
// add/retry_plugin 與 session load 都走這裡,不得各寫一份推導。
sandbox::PreflightFailure preflight_plugin(const AudioEngine& engine,
                                           const std::string& module_path,
                                           const std::string& class_id, std::string& error);

}  // namespace rmx::session
