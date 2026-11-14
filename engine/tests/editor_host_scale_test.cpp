#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <windowsx.h>

#include "command_contract.hpp"
#include "editor_host.hpp"
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
    // 具真實 rect 的 UI 主視窗替身(message-only target 的 GetWindowRect 是空的,
    // 測不了預設位置):預設位置與動態置頂都以它為基準。
    const HWND ui_owner = CreateWindowExW(0, L"RmxScaleTestTarget", L"", WS_OVERLAPPED,
                                          200, 100, 800, 600, nullptr, nullptr,
                                          GetModuleHandleW(nullptr), nullptr);
    CHECK(ui_owner != nullptr);
    MoveWindow(ui_owner, 200, 100, 800, 600, FALSE);
    ok("set_editor_owner", {{"hwnd", static_cast<std::uintptr_t>(
        reinterpret_cast<std::uintptr_t>(ui_owner))}});
    ok("open_editor", {{"instanceId", plugin}});
    const HWND host = FindWindowW(L"RmxVST3Editor", nullptr);
    CHECK(host != nullptr);
    CHECK(!IsWindowVisible(host));  // 測試模式:視窗建而不顯(建置跑批不閃現)
    const HWND tabs = FindWindowExW(host, nullptr, L"RmxEditorTabs", nullptr);
    const HWND client = FindWindowExW(host, nullptr, L"RmxEditorClient", nullptr);
    const HWND child = FindWindowExW(client, nullptr, L"RmxEditorScaleFixture", nullptr);
    CHECK(tabs && client && child);
    // 預設位置:主介面假想中線右側(左緣 >= 中線 + 12px 隨縮放)、垂直置中
    // (插件 attach 撐到原生尺寸後重新對中,中心不偏離 UI 視窗中心)。
    // 建窗即 TOPMOST(開啟當下 RoudaMix 必是前景)。兩者都必須在 pump 之前
    // 檢查:pump 會派送真實前景 hook 事件(測試機前景是 console = 非自家 →
    // 正確解除 TOPMOST)。
    const auto owner_rect = bounds(ui_owner);
    const auto host_rect = bounds(host);
    CHECK(host_rect.left >= owner_rect.left + (owner_rect.right - owner_rect.left) / 2
              + MulDiv(12, initial_percent, 100));
    const LONG ui_cy = owner_rect.top + (owner_rect.bottom - owner_rect.top) / 2;
    const LONG ed_cy = host_rect.top + (host_rect.bottom - host_rect.top) / 2;
    CHECK((ed_cy > ui_cy ? ed_cy - ui_cy : ui_cy - ed_cy)
              <= MulDiv(24, initial_percent, 100));
    CHECK((GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    pump();
    // 受控前景的動態置頂(直接餵 hwnd 給 update_topmost,不依賴測試機真實
    // 前景;hook 事件只在 pump 派送,不會插入這幾行同步呼叫):
    if (HWND shell = GetShellWindow();
        shell != nullptr && GetWindowThreadProcessId(shell, nullptr) != GetCurrentProcessId()) {
        rmx::EditorHost::instance().update_topmost(shell);
        CHECK((GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0);
        rmx::EditorHost::instance().update_topmost(ui_owner);
        CHECK((GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
        rmx::EditorHost::instance().update_topmost(host);
        CHECK((GetWindowLongPtrW(host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    }
    // 跟隨:UI 主視窗移動(拖到別處/別螢幕)→ 編輯器同量平移保留相對位置;
    // 落點在虛擬桌面內(含跨螢幕/局部越界)= 照 delta 走不夾限(夾在所在
    // 螢幕會把跨螢幕的編輯器釘在舊螢幕邊緣),整個出桌面才拉回 UI 所在螢幕
    // 工作區。期望值與 on_ui_moved 同構鏡像,任何桌面拓撲下都確定性。同步
    // 入口後,pump 派送的真實 hook 事件 delta 已歸零,冪等不會二次平移。
    LONG prev_ui_left = 200, prev_ui_top = 100;
    const auto move_ui_and_expect = [&](LONG new_left, LONG new_top) {
        const auto hb0 = bounds(host);  // 實作是鏈式:當前位置 + 本次 delta
        MoveWindow(ui_owner, new_left, new_top, 800, 600, FALSE);
        rmx::EditorHost::instance().ui_moved(ui_owner, OBJID_WINDOW);
        const LONG w = hb0.right - hb0.left;
        const LONG h = hb0.bottom - hb0.top;
        LONG ex = hb0.left + (new_left - prev_ui_left);
        LONG ey = hb0.top + (new_top - prev_ui_top);
        RECT vr{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                0, 0};
        vr.right = vr.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        vr.bottom = vr.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        RECT moved{ex, ey, ex + w, ey + h}, inter{};
        IntersectRect(&inter, &moved, &vr);
        if (IsRectEmpty(&inter)) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            CHECK(GetMonitorInfoW(MonitorFromWindow(ui_owner, MONITOR_DEFAULTTONEAREST), &mi));
            const RECT& wa = mi.rcWork;
            ex = std::clamp<LONG>(ex, wa.left, (std::max)(wa.left, wa.right - w));
            ey = std::clamp<LONG>(ey, wa.top, (std::max)(wa.top, wa.bottom - h));
        }
        const auto hb = bounds(host);
        CHECK(hb.left == ex && hb.top == ey);
        prev_ui_left = new_left;
        prev_ui_top = new_top;
    };
    move_ui_and_expect(337, 57);         // 一般拖曳(含跨螢幕落點)= 同量平移
    move_ui_and_expect(337 - 2400, 57);  // 極端 offset:整個出桌面 → 拉回 UI 螢幕

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
    DestroyWindow(ui_owner);
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
    rmx::EditorHost::instance().hide_windows_for_tests();
    for (int mode : {1, 2, 3})
        for (int percent : {75, 100, 150}) check_editor(std::filesystem::absolute(argv[1]).string(), mode, percent, target);
    DestroyWindow(target);
    SetEnvironmentVariableW(L"ROUDAMIX_TEST_EDITOR_MODE", nullptr);
    CoUninitialize();
    std::puts("editor_host_scale: all pass");
    return 0;
}
