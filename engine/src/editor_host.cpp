// Studio Pro 式 editor host 實作。單一 top-level 視窗 + 兩個子視窗:
//   tabs(RmxEditorTabs)= 頂部 56px 自繪列:帶1 plugin tabs、帶2 bypass 電源鈕 +
//                        載入 Preset 鈕 + preset 檔名
//   client(RmxEditorClient)= 深色底,plugin 原生 editor attach 在這
// 執行緒模型:全部 main thread(engine 的 message loop 服務)。host 讀 rack
// 直接讀(rack_ 只在 main thread 變,同執行緒天然序列化);**變更**(bypass /
// preset)必經 EditorHostCmd POST 到 main window,由 main.cpp 的 handler 鎖
// g_engine_mutex 走正規路徑 —— host 的 wnd_proc 可能在 dispatch 持鎖中被
// DestroyWindow 的 sent message 同步重入,host 自身絕不鎖(自死鎖)。
#include "editor_host.hpp"

#include "audio_engine.hpp"
#include "rack.hpp"
#include "vst3_host.hpp"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>

#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace rmx {

namespace {

using namespace Steinberg;

constexpr int kTabH = 28;       // 帶1:plugin tabs
constexpr int kStripH = 64;     // 帶1 tabs 28 + 帶2 控制 36
constexpr int kClientMinW = 80;
constexpr int kClientMinH = 60;
constexpr int kHostMinClientW = 282;  // 帶2 全控(電源+兩鈕,load 右緣 268 + 邊距)

constexpr wchar_t kHostClassName[] = L"RmxVST3Editor";   // 沿用:m3b probe 以 class 數窗
constexpr wchar_t kTabsClassName[] = L"RmxEditorTabs";
constexpr wchar_t kClientClassName[] = L"RmxEditorClient";

#ifndef GWL_HWNDPARENT  // 部分SDK header 條件編譯才給;SetWindowLongPtr 的 owner 欄
#define GWL_HWNDPARENT (-8)
#endif

// 深色主題(zinc 系,貼 app 風格)
// 對齊主程式 ui/app.css 色票(去網頁感第一步 = 同一個深色系)
constexpr COLORREF kStripBg = RGB(0x1c, 0x1f, 0x24);     // = --bg-panel
constexpr COLORREF kTabActiveBg = RGB(0x24, 0x28, 0x2e); // = --bg-raised
constexpr COLORREF kAccent = RGB(0x4d, 0xa3, 0xff);      // = --accent
constexpr COLORREF kTextActive = RGB(0xd8, 0xdc, 0xe2);  // = --text
constexpr COLORREF kTextIdle = RGB(0x8a, 0x91, 0x9b);    // = --text-dim
constexpr COLORREF kTextDim = RGB(0x56, 0x5b, 0x64);
constexpr COLORREF kOnGreen = RGB(0x3d, 0xdc, 0x84);     // = --ok
constexpr COLORREF kBtnBorder = RGB(0x33, 0x38, 0x3f);   // = --border

HFONT strip_font() {
    static HFONT f = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, L"Segoe UI");
    return f;
}

// 背景 process 無權 SetForegroundWindow 搶焦點:借目前前景視窗 thread 的
// input queue(AttachThreadInput)取得許可;沒有前景視窗時直接呼叫即可
void foreground_window(HWND wnd) noexcept {
    // 最佳努力帶前景。**不能在主 thread 直接做**:AttachThreadInput /
    // SetForegroundWindow 內部有跨 thread sync send,對方 thread 半死時會把
    // main 的 message loop 卡住(probe 實測過)。dance 丟 helper thread ——
    // attach 對象必須是「呼叫 SetForegroundWindow 的 thread」= helper 自己
    // (在主 thread 捕捉 tid 會 attach 錯邊,helper 沒接到 input queue 權限)。
    // 窗先滅/tid 已死 = 呼叫無效返回,不卡
    std::thread([wnd] {
        for (int i = 0; i < 20; ++i) {
            if (GetForegroundWindow() == wnd) break;
            const HWND fg = GetForegroundWindow();
            const DWORD fg_tid = fg != nullptr ? GetWindowThreadProcessId(fg, nullptr) : 0;
            const DWORD my_tid = GetCurrentThreadId();
            if (fg_tid != 0 && fg_tid != my_tid) AttachThreadInput(my_tid, fg_tid, TRUE);
            const BOOL ok = SetForegroundWindow(wnd);
            if (fg_tid != 0 && fg_tid != my_tid) AttachThreadInput(my_tid, fg_tid, FALSE);
            if (ok) break;
            Sleep(50);  // 被拒(對方 thread 忙/沒 pump)→ 等一輪再試,共 ~1s
        }
    }).detach();
}

