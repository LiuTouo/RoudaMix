#include "vst_registry.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <io.h>
#include <map>
#include <sstream>

namespace rmx::vst_registry {

namespace {

std::filesystem::path absolute_normal(const std::filesystem::path& path) {
    std::error_code ec;
    auto out = std::filesystem::absolute(path, ec);
    if (ec) out = path;
    return out.lexically_normal();
}

nlohmann::json registry_json(const Registry& registry) {
    auto roots = nlohmann::json::array();
    for (const auto& root : registry.roots) roots.push_back(path_utf8(root));
    auto entries = nlohmann::json::array();
    for (const auto& entry : registry.entries) entries.push_back(entry_json(entry));
    return {
        {"schemaVersion", kSchemaVersion},
        {"scannerVersion", kScannerVersion},
        {"roots", std::move(roots)},
        {"entries", std::move(entries)},
    };
}

}  // namespace

std::filesystem::path cache_path_from_env() {
    const DWORD needed = GetEnvironmentVariableW(kCacheEnv, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(kCacheEnv, value.data(), needed);
    if (written == 0 || written >= needed) return {};
    value.resize(written);
    return std::filesystem::path{value};
}

std::string path_utf8(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                      static_cast<int>(wide.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string text(static_cast<std::size_t>(n), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), text.data(), n,
                            nullptr, nullptr) != n)
        return {};
    return text;
}

std::wstring path_key(const std::filesystem::path& path) {
    std::wstring key = absolute_normal(path).wstring();
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return key;
}

std::filesystem::path path_from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), wide.data(), n) != n)
        return {};
    return std::filesystem::path{wide};
}

bool fingerprint(const std::filesystem::path& path, Fingerprint& out,
                 std::string& error) {
    std::error_code ec;
    const bool regular = std::filesystem::is_regular_file(path, ec);
    if (ec) {
        error = "cannot inspect VST module: " + path_utf8(path) + ": " + ec.message();
        return false;
    }
    const bool directory = !regular && std::filesystem::is_directory(path, ec);
    if (ec || (!regular && !directory)) {
        error = "not a readable VST file or bundle directory: " + path_utf8(path);
        return false;
    }

    std::uint64_t total_size = 0;
    auto latest = std::filesystem::last_write_time(path, ec);
    if (ec) {
        error = "cannot read last-write time: " + path_utf8(path) + ": " + ec.message();
        return false;
    }
    if (regular) {
        total_size = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
        if (ec) {
            error = "cannot read file size: " + path_utf8(path) + ": " + ec.message();
            return false;
        }
    } else {
        std::filesystem::recursive_directory_iterator it{
            path, std::filesystem::directory_options::skip_permission_denied, ec};
        if (ec) {
            error = "cannot enumerate VST bundle: " + path_utf8(path) + ": " + ec.message();
            return false;
        }
        for (decltype(it) end; it != end; it.increment(ec)) {
            if (ec) {
                error = "cannot enumerate VST bundle: " + path_utf8(path) + ": " +
                        ec.message();
                return false;
            }
            std::error_code item_ec;
            const auto item_modified = std::filesystem::last_write_time(it->path(), item_ec);
            if (!item_ec && item_modified > latest) latest = item_modified;
            if (it->is_regular_file(item_ec) && !item_ec) {
                const auto size = std::filesystem::file_size(it->path(), item_ec);
                if (!item_ec) total_size += static_cast<std::uint64_t>(size);
            }
        }
    }
    out.size = total_size;
    out.modified = static_cast<std::int64_t>(latest.time_since_epoch().count());
    error.clear();
    return true;
}

nlohmann::json entry_json(const Entry& entry) {
    nlohmann::json out{
        {"path", path_utf8(entry.path)},
        {"size", entry.fingerprint.size},
        {"modified", entry.fingerprint.modified},
    };
    if (!entry.error.empty())
        out["error"] = entry.error;
    else
        out["classes"] = entry.classes;
    return out;
}

bool entry_from_json(const nlohmann::json& json, Entry& out) {
    if (!json.is_object() || !json.contains("path") || !json["path"].is_string() ||
        !json.contains("size") || !json["size"].is_number_unsigned() ||
        !json.contains("modified") || !json["modified"].is_number_integer())
        return false;
    const auto path = path_from_utf8(json["path"].get<std::string>());
    if (path.empty()) return false;

    Entry candidate;
    candidate.path = absolute_normal(path);
    candidate.fingerprint.size = json["size"].get<std::uint64_t>();
    candidate.fingerprint.modified = json["modified"].get<std::int64_t>();
    if (json.contains("error") && json["error"].is_string() &&
        !json["error"].get_ref<const std::string&>().empty()) {
        candidate.error = json["error"].get<std::string>();
    } else if (json.contains("classes") && json["classes"].is_array() &&
               !json["classes"].empty()) {
        for (const auto& plugin_class : json["classes"]) {
            if (!plugin_class.is_object()) return false;
            for (const char* field : {"uid", "name", "vendor", "version",
                                      "subcategories"})
                if (!plugin_class.contains(field) || !plugin_class[field].is_string())
                    return false;
        }
        candidate.classes = json["classes"];
    } else {
        return false;
    }
    out = std::move(candidate);
    return true;
}

