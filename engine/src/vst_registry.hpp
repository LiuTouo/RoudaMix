// VST 掃描 registry：跨啟動持久快取 + 檔案 fingerprint。
// engine 與 sandbox worker 共用，避免兩邊各自解讀不同格式。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace rmx::vst_registry {

inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::uint32_t kScannerVersion = 2;
inline constexpr wchar_t kCacheEnv[] = L"ROUDAMIX_VST_REGISTRY";

struct Fingerprint {
    std::uint64_t size{};
    std::int64_t modified{};

    bool operator==(const Fingerprint&) const = default;
};

struct Entry {
    std::filesystem::path path;
    Fingerprint fingerprint;
    nlohmann::json classes = nlohmann::json::array();
    std::string error;
};

struct Registry {
    std::vector<std::filesystem::path> roots;
    std::vector<Entry> entries;
};

// Tauri spawn engine 時設定；worker 繼承同一環境變數。未設定代表停用持久化。
std::filesystem::path cache_path_from_env();

// Windows 路徑 JSON 一律 UTF-8；key 正規化且不分大小寫，用於去重/查找。
std::string path_utf8(const std::filesystem::path& path);
std::wstring path_key(const std::filesystem::path& path);

bool fingerprint(const std::filesystem::path& path, Fingerprint& out,
                 std::string& error);

// missing 檔視為乾淨空 registry；壞檔/版本不符回 false 並清空 out。
bool load(const std::filesystem::path& file, Registry& out, std::string& error);

// temp + flush + backup + rename；失敗不破壞正式檔。
bool save_atomic(const std::filesystem::path& file, const Registry& registry,
                 std::string& error);

const Entry* find_unchanged(const Registry& registry,
                            const std::filesystem::path& path,
                            const Fingerprint& fingerprint);

// worker JSONL 與 cache 共用 entry shape；嚴格驗證後才接受。
nlohmann::json entry_json(const Entry& entry);
bool entry_from_json(const nlohmann::json& json, Entry& out);

// 去除重疊 root 造成的重複路徑，並固定排序，讓 UI/快取結果穩定。
void normalize(Registry& registry);

nlohmann::json plugins_json(const Registry& registry);
nlohmann::json failures_json(const Registry& registry);

}  // namespace rmx::vst_registry
