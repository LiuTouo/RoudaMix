#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "router.hpp"

#include "command_contract.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;

nlohmann::json read_json(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream content;
    content << file.rdbuf();
    return nlohmann::json::parse(content.str());
}

void expect_equal(const nlohmann::json& actual, const nlohmann::json& expected,
                  const char* message) {
    // frame_io::write_frame 送出的 payload 正是 json.dump()；比較 dump 才會連
    // number representation 與 envelope key serialization 一起鎖住。
    if (actual.dump() == expected.dump()) return;
    std::fprintf(stderr, "FAIL: %s\nexpected: %s\nactual:   %s\n", message,
                 expected.dump().c_str(), actual.dump().c_str());
    ++failures;
}

void request_reply_fixture(const fs::path& fixture_path) {
    const auto fixture = read_json(fixture_path);
    expect_equal(fixture.at("baselineCommit"), "fa1eb53bda529dbd2371c2171bb5defe80f824e7",
                 "router fixture records its pre-refactor baseline");
    const auto& request = fixture.at("request");
    rmx::Command command;
    command.protocol_version = request.at("protocolVersion").get<std::uint32_t>();
    command.id = request.at("id").get<std::uint64_t>();
    command.kind = request.at("kind").get<std::string>();
    command.payload = request.at("payload");

    rmx::Router router;
    std::vector<nlohmann::json> frames;
    router.connect(1, [&](const nlohmann::json& frame) { frames.push_back(frame); });
    frames.clear();  // connection snapshot belongs to a separate protocol event
    (void)router.dispatch_guarded(1, command);

    expect_equal(frames, fixture.at("frames"), "router request/reply fixture");
}

void connection_snapshot_fixture(const fs::path& fixture_path) {
    const auto fixture = read_json(fixture_path);
    expect_equal(fixture.at("baselineCommit"), "fa1eb53bda529dbd2371c2171bb5defe80f824e7",
                 "snapshot fixture records its pre-refactor baseline");
    rmx::Router router;
    std::vector<nlohmann::json> frames;
    router.connect(7, [&](const nlohmann::json& frame) { frames.push_back(frame); });
    expect_equal(frames, fixture.at("frames"), "router connection snapshot fixture");
}

rmx::Command track_add_command(std::uint64_t id, const char* name) {
    return rmx::Command{rmx::kProtocolVersion, id, "track_add",
                        {{"kind", "audio"}, {"name", name}, {"color", 0}}};
}

void concurrent_commands_match_a_serial_sequence() {
    const auto first = track_add_command(101, "Concurrent A");
    const auto second = track_add_command(102, "Concurrent B");
    rmx::Router concurrent;
    std::vector<nlohmann::json> concurrent_frames;
    concurrent.connect(1, [&](const nlohmann::json& frame) {
        concurrent_frames.push_back(frame);
    });
    concurrent_frames.clear();

    std::thread a([&] { (void)concurrent.dispatch_guarded(1, first); });
    std::thread b([&] { (void)concurrent.dispatch_guarded(1, second); });
    a.join();
    b.join();

    std::vector<rmx::Command> observed_order;
    for (const auto& frame : concurrent_frames) {
        if (!frame.contains("id")) continue;
        observed_order.push_back(frame.at("id") == first.id ? first : second);
    }

    rmx::Router serial;
    std::vector<nlohmann::json> serial_frames;
    serial.connect(1, [&](const nlohmann::json& frame) { serial_frames.push_back(frame); });
    serial_frames.clear();
    for (const auto& command : observed_order)
        (void)serial.dispatch_guarded(1, command);

    expect_equal(concurrent_frames, serial_frames,
                 "concurrent command replies match one serial execution order");
}

const nlohmann::json* find_reply(const std::vector<nlohmann::json>& frames,
                                 std::uint64_t id) {
    for (const auto& frame : frames)
        if (frame.contains("id") && frame.at("id") == id) return &frame;
    return nullptr;
}