void normalize(Registry& registry) {
    std::map<std::wstring, Entry> entries;
    for (auto& entry : registry.entries) {
        if (entry.path.empty()) continue;
        entry.path = absolute_normal(entry.path);
        entries[path_key(entry.path)] = std::move(entry);
    }
    registry.entries.clear();
    registry.entries.reserve(entries.size());
    for (auto& [_, entry] : entries) registry.entries.push_back(std::move(entry));

    std::map<std::wstring, std::filesystem::path> roots;
    for (auto& root : registry.roots) {
        if (root.empty()) continue;
        root = absolute_normal(root);
        roots[path_key(root)] = std::move(root);
    }
    registry.roots.clear();
    registry.roots.reserve(roots.size());
    for (auto& [_, root] : roots) registry.roots.push_back(std::move(root));
}

bool load(const std::filesystem::path& file, Registry& out, std::string& error) {
    out = {};
    error.clear();
    if (file.empty()) return true;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return !ec;
    if (ec) {
        error = "cannot inspect VST registry: " + ec.message();
        return false;
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "cannot open VST registry: " + path_utf8(file);
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto json = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (json.is_discarded() || !json.is_object() ||
        !json.contains("schemaVersion") || !json["schemaVersion"].is_number_unsigned() ||
        json["schemaVersion"].get<std::uint32_t>() != kSchemaVersion ||
        !json.contains("scannerVersion") || !json["scannerVersion"].is_number_unsigned() ||
        json["scannerVersion"].get<std::uint32_t>() != kScannerVersion ||
        !json.contains("roots") || !json["roots"].is_array() ||
        !json.contains("entries") || !json["entries"].is_array()) {
        error = "invalid or incompatible VST registry";
        return false;
    }

    Registry candidate;
    for (const auto& root : json["roots"]) {
        if (!root.is_string()) {
            error = "invalid VST registry root";
            return false;
        }
        const auto path = path_from_utf8(root.get<std::string>());
        if (path.empty()) {
            error = "invalid UTF-8 path in VST registry";
            return false;
        }
        candidate.roots.push_back(path);
    }
    for (const auto& item : json["entries"]) {
        Entry entry;
        if (!entry_from_json(item, entry)) {
            error = "invalid VST registry entry";
            return false;
        }
        candidate.entries.push_back(std::move(entry));
    }
    normalize(candidate);
    out = std::move(candidate);
    return true;
}

bool save_atomic(const std::filesystem::path& file, const Registry& registry,
                 std::string& error) {
    error.clear();
    if (file.empty()) return true;
    std::error_code ec;
    if (const auto parent = file.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "cannot create VST registry directory: " + ec.message();
            return false;
        }
    }

    auto tmp = file;
    tmp += L".tmp";
    auto bak = file;
    bak += L".bak";
    const std::string text = registry_json(registry).dump(2);
    {
        std::FILE* handle = nullptr;
        if (_wfopen_s(&handle, tmp.c_str(), L"wb") != 0 || handle == nullptr) {
            error = "cannot open VST registry temp file";
            return false;
        }
        const bool wrote = std::fwrite(text.data(), 1, text.size(), handle) == text.size();
        const bool flushed = wrote && std::fflush(handle) == 0 && _commit(_fileno(handle)) == 0;
        std::fclose(handle);
        if (!flushed) {
            std::filesystem::remove(tmp, ec);
            error = "cannot write/flush VST registry temp file";
            return false;
        }
    }

    const bool had_old = std::filesystem::exists(file, ec) && !ec;
    if (had_old) {
        std::filesystem::remove(bak, ec);
        ec.clear();
        std::filesystem::rename(file, bak, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            error = "cannot back up previous VST registry: " + ec.message();
            return false;
        }
    }
    ec.clear();
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::error_code rollback;
        if (had_old) std::filesystem::rename(bak, file, rollback);
        error = "cannot replace VST registry: " + ec.message();
        return false;
    }
    return true;
}

const Entry* find_unchanged(const Registry& registry,
                            const std::filesystem::path& path,
                            const Fingerprint& value) {
    const auto key = path_key(path);
    for (const auto& entry : registry.entries)
        if (path_key(entry.path) == key && entry.fingerprint == value) return &entry;
    return nullptr;
}

nlohmann::json plugins_json(const Registry& registry) {
    auto plugins = nlohmann::json::array();
    for (const auto& entry : registry.entries)
        if (entry.error.empty() && entry.classes.is_array() && !entry.classes.empty())
            plugins.push_back({{"path", path_utf8(entry.path)}, {"classes", entry.classes}});
    return plugins;
}

nlohmann::json failures_json(const Registry& registry) {
    auto failures = nlohmann::json::array();
    for (const auto& entry : registry.entries)
        if (!entry.error.empty())
            failures.push_back({{"path", path_utf8(entry.path)}, {"error", entry.error}});
    return failures;
}

}  // namespace rmx::vst_registry
