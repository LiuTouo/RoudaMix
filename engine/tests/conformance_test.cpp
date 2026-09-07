// fixtures conformance:valid 必解、invalid 必拒(契約:contracts/protocol.md §9)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "command_contract.hpp"
#include "protocol.hpp"

namespace fs = std::filesystem;

static int g_fail = 0;

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void check_dir(const fs::path& dir, bool must_pass) {
    if (!fs::exists(dir)) {
        std::printf("MISSING DIR: %s\n", dir.string().c_str());
        ++g_fail;
        return;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) files.push_back(e.path());
    if (files.empty()) {
        std::printf("EMPTY DIR (fixtures missing): %s\n", dir.string().c_str());
        ++g_fail;
        return;
    }
    for (const auto& p : files) {
        nlohmann::json j = nlohmann::json::parse(read_file(p), nullptr, false);
        bool pass;
        if (j.is_discarded()) {
            pass = !must_pass;
        } else {
            try {
                rmx::parse_frame(j, /*strict=*/true);
                pass = must_pass;
            } catch (const rmx::ParseError&) {
                pass = !must_pass;
            }
        }
        if (!pass) {
            std::printf("FAIL(%s): %s\n", must_pass ? "valid" : "invalid",
                        p.filename().string().c_str());
            ++g_fail;
        }
    }
}

void expect_result_valid(const std::string& kind, const nlohmann::json& result) {
    try {
        rmx::contract::validate_result(kind, result);
    } catch (const rmx::contract::ValidationError& error) {
        std::printf("FAIL(valid result %s): %s\n", kind.c_str(), error.what());
        ++g_fail;
    }
}

void expect_result_invalid(const std::string& kind, const nlohmann::json& result,
                           const char* case_name) {
    try {
        rmx::contract::validate_result(kind, result);
        std::printf("FAIL(invalid result %s/%s): accepted\n", kind.c_str(), case_name);
        ++g_fail;
    } catch (const rmx::contract::ValidationError&) {
    }
}

void check_command_result_schemas() {
    const std::unordered_map<std::string, nlohmann::json> valid_results = {
        {"ping", {{"engineVersion", "0.1.0"}}},
        {"get_snapshot", {{"snapshot", nlohmann::json::object()}}},
        {"get_latency_report",
         {{"report",
           {{"generation", 0}, {"ok", true}, {"error", ""}, {"bufferBytes", 0},
            {"outputs", nlohmann::json::array()}, {"edges", nlohmann::json::array()},
            {"tracks", nlohmann::json::array()}}}}},
        {"list_devices", {{"devices", nlohmann::json::array()}}},
        {"list_audio_apps", {{"apps", nlohmann::json::array()}}},
        {"list_render_devices", {{"devices", nlohmann::json::array()}}},
        {"list_capture_devices", {{"devices", nlohmann::json::array()}}},
        {"start", nlohmann::json::object()},
        {"stop", nlohmann::json::object()},
        {"open_device_panel", {{"panel", true}}},
        {"track_add", {{"trackId", 1}, {"tracks", nlohmann::json::array()}}},
        {"track_remove", {{"tracks", nlohmann::json::array()}}},
        {"track_set", {{"tracks", nlohmann::json::array()}}},
        {"track_set_source", {{"tracks", nlohmann::json::array()}}},
        {"track_set_dests", {{"tracks", nlohmann::json::array()}}},
        {"track_set_output", {{"tracks", nlohmann::json::array()}}},
        {"track_set_output_latency_policy", {{"tracks", nlohmann::json::array()}}},
        {"track_move", {{"tracks", nlohmann::json::array()}}},
        {"start_scan", {{"jobId", 1}, {"reused", false}}},
        {"cancel_scan", {{"jobId", 1}, {"cancelling", true}}},
        {"add_plugin",
         {{"instanceId", 1}, {"trackId", 2}, {"tracks", nlohmann::json::array()}}},
        {"copy_plugin", {{"clipboardId", "1"}, {"name", "Fixture"}}},
        {"paste_plugin",
         {{"instanceId", 2}, {"trackId", 2}, {"tracks", nlohmann::json::array()}}},
        {"duplicate_plugin",
         {{"instanceId", 3}, {"trackId", 2}, {"tracks", nlohmann::json::array()}}},
        {"remove_plugin", {{"tracks", nlohmann::json::array()}}},
        {"move_plugin", {{"tracks", nlohmann::json::array()}}},
        {"set_bypass", {{"tracks", nlohmann::json::array()}}},
        {"set_monitor_bypass", {{"tracks", nlohmann::json::array()}}},
        {"retry_plugin", {{"instanceId", 1}, {"tracks", nlohmann::json::array()}}},
        {"set_param", nlohmann::json::object()},
        {"get_params", {{"instanceId", 1}, {"params", nlohmann::json::array()}}},
        {"open_editor", {{"instanceId", 1}, {"editor", true}}},
        {"close_editor", nlohmann::json::object()},
        {"save_preset", {{"savedPath", "preset.vstpreset"}}},
        {"load_preset", {{"tracks", nlohmann::json::array()}}},
        {"save_session", {{"savedPath", "mix.rmsession"}, {"revision", 1}}},
        {"load_session", {{"revision", 1}}},
        {"ensure_system_outputs",
         {{"tracks", nlohmann::json::array()}, {"revision", 1}}},
        {"set_editor_owner", nlohmann::json::object()},
        {"shutdown_engine", nlohmann::json::object()},
    };

    const auto& commands = rmx::contract::table().at("commands");
    if (valid_results.size() != commands.size()) {
        std::printf("FAIL(result coverage): %zu samples for %zu commands\n",
                    valid_results.size(), commands.size());
        ++g_fail;
    }
    for (const auto& command : commands) {
        const auto kind = command.at("kind").get<std::string>();
        const auto sample = valid_results.find(kind);
        if (sample == valid_results.end()) {
            std::printf("FAIL(result coverage): missing %s\n", kind.c_str());
            ++g_fail;
            continue;
        }
        expect_result_valid(kind, sample->second);
    }

    expect_result_invalid("ping", nlohmann::json::object(), "missing required field");
    expect_result_invalid("set_param", {{"unexpected", true}}, "unknown field");
    expect_result_invalid("ping", {{"engineVersion", 1}}, "wrong field type");
}

int main(int argc, char** argv) {
    fs::path fixtures = argc > 1 ? fs::path(argv[1])
                                 : fs::path("fixtures") / "protocol";
    check_dir(fixtures / "valid", /*must_pass=*/true);
    check_dir(fixtures / "invalid", /*must_pass=*/false);
    check_command_result_schemas();
    if (g_fail == 0) {
        std::printf("conformance: all pass\n");
        return 0;
    }
    std::printf("conformance: %d failure(s)\n", g_fail);
    return 1;
}
