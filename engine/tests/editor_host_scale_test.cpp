#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <windowsx.h>

#include "command_contract.hpp"
#include "router.hpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(EXIT_FAILURE); } } while (0)

namespace {

int bypass_commands = 0;
LRESULT CALLBACK command_target(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == rmx::WM_APP_HOSTCMD) {
        const auto* command = reinterpret_cast<rmx::EditorHostCmd*>(lp);
        if (command->kind == rmx::kHostBypass) ++bypass_commands;
        delete command;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

RECT bounds(HWND hwnd) {
    RECT rect{};
    CHECK(GetWindowRect(hwnd, &rect));
    return rect;
}

void check_editor(const std::string& module, int mode, int initial_percent, HWND target) {
    const wchar_t mode_text[]{static_cast<wchar_t>(L'0' + mode), L'\0'};
    SetEnvironmentVariableW(L"ROUDAMIX_TEST_EDITOR_MODE", mode_text);
    rmx::Router router;
    router.set_editor_command_target(target);
    std::vector<nlohmann::json> frames;
    std::uint64_t id = 0;
    router.connect(1, [&](const auto& frame) { frames.push_back(frame); });
    auto ok = [&](const std::string& kind, const nlohmann::json& payload) {
        rmx::contract::validate_command_payload(kind, payload);
        frames.clear();
        CHECK(!router.dispatch_guarded(1, {rmx::kProtocolVersion, ++id, kind, payload}));
        for (const auto& frame : frames) {
            if (!frame.contains("id") || frame["id"] != id) continue;
            if (!frame["ok"].get<bool>()) std::fprintf(stderr, "%s: %s\n", kind.c_str(), frame.dump().c_str());
            CHECK(frame["ok"].get<bool>());
            rmx::contract::validate_result(kind, frame["result"]);
            return frame["result"];
        }
        CHECK(false); return nlohmann::json{};
    };
    auto scale = [&](int percent) {
        ok("set_editor_scale", {{"percent", percent}});
        CHECK(frames.size() == 1); // no status broadcast / audio graph mutation
        pump();
    };
    scale(initial_percent); // 新開的視窗也必須沿用比例
    const auto track = ok("track_add", {{"kind", "audio"}, {"name", "Test"}})["trackId"];
    const auto plugin = ok("add_plugin", {{"trackId", track}, {"path", module}})["instanceId"];
    ok("set_plugin_name", {{"instanceId", plugin}, {"name", "A"}});
    ok("open_editor", {{"instanceId", plugin}});
    pump();
    const HWND host = FindWindowW(L"RmxVST3Editor", nullptr);
    CHECK(host != nullptr);
    const HWND tabs = FindWindowExW(host, nullptr, L"RmxEditorTabs", nullptr);
    const HWND client = FindWindowExW(host, nullptr, L"RmxEditorClient", nullptr);
    const HWND child = FindWindowExW(client, nullptr, L"RmxEditorScaleFixture", nullptr);
    CHECK(tabs && client && child);
    const auto child_initial = bounds(child);
    CHECK(child_initial.right - child_initial.left == (mode == 3 ? 360 : 300));
    CHECK(child_initial.bottom - child_initial.top == (mode == 3 ? 220 : 180));
    const auto snapshot = ok("get_snapshot", nlohmann::json::object())["snapshot"];
    const auto resize_count = GetPropW(child, L"resize-count");
    const auto check_geometry = [&](int percent) {
        const auto hr = bounds(host), tr = bounds(tabs), cr = bounds(client), pr = bounds(child);
        CHECK(pr.right - pr.left == child_initial.right - child_initial.left);
        CHECK(pr.bottom - pr.top == child_initial.bottom - child_initial.top);
        CHECK(tr.top - hr.top == MulDiv(32, percent, 100));
        CHECK(tr.bottom - tr.top == MulDiv(28, percent, 100) + MulDiv(36, percent, 100));
        CHECK(cr.top == tr.bottom);
        CHECK(pr.left >= cr.left && pr.top >= cr.top && pr.right <= cr.right && pr.bottom <= cr.bottom);
        CHECK(hr.right - hr.left >= MulDiv(282, percent, 100));
        CHECK(GetPropW(child, L"resize-count") == resize_count);
        // Caption 關閉鈕與拖曳 hit-test 必須跟著新幾何，不是僅放大繪圖。
        const int cap = MulDiv(32, percent, 100);
        CHECK(SendMessageW(host, WM_NCHITTEST, 0, MAKELPARAM(hr.right - cap / 2, hr.top + cap / 2)) == HTCLIENT);
        CHECK(SendMessageW(host, WM_NCHITTEST, 0, MAKELPARAM(hr.left + cap, hr.top + cap / 2)) == HTCAPTION);
        const int before = bypass_commands;
        SendMessageW(tabs, WM_LBUTTONDOWN, MK_LBUTTON,
                     MAKELPARAM(MulDiv(22, percent, 100), MulDiv(28, percent, 100) + MulDiv(18, percent, 100)));
        pump();
        CHECK(bypass_commands == before + 1);
    };
    check_geometry(initial_percent);
    for (int percent : {75, 100, 125, 150, 100, 75, 150}) {
        scale(percent);
        check_geometry(percent);
    }
    CHECK(ok("get_snapshot", nlohmann::json::object())["snapshot"] == snapshot);

    // 分頁增刪/換行也不能吃掉插件高度或拉伸可調整插件。
    const auto other = ok("add_plugin", {{"trackId", track}, {"path", module}})["instanceId"];
    ok("set_plugin_name", {{"instanceId", other}, {"name", "A reasonably long second plugin name"}});
    pump();
    const auto wrapped = bounds(tabs);
    CHECK(wrapped.bottom - wrapped.top >= MulDiv(28, 150, 100) * 2 + MulDiv(36, 150, 100));
    CHECK(GetPropW(child, L"resize-count") == resize_count);
    CHECK(bounds(client).bottom - bounds(client).top == child_initial.bottom - child_initial.top);
    ok("remove_plugin", {{"instanceId", other}});

    // 插件主動 resizeView 在所有比例下仍使用原生尺寸，縮小也不回彈。
    for (int percent : {75, 150}) {
        scale(percent);
        for (const auto size : {SIZE{420, 260}, SIZE{300, 180}}) {
            const auto before = reinterpret_cast<std::uintptr_t>(GetPropW(child, L"resize-count"));
            CHECK(SendMessageW(child, WM_APP + 1, size.cx, size.cy) == 0);
            pump();
            const auto cr = bounds(client), pr = bounds(child);
            CHECK(pr.right - pr.left == size.cx && pr.bottom - pr.top == size.cy);
            CHECK(pr.left >= cr.left && pr.top >= cr.top && pr.right <= cr.right && pr.bottom <= cr.bottom);
            CHECK(reinterpret_cast<std::uintptr_t>(GetPropW(child, L"resize-count")) == before + 1);
        }
    }
    if (mode == 2) {
        scale(100);
        const auto tr = bounds(tabs);
        // 使用者明確調整外框仍允許可調整的 plugin 收到 onSize。
        SetWindowPos(host, nullptr, 0, 0, 290, 160 + 32 + tr.bottom - tr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump();
        CHECK(bounds(child).right - bounds(child).left == 290);
        CHECK(bounds(child).bottom - bounds(child).top == 160);
        scale(150);
        CHECK(bounds(child).right - bounds(child).left == 290);
        CHECK(bounds(child).bottom - bounds(child).top == 160);
    }
    // 反覆縮放不能累積 GDI 字型物件。
    const DWORD objects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 20; ++i) { scale(i % 2 ? 75 : 150); UpdateWindow(host); UpdateWindow(tabs); }
    CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= objects + 3);
    ok("close_editor", {{"instanceId", plugin}});
    CHECK(!IsWindow(host));
    ok("open_editor", {{"instanceId", plugin}});
    const HWND reopened = FindWindowW(L"RmxVST3Editor", nullptr);
    CHECK(reopened != nullptr);
    CHECK(bounds(FindWindowExW(reopened, nullptr, L"RmxEditorTabs", nullptr)).top - bounds(reopened).top == 24);
    ok("close_editor", {{"instanceId", plugin}});
    router.shutdown();
    std::printf("editor chrome: mode=%d initial=%d PASSED\n", mode, initial_percent);
}

} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2);
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    WNDCLASSW wc{};
    wc.lpfnWndProc = command_target;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RmxScaleTestTarget";
    CHECK(RegisterClassW(&wc));
    const HWND target = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    CHECK(target);
    for (int mode : {1, 2, 3})
        for (int percent : {75, 100, 150}) check_editor(std::filesystem::absolute(argv[1]).string(), mode, percent, target);
    DestroyWindow(target);
    SetEnvironmentVariableW(L"ROUDAMIX_TEST_EDITOR_MODE", nullptr);
    CoUninitialize();
    std::puts("editor_host_scale: all pass");
    return 0;
}
