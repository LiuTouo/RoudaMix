// Session 檔案(.rmsession,UTF-8 JSON)— 契約:contracts/protocol.md §8 SessionFile。
// save 只序列化;load 逐項重建 rack(壞 slot 略過不整體失敗)+ 套 source。
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

// serialize + 寫檔;overrides.deviceKey/sampleRate(UI 帶目前選的裝置)存在時蓋寫;
// 失敗回 false(err 帶原因)
bool save(const AudioEngine& engine, const std::filesystem::path& file, std::string& err,
          const nlohmann::json& overrides = nlohmann::json::object());

// 讀檔 + 重建:清空 rack → 逐一 add(載入失敗的 slot 略過)→ bypass/params → source。
// 不動裝置(不自動 start);session 的 deviceKey/sampleRate 原樣放 applied 給 caller。
// 檔案結構壞(非 JSON / 版本不符)= false;個別 slot 壞不 fail。
bool load(AudioEngine& engine, const std::filesystem::path& file, nlohmann::json& applied,
          std::string& err);

}  // namespace rmx::session