// IPlugFrame:plugin 要求 resize editor → 調 host 視窗外框(client 高 + strip)。
// host 持有一個,同時最多一個 view 在附著
class EditorPlugFrame final : public IPlugFrame {
public:
    HWND hwnd{};        // host top-level(視窗銷毀時清 null,擋遲到的 resizeView)
    int extra_cy = kStripH;

    tresult PLUGIN_API queryInterface(const TUID requested_iid, void** obj) override {
        if (obj == nullptr) return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(requested_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(requested_iid, IPlugFrame::iid)) {
            *obj = static_cast<IPlugFrame*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++references_; }
    uint32 PLUGIN_API release() override {
        const auto remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    // IPlugFrame 契約:plugin resize editor → resizeView;之後 host 須呼 view->onSize
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override {
        if (hwnd == nullptr || r == nullptr || view == nullptr) return kInvalidArgument;
        int w = r->right - r->left;
        int h = r->bottom - r->top;
        if (w <= 0 || h <= 0) return kInvalidArgument;
        w = std::clamp(w, kClientMinW, 4096);
        h = std::clamp(h, kClientMinH, 4096);
        RECT rc{0, 0, w, h + extra_cy};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ViewRect clamped{0, 0, w, h};
        view->onSize(&clamped);  // 契約:視窗 resize 完成後通知 plugin
        return kResultOk;
    }

private:
    std::atomic<uint32> references_{1};
};

// 帶2 控制項的矩形(client 座標;帶高 36,控制項上下留 4~8px)
RECT power_rect(int /*cw*/) noexcept { return {8, kTabH + 4, 44, kStripH - 4}; }
RECT save_btn_rect(int /*cw*/) noexcept { return {52, kTabH + 7, 156, kStripH - 7}; }
RECT load_btn_rect(int /*cw*/) noexcept { return {164, kTabH + 7, 268, kStripH - 7}; }
RECT preset_name_rect(int cw) noexcept { return {278, kTabH + 4, cw - 10, kStripH - 4}; }

LRESULT CALLBACK host_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) noexcept;
LRESULT CALLBACK tabs_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) noexcept;
LRESULT CALLBACK client_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) noexcept;

ATOM register_class(const wchar_t* name, WNDPROC proc, HBRUSH bg, UINT style) noexcept {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = bg;
    wc.lpszClassName = name;
    return RegisterClassExW(&wc);
}

HBRUSH dark_brush() noexcept {
    static const HBRUSH b = CreateSolidBrush(kStripBg);
    return b;
}

HBRUSH tab_active_brush() noexcept {
    static const HBRUSH b = CreateSolidBrush(kTabActiveBg);
    return b;
}

HBRUSH accent_brush() noexcept {
    static const HBRUSH b = CreateSolidBrush(kAccent);
    return b;
}