void session_rebuild_advances_revision_once() {
    const auto session_path = fs::temp_directory_path() /
                              ("roudamix-router-" +
                               std::to_string(GetCurrentProcessId()) + ".rmsession");
    std::error_code ignored;
    fs::remove(session_path, ignored);

    rmx::Router router;
    std::vector<nlohmann::json> frames;
    router.connect(1, [&](const nlohmann::json& frame) { frames.push_back(frame); });
    frames.clear();

    (void)router.dispatch_guarded(
        1, rmx::Command{rmx::kProtocolVersion, 201, "ensure_system_outputs",
                        nlohmann::json::object()});
    (void)router.dispatch_guarded(
        1, rmx::Command{rmx::kProtocolVersion, 202, "save_session",
                        {{"path", session_path.string()}}});
    (void)router.dispatch_guarded(1, track_add_command(203, "Temporary Track"));
    (void)router.dispatch_guarded(
        1, rmx::Command{rmx::kProtocolVersion, 204, "load_session",
                        {{"path", session_path.string()}}});

    const auto* ensured = find_reply(frames, 201);
    const auto* saved = find_reply(frames, 202);
    const auto* loaded = find_reply(frames, 204);
    if (ensured == nullptr || saved == nullptr || loaded == nullptr) {
        std::fprintf(stderr, "FAIL: revision test replies are missing\n");
        ++failures;
    } else {
        expect_equal(ensured->at("result").at("revision"), 1,
                     "ensure_system_outputs advances revision once");
        expect_equal(saved->at("result").at("revision"), 1,
                     "save_session does not advance revision");
        expect_equal(loaded->at("result").at("revision"), 3,
                     "load_session rebuild advances revision once");
    }
    fs::remove(session_path, ignored);
}

void error_replies_carry_declared_codes() {
    rmx::Router router;
    std::vector<nlohmann::json> frames;
    router.connect(1, [&](const nlohmann::json& frame) { frames.push_back(frame); });
    frames.clear();

    // track_set 未知軌 = bad_command(宣告集內);回覆只帶 error、不帶 result
    (void)router.dispatch_guarded(
        1, rmx::Command{rmx::kProtocolVersion, 301, "track_set",
                        {{"trackId", 999}, {"gain", 1.5}}});
    (void)router.dispatch_guarded(
        1, rmx::Command{rmx::kProtocolVersion, 302, "track_remove", {{"trackId", 999}}});

    const auto* bad = find_reply(frames, 301);
    const auto* missing = find_reply(frames, 302);
    if (bad == nullptr || missing == nullptr) {
        std::fprintf(stderr, "FAIL: error reply test replies are missing\n");
        ++failures;
        return;
    }
    expect_equal(bad->at("ok"), false, "track_set unknown track fails");
    expect_equal(bad->at("error").at("code"), "bad_command",
                 "error reply carries the produced code");
    expect_equal(missing->at("error").at("code"), "track_not_found",
                 "track_remove unknown track classifies as track_not_found");
    expect_equal(bad->contains("result"), false, "error reply carries no result");
}

void contract_error_declared_set_is_enforced() {
    // 執法表本身:宣告集內 = true、集外 = false(反例)、未知 kind 跳過
    expect_equal(rmx::contract::is_declared_error("start", "already_running"), true,
                 "start declares already_running");
    expect_equal(rmx::contract::is_declared_error("track_set_source", "unsupported_windows"),
                 true, "track_set_source declares unsupported_windows");
    expect_equal(rmx::contract::is_declared_error("start", "plugin_not_found"), false,
                 "start must not emit plugin_not_found (negative case)");
    expect_equal(rmx::contract::is_declared_error("ping", "bad_command"), false,
                 "ping declares no errors (negative case)");
    expect_equal(rmx::contract::is_declared_error("no_such_command", "internal"), true,
                 "unknown kinds skip the declared-set check");
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path fixture_root =
        argc > 1 ? fs::path(argv[1]) : fs::path("fixtures") / "router";
    request_reply_fixture(fixture_root / "ping.json");
    request_reply_fixture(fixture_root / "track_add.json");
    connection_snapshot_fixture(fixture_root / "connect.json");
    concurrent_commands_match_a_serial_sequence();
    session_rebuild_advances_revision_once();
    error_replies_carry_declared_codes();
    contract_error_declared_set_is_enforced();
    if (failures == 0) {
        std::puts("router: all pass");
        return 0;
    }
    return 1;
}
