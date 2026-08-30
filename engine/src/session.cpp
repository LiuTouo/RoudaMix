#include "session.hpp"

#include <cstdio>
#include <cstdlib>

namespace rmx::session {

namespace {
constexpr int kSessionVersion = 1;
}

std::filesystem::path default_path() {
    const char* appdata = std::getenv("APPDATA");
    std::filesystem::path base = (appdata != nullptr && *appdata)
                                     ? std::filesystem::path(appdata) / "RoudaMix"
                                     : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(base, ec);  // 已存在不報錯
    return base / "default.rmsession";
}

nlohmann::json serialize(const AudioEngine& engine) {
    const auto st = engine.status();
    nlohmann::json rack = nlohmann::json::array();
    for (const auto& s : engine.rack()) {
        nlohmann::json params = nlohmann::json::array();
        for (const auto& [id, v] : s.param_values)
            params.push_back({{"paramId", id}, {"normalized", v}});
        rack.push_back({
            {"pluginPath", s.module_path},
            {"classId", s.class_id},
            {"name", s.name},
            {"bypassed", s.bypass},
            {"params", params},
        });
    }
    return nlohmann::json{
        {"roudamixSession", kSessionVersion},
        {"deviceKey", !engine.last_device_key().empty()
                          ? nlohmann::json(engine.last_device_key())
                          : nlohmann::json(nullptr)},
        {"sampleRate", engine.last_sample_rate() > 0 ? nlohmann::json(engine.last_sample_rate())
                                                     : nlohmann::json(nullptr)},
        {"bufferSize", engine.last_buffer_size() > 0
                           ? nlohmann::json(engine.last_buffer_size())
                           : nlohmann::json(nullptr)},
        {"source", st.source},
        {"sineFreq", st.sine_freq},
        {"inputMono", st.input_mono},
        {"rack", rack},
    };
}

bool save(const AudioEngine& engine, const std::filesystem::path& file, std::string& err,
          const nlohmann::json& overrides) {
    nlohmann::json j = serialize(engine);
    if (overrides.is_object()) {
        if (overrides.contains("deviceKey") && overrides["deviceKey"].is_string())
            j["deviceKey"] = overrides["deviceKey"];
        if (overrides.contains("sampleRate") && overrides["sampleRate"].is_number_unsigned())
            j["sampleRate"] = overrides["sampleRate"];
        if (overrides.contains("bufferSize") && overrides["bufferSize"].is_number_unsigned())
            j["bufferSize"] = overrides["bufferSize"];
        if (overrides.contains("inputMono") && overrides["inputMono"].is_boolean())
            j["inputMono"] = overrides["inputMono"];
    }
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || f == nullptr) {
        err = "cannot open session file for writing: " + file.string();
        return false;
    }
    const std::string text = j.dump(2);
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!wrote) {
        err = "session file write failed: " + file.string();
        return false;
    }
    return true;
}

bool load(AudioEngine& engine, const std::filesystem::path& file, nlohmann::json& applied,
          std::string& err) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || f == nullptr) {
        err = "cannot open session file: " + file.string();
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("roudamixSession") ||
        !j["roudamixSession"].is_number_integer() ||
        j["roudamixSession"].get<int>() != kSessionVersion) {
        err = "not a RoudaMix session file (roudamixSession != 1)";
        return false;
    }

    // 舊 rack 全清(instanceId 不保留 — load 後是新 instance);先快照 ids,remove 會動 vector
    std::vector<std::uint32_t> old_ids;
    for (const auto& s : engine.rack()) old_ids.push_back(s.instance_id);
    for (const auto id : old_ids) {
        std::string drop_err;
        engine.remove_plugin(id, drop_err);
    }

    if (j.contains("rack") && j["rack"].is_array()) {
        for (const auto& sr : j["rack"]) {
            if (!sr.is_object() || !sr.contains("pluginPath") || !sr["pluginPath"].is_string())
                continue;  // 壞 slot 略過不整體失敗
            std::string class_id;
            if (sr.contains("classId") && sr["classId"].is_string())
                class_id = sr["classId"].get<std::string>();
            std::uint32_t instance_id = 0;
            std::string add_err;
            if (!engine.add_plugin(sr["pluginPath"].get<std::string>(), class_id, instance_id,
                                   add_err))
                continue;  // module 消失/載入失敗:略過
            std::string op_err;
            if (sr.contains("bypassed") && sr["bypassed"].is_boolean())
                engine.set_bypass(instance_id, sr["bypassed"].get<bool>(), op_err);
            if (sr.contains("params") && sr["params"].is_array()) {
                for (const auto& p : sr["params"]) {
                    if (p.is_object() && p.contains("paramId") && p.contains("normalized") &&
                        p["paramId"].is_number_unsigned() && p["normalized"].is_number())
                        engine.set_param(instance_id, p["paramId"].get<std::uint32_t>(),
                                         p["normalized"].get<double>(), op_err);
                }
                // set_param 只餵 RT;controller 也推,開 plugin GUI 才會顯示場景值
                engine.sync_controller_params(instance_id);
            }
        }
    }

    if (j.contains("source") && j["source"].is_string() && j.contains("sineFreq") &&
        j["sineFreq"].is_number()) {
        std::string op_err;
        engine.set_source(j["source"].get<std::string>() == "passthrough",
                          j["sineFreq"].get<float>(), op_err);
    }

    applied = nlohmann::json{
        {"deviceKey", j.contains("deviceKey") && j["deviceKey"].is_string()
                          ? j["deviceKey"]
                          : nlohmann::json(nullptr)},
        {"sampleRate", j.contains("sampleRate") && j["sampleRate"].is_number_unsigned()
                           ? j["sampleRate"]
                           : nlohmann::json(nullptr)},
        {"bufferSize", j.contains("bufferSize") && j["bufferSize"].is_number_unsigned()
                           ? j["bufferSize"]
                           : nlohmann::json(nullptr)},
        {"inputMono", j.contains("inputMono") && j["inputMono"].is_boolean()
                          ? j["inputMono"]
                          : nlohmann::json(nullptr)},
    };
    return true;
}

}  // namespace rmx::session