// 對話框 modal 期間 dispatch 可重入(rack 可能變),套用前以 id 重查 slot
bool pick_preset_file(HWND owner, std::wstring& out) noexcept {
    IFileOpenDialog* dlg = nullptr;
    if (CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)) != S_OK)
        return false;
    bool ok = false;
    const COMDLG_FILTERSPEC specs[] = {{L"VST3 Preset", L"*.vstpreset"},
                                       {L"所有檔案", L"*.*"}};
    dlg->SetFileTypes(2, specs);
    dlg->SetOptions(FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"載入 VST3 Preset");
    if (dlg->Show(owner) == S_OK) {
        IShellItem* item = nullptr;
        if (dlg->GetResult(&item) == S_OK && item != nullptr) {
            PWSTR path = nullptr;
            if (item->GetDisplayName(SIGDN_FILESYSPATH, &path) == S_OK && path != nullptr) {
                out = path;
                ok = true;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

bool pick_preset_save(HWND owner, const std::wstring& default_name, std::wstring& out) noexcept {
    IFileSaveDialog* dlg = nullptr;
    if (CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IFileSaveDialog, reinterpret_cast<void**>(&dlg)) != S_OK)
        return false;
    bool ok = false;
    const COMDLG_FILTERSPEC specs[] = {{L"VST3 Preset", L"*.vstpreset"}};
    dlg->SetFileTypes(1, specs);
    dlg->SetDefaultExtension(L"vstpreset");
    if (!default_name.empty()) dlg->SetFileName(default_name.c_str());
    dlg->SetTitle(L"儲存 VST3 Preset");
    if (dlg->Show(owner) == S_OK) {
        IShellItem* item = nullptr;
        if (dlg->GetResult(&item) == S_OK && item != nullptr) {
            PWSTR path = nullptr;
            if (item->GetDisplayName(SIGDN_FILESYSPATH, &path) == S_OK && path != nullptr) {
                out = path;
                ok = true;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

void post_host_cmd(HWND target, int kind, std::uint32_t id, std::wstring path = {}) {
    if (target == nullptr) return;
    auto* cmd = new EditorHostCmd{kind, id, std::move(path)};
    if (!PostMessageW(target, WM_APP_HOSTCMD, 0, reinterpret_cast<LPARAM>(cmd))) delete cmd;
}

}  // namespace

struct EditorHost::Impl {
    AudioEngine* engine = nullptr;
    HWND post_to = nullptr;  // EditorHostCmd 投遞目標(main window)
    HWND owner = nullptr;    // UI 主視窗(owned 浮動視窗;null = 獨立)
    HWND wnd = nullptr, tabs = nullptr, client = nullptr;
    std::uint32_t active_id = 0;
    std::vector<std::uint32_t> opened_ids;
    std::wstring preset_name;  // 當前 active slot 顯示用的 preset 檔名(切換即清)
    struct Tab {
        std::uint32_t id;
        std::wstring name;
        bool dim;      // 無 editor / 上次 attach 失敗
        bool bypass;
    };
    std::vector<Tab> tabs_data;
    IPtr<EditorPlugFrame> frame;

    ~Impl() {
        if (wnd != nullptr) DestroyWindow(wnd);  // WM_DESTROY 內 detach + 清欄位
    }

    const RackSlot* find_slot(std::uint32_t id) const noexcept {
        return engine != nullptr ? engine->find_slot(id) : nullptr;
    }

    // 以 tracks 現況重建 tab 快取 + 重繪(main thread 直接讀,無需鎖)
    void render_tabs() {
        tabs_data.clear();
        if (engine == nullptr) return;
        for (const auto& t : engine->plugin_tabs()) {
            tabs_data.push_back(
                {t.instance_id, to_wide(t.label), !t.editor_capable, t.bypass});
        }
        if (tabs != nullptr) InvalidateRect(tabs, nullptr, FALSE);
    }

    // 原生 editor 比 client 小(plugin 不縮放,如 kHs Gain)→ 置中,周圍留深色底
    void center_editor_child() noexcept {
        if (client == nullptr) return;
        HWND child = FindWindowExW(client, nullptr, nullptr, nullptr);
        if (child == nullptr) return;
        RECT wr{};
        GetWindowRect(child, &wr);
        POINT tl{wr.left, wr.top};
        MapWindowPoints(nullptr, client, &tl, 1);
        RECT cr{};
        GetClientRect(client, &cr);
        const int w = wr.right - wr.left;
        const int h = wr.bottom - wr.top;
        const int x = (cr.right - cr.left - w) / 2;
        const int y = (cr.bottom - cr.top - h) / 2;
        if (x != tl.x || y != tl.y)
            SetWindowPos(child, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void layout_children() noexcept {
        if (wnd == nullptr || tabs == nullptr || client == nullptr) return;  // WM_SIZE 在子視窗建立前也會來
        RECT rc{};
        GetClientRect(wnd, &rc);
        const int cw = rc.right - rc.left;
        const int ch = rc.bottom - rc.top;
        MoveWindow(tabs, 0, 0, cw, kStripH, TRUE);
        MoveWindow(client, 0, kStripH, cw, ch - kStripH, TRUE);
        if (const auto* slot = find_slot(active_id); slot != nullptr && slot->plugin->editor_open())
            slot->plugin->editor_resize_view(cw, ch - kStripH);
        center_editor_child();
    }

    void resize_to_client(int w, int h) noexcept {
        RECT rc{0, 0, w, h + kStripH};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(wnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void set_title(std::uint32_t id) {
        const auto* slot = find_slot(id);
        const std::wstring title =
            slot != nullptr ? L"RoudaMix — " + to_wide(slot->name) : L"RoudaMix";
        SetWindowTextW(wnd, title.c_str());
    }

    // 沒窗就建(含子視窗)。回傳「本次新建」(呼叫端在 attach 失敗時要拆)
    bool ensure_window() {
        if (wnd != nullptr) return false;
        // class 只註冊一次(RegisterClassExW 對重複註冊回 0,非錯誤訊號)
        static const ATOM host_atom = register_class(kHostClassName, &host_wnd_proc,
                                                     dark_brush(),
                                                     CS_HREDRAW | CS_VREDRAW);
        if (host_atom == 0) return false;
        static bool children_registered = [] {
            return register_class(kTabsClassName, &tabs_wnd_proc, dark_brush(), 0) != 0 &&
                   register_class(kClientClassName, &client_wnd_proc, dark_brush(), 0) != 0;
        }();
        if (!children_registered) return false;
        if (frame == nullptr) frame = owned(new EditorPlugFrame());
        // WS_EX_TOOLWINDOW:不進工作列/Alt-Tab(Studio Pro 浮動視窗感);
        // owner 是 UI 主視窗 → owned 視窗永在主程式之上、隨其最小化。
        // owner 可能已死(UI 重啟 race):IsWindow 不過 = 退回獨立視窗
        const HWND own = owner != nullptr && IsWindow(owner) ? owner : nullptr;
        // WS_VISIBLE:create 即顯示。spawn engine 的 STARTUPINFO 帶 SW_HIDE 時
        // (Start-Process -WindowStyle Hidden),首個 top-level 視窗的第一個
        // ShowWindow 呼叫會被替換成 startup 的 SW_HIDE —— 視窗建了但永遠 hidden
        wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClassName, L"RoudaMix",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              480, 360, own, nullptr, GetModuleHandleW(nullptr), this);
        if (wnd == nullptr) return false;
        // 深色標題列:預設亮色 caption 在深色主題裡 = 割裂感(舊 Win10 無 35/36 = 略過,
        // 20 號 immersive dark 從 1809+ 就有)
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(wnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const COLORREF cap = kStripBg, brc = kBtnBorder, txt = kTextActive;
        DwmSetWindowAttribute(wnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
        DwmSetWindowAttribute(wnd, DWMWA_BORDER_COLOR, &brc, sizeof(brc));
        DwmSetWindowAttribute(wnd, DWMWA_TEXT_COLOR, &txt, sizeof(txt));
        frame->hwnd = wnd;
        frame->extra_cy = kStripH;
        tabs = CreateWindowExW(0, kTabsClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 480,
                               kStripH, wnd, nullptr, GetModuleHandleW(nullptr), this);
        client = CreateWindowExW(0, kClientClassName, L"", WS_CHILD | WS_VISIBLE, 0, kStripH,
                                 480, 300, wnd, nullptr, GetModuleHandleW(nullptr), this);
        return tabs != nullptr && client != nullptr;
    }

    // 核心:detach 當前 → attach id 的 editor → 尺寸/標題/前景/tab。caller 保證
    // id 存在且(多半)editor_capable。新窗 attach 失敗會拆窗
    bool activate(std::uint32_t id, std::string& err) {
        const auto* slot = find_slot(id);
        if (slot == nullptr) {
            err = "unknown instanceId";
            return false;
        }
        const bool fresh = ensure_window();
        if (wnd == nullptr) {
            err = "editor window creation failed";
            return false;
        }
        if (frame == nullptr) frame = owned(new EditorPlugFrame());
        frame->hwnd = wnd;
        // detach 舊(destroy-on-switch:同時只有一個活 view)
        if (const auto* cur = find_slot(active_id);
            cur != nullptr && active_id != id && cur->plugin->editor_open())
            cur->plugin->close_editor();
        active_id = 0;
        preset_name.clear();
        auto plugin = slot->plugin;  // shared_ptr:attach 期間釘住生命週期
        // editor 內改參數 → performEdit → set_param 同語意(main thread,無並發)
        plugin->set_param_callback(
            [eng = engine, id](std::uint32_t param_id, double value) {
                std::string e;
                eng->set_param(id, param_id, value, e);
            });
        int w = 0, h = 0;
        if (!plugin->attach_editor(client, frame.get(), w, h)) {
            err = plugin->last_error();
            if (fresh) destroy_window();
            render_tabs();
            return false;
        }
        active_id = id;
        if (std::find(opened_ids.begin(), opened_ids.end(), id) == opened_ids.end())
            opened_ids.push_back(id);
        resize_to_client(w, h);
        set_title(id);
        ShowWindow(wnd, SW_SHOW);
        foreground_window(wnd);
        render_tabs();
        return true;
    }

    // active 遺失後的自動切換:opened_ids 依 tab 序找第一個能 attach 的。
    // 絕不自動開使用者沒開過的 plugin(能力會 resurrect 關掉的 editor = 翻來覆去)
    bool switch_to_next() {
        std::string err;
        for (const auto& t : engine->plugin_tabs()) {
            if (std::find(opened_ids.begin(), opened_ids.end(), t.instance_id) ==
                opened_ids.end())
                continue;
            if (activate(t.instance_id, err)) return true;
        }
        return false;
    }

    void destroy_window() noexcept {
        if (wnd != nullptr) DestroyWindow(wnd);  // 清理在 WM_DESTROY
    }
};

EditorHost& EditorHost::instance() noexcept {
    static EditorHost host;
    if (host.impl_ == nullptr) host.impl_ = new Impl();
    return host;
}

EditorHost::~EditorHost() {
    delete impl_;
}

void EditorHost::set_engine(AudioEngine* engine) noexcept {
    impl_->engine = engine;
}

void EditorHost::set_command_target(HWND post_to) noexcept {
    impl_->post_to = post_to;
}

void EditorHost::set_owner(HWND owner) noexcept {
    impl_->owner = owner;
    if (impl_->wnd != nullptr && owner != nullptr && IsWindow(owner))
        SetWindowLongPtrW(impl_->wnd, GWL_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
}

bool EditorHost::open(std::uint32_t instance_id, std::string& err) {
    if (impl_->engine == nullptr) {
        err = "engine not ready";
        return false;
    }
    if (impl_->find_slot(instance_id) == nullptr) {
        err = "unknown instanceId";
        return false;
    }
    if (impl_->wnd != nullptr && impl_->active_id == instance_id) {
        foreground_window(impl_->wnd);  // 冪等:已顯示 = 帶回前景
        return true;
    }
    return impl_->activate(instance_id, err);
}

void EditorHost::close(std::uint32_t instance_id) noexcept {
    auto& ids = impl_->opened_ids;
    ids.erase(std::remove(ids.begin(), ids.end(), instance_id), ids.end());
    if (impl_->wnd == nullptr) return;  // 沒窗 = 全 detach 過了
    if (impl_->active_id == instance_id) {
        if (const auto* slot = impl_->find_slot(instance_id);
            slot != nullptr && slot->plugin->editor_open())
            slot->plugin->close_editor();
        impl_->active_id = 0;
        impl_->preset_name.clear();
        if (!impl_->switch_to_next()) impl_->destroy_window();
        else impl_->render_tabs();
    }
}

void EditorHost::notify_tracks_changed() {
    if (impl_->wnd == nullptr) return;
    auto& ids = impl_->opened_ids;
    // prune:開過的 id 可能已被 remove_plugin 移除
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [this](std::uint32_t id) {
                                 return impl_->find_slot(id) == nullptr;
                             }),
              ids.end());
    // active 被移除(remove_plugin 已 detach 它的 view)→ 自動切換或關窗
    if (impl_->active_id != 0 && impl_->find_slot(impl_->active_id) == nullptr) {
        impl_->active_id = 0;
        impl_->preset_name.clear();
        if (!impl_->switch_to_next()) {
            impl_->destroy_window();
            return;
        }
    }
    impl_->render_tabs();
}

void EditorHost::shutdown() noexcept {
    impl_->destroy_window();
}

namespace {

// ---- host top-level ----

LRESULT CALLBACK host_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    if (msg == WM_NCCREATE) {
        // 建立時把 EditorHost::Impl* 存進 GWLP_USERDATA;不存則之後 self 永遠
        // null,WM_DESTROY 清理整段被跳過 → 重開 editor 撞冪等檢查靜默失敗
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, msg, wp, lp);
    }
    auto* self = reinterpret_cast<EditorHost::Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (self != nullptr) {
            if (self->frame != nullptr) self->frame->hwnd = nullptr;
            if (const auto* slot = self->find_slot(self->active_id);
                slot != nullptr && slot->plugin->editor_open())
                slot->plugin->close_editor();  // detach view(X 關窗;plugin 不滅)
            self->wnd = self->tabs = self->client = nullptr;
            self->active_id = 0;
            self->preset_name.clear();
            self->opened_ids.clear();
        }
        return 0;
    case WM_SIZE:
        if (self != nullptr && wp != SIZE_MINIMIZED) self->layout_children();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        // 寬下限 = 帶2 全控能完整顯示(不是 editor 需求 —— editor 小就置中留深色底)
        RECT rc{0, 0, kHostMinClientW, kClientMinH + kStripH};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }
    case WM_SETFOCUS:
        // 點 tab 列/邊框搶走 focus 會斷 plugin 鍵盤輸入 → 轉給 client
        if (self != nullptr && self->client != nullptr) SetFocus(self->client);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

// ---- 頂部列:帶1 tabs + 帶2 bypass/preset(全部自繪 + hit-test,單一 HWND)----

void draw_power_icon(HDC dc, int cx, int cy, double rad, bool bypassed) {
    // 電源鍵(比照 UI 端使用者提供 SVG):開 = 綠 #16A34A、bypass = 黑。
    // GDI 筆無反鋸齒 + 折線頂點取整數 = 線條抖;改 GDI+(AA + 浮點座標)。
    // 圓(頂部 90° 開口)+ 豎線從頂穿到圓心;筆寬 = SVG 40/236 比例。
    static const ULONG_PTR gdip_token = [] {
        Gdiplus::GdiplusStartupInput in;
        ULONG_PTR t = 0;
        Gdiplus::GdiplusStartup(&t, &in, nullptr);
        return t;
    }();
    (void)gdip_token;
    const Gdiplus::Color color = bypassed ? Gdiplus::Color(255, 0, 0, 0)
                                          : Gdiplus::Color(255, 0x16, 0xA3, 0x4A);
    const float penw = (std::max)(2.0f, static_cast<float>(rad * 40.0 / 236.0));
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(color, penw);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float r = static_cast<float>(rad);
    // 螢幕座標(y 向下)0°=右、90°=下;從右上 315° 順時針掃 270° = 頂部 90° 開口
    g.DrawArc(&pen, static_cast<float>(cx) - r, static_cast<float>(cy) - r, 2 * r, 2 * r,
              315.0f, 270.0f);
    g.DrawLine(&pen, static_cast<float>(cx), static_cast<float>(cy) - r - penw,
               static_cast<float>(cx), static_cast<float>(cy));
}

HPEN btn_border_pen() noexcept {
    static const HPEN p = CreatePen(PS_SOLID, 1, kBtnBorder);
    return p;
}

// 圓角平面按鈕(raised 填色 + 1px 邊框 + 置中文字;半徑對齊主程式 6px,圓太大 = 網頁感)
void draw_modern_button(HDC dc, RECT r, const wchar_t* text) {
    const HGDIOBJ old_pen = SelectObject(dc, btn_border_pen());
    const HGDIOBJ old_brush = SelectObject(dc, tab_active_brush());
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    SetTextColor(dc, kTextActive);
    DrawTextW(dc, text, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

LRESULT CALLBACK tabs_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, msg, wp, lp);
    }
    auto* self = reinterpret_cast<EditorHost::Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        const int cw = rc.right - rc.left;
        FillRect(dc, &rc, dark_brush());
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, strip_font());
        const auto& tabs = self->tabs_data;
        if (!tabs.empty()) {
            const int tw = (std::min)(160, cw / static_cast<int>(tabs.size()));
            for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
                const auto& t = tabs[static_cast<size_t>(i)];
                const bool active = t.id == self->active_id;
                RECT tr{i * tw, 0, (i + 1) * tw, kTabH - 1};
                if (active) {
                    FillRect(dc, &tr, tab_active_brush());
                    RECT line{i * tw, kTabH - 3, (i + 1) * tw, kTabH - 1};
                    FillRect(dc, &line, accent_brush());
                }
                RECT text = tr;
                text.left += 8;
                text.right -= 8;
                SetTextColor(dc, t.dim ? kTextDim : active ? kTextActive : kTextIdle);
                DrawTextW(dc, t.name.c_str(), -1, &text,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        // 帶1/帶2 分隔線
        RECT div{0, kTabH - 1, cw, kTabH};
        FillRect(dc, &div, tab_active_brush());
        if (self->active_id != 0) {
            bool bypassed = false;
            if (const auto* slot = self->find_slot(self->active_id); slot != nullptr)
                bypassed = slot->bypass;
            const RECT pr = power_rect(cw);
            draw_power_icon(dc, (pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2,
                            (pr.bottom - pr.top) / 2.0 - 3.0, bypassed);
            if (save_btn_rect(cw).right < cw)
                draw_modern_button(dc, save_btn_rect(cw), L"儲存 Preset");
            if (load_btn_rect(cw).right < cw)
                draw_modern_button(dc, load_btn_rect(cw), L"載入 Preset");
            RECT nr = preset_name_rect(cw);
            SetTextColor(dc, kTextIdle);
            DrawTextW(dc, self->preset_name.c_str(), -1, &nr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (self == nullptr || self->engine == nullptr) break;
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        RECT rc{};
        GetClientRect(h, &rc);
        const int cw = rc.right - rc.left;
        const auto& tabs = self->tabs_data;
        if (y < kTabH) {
            if (tabs.empty()) break;
            const int tw = (std::min)(160, cw / static_cast<int>(tabs.size()));
            const int idx = x / tw;
            if (idx < 0 || idx >= static_cast<int>(tabs.size())) break;
            const auto& t = tabs[static_cast<size_t>(idx)];
            if (t.dim || t.id == self->active_id) break;
            std::string err;
            self->activate(t.id, err);  // 失敗 = dim + 空 client(文件化行為)
        } else if (self->active_id != 0) {
            const POINT pt{x, y};
            RECT pr = power_rect(cw);
            RECT sr = save_btn_rect(cw);
            RECT lr = load_btn_rect(cw);
            if (PtInRect(&pr, pt)) {
                post_host_cmd(self->post_to, kHostBypass, self->active_id);
            } else if (PtInRect(&sr, pt) && sr.right < cw) {
                const auto* slot = self->find_slot(self->active_id);
                const std::wstring def =
                    slot != nullptr ? to_wide(slot->name) + L".vstpreset" : L"";
                std::wstring path;
                // owner = host 視窗:對話框期間停用 host(避免 modal 中切換)
                if (pick_preset_save(self->wnd, def, path)) {
                    self->preset_name = path.substr(path.find_last_of(L'\\') + 1);
                    InvalidateRect(h, nullptr, FALSE);
                    post_host_cmd(self->post_to, kHostSavePreset, self->active_id, path);
                }
            } else if (PtInRect(&lr, pt) && lr.right < cw) {
                std::wstring path;
                if (pick_preset_file(self->wnd, path)) {
                    self->preset_name = path.substr(path.find_last_of(L'\\') + 1);
                    InvalidateRect(h, nullptr, FALSE);
                    post_host_cmd(self->post_to, kHostPreset, self->active_id, path);
                }
            }
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- client:plugin editor 的 attach 父(深色底)----

LRESULT CALLBACK client_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    switch (msg) {
    case WM_ERASEBKGND: {
        // 深色填滿:plugin editor 沒鋪滿時不會露白
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc{};
        GetClientRect(h, &rc);
        FillRect(dc, &rc, dark_brush());
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);  // 全靠 ERASEBKGND 填色
        return 0;
    }
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

}  // namespace

}  // namespace rmx
