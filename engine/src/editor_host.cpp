// Studio Pro 式 editor host 實作。單一 top-level 視窗,自繪 caption(無 DWM
// 標題列)+ 兩個子視窗:
//   caption(視窗自己畫)= 頂部 32px:視窗標題 + 關閉鈕,霜面語言與帶列一致
//   tabs(RmxEditorTabs)= 帶1 plugin tabs、帶2 bypass 電源鈕 +
//                        載入 Preset 鈕 + preset 檔名
//   client(RmxEditorClient)= 深色底,plugin 原生 editor attach 在這
// 執行緒模型:全部 main thread(engine 的 message loop 服務)。host 讀 rack
// 直接讀(rack_ 只在 main thread 變,同執行緒天然序列化);**變更**(bypass /
// preset)必經 EditorHostCmd POST 到 main window,由 main.cpp 的 handler 鎖
// Router 臨界區走正規路徑 —— host 的 wnd_proc 可能在 dispatch 持鎖中被
// DestroyWindow 的 sent message 同步重入,host 自身絕不鎖(自死鎖)。
// 視窗拓撲:host 是**無 owner 的獨立 top-level 視窗**(WS_EX_TOPMOST 浮上面)。
// 不可用跨 process 的 UI 主視窗當 owner:跨進程 ownership 會把兩邊 thread 的
// input queue 接在一起,焦點/前景狀態共用,關閉 host 時的 close 手勢可能落在
// 主視窗上(實害:彈出視窗關閉連帶主視窗被關)。
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
constexpr int kCaptionH = 32;   // 自繪標題列(取代 DWM caption,同高)
constexpr int kStripH = 64;     // 帶1 tabs 28 + 帶2 控制 36
constexpr int kClientMinW = 80;
constexpr int kClientMinH = 60;
constexpr int kHostMinClientW = 282;  // 帶2 全控(電源+兩鈕,load 右緣 268 + 邊距)

constexpr wchar_t kHostClassName[] = L"RmxVST3Editor";   // 沿用:m3b probe 以 class 數窗
constexpr wchar_t kTabsClassName[] = L"RmxEditorTabs";
constexpr wchar_t kClientClassName[] = L"RmxEditorClient";

// (跨進程 owner 已移除,不再需要 GWL_HWNDPARENT)

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

// ---- 霜面帶列（自繪毛玻璃感）----
// 曾嘗試 OS 級 blur：WCA accent acrylic 在 24H2 每幀重套，帶列以 ~15Hz
// 交替明滅 = 使用者看到的「瘋狂閃爍」；DWMWA_SYSTEMBACKDROP_TYPE（Mica／
// Acrylic）在 26100 上 S_OK 卻永不渲染（最小視窗 + 訊息泵實測）。OS blur
// 在本機不可用 → 改自繪霜面底紋：垂直漸層 + 固定種子細噪點，單次 BitBlt
// 組合，結構上不可能閃，且任何機器（含 Win10 portable）外觀一致。

// 霜面用色（zinc 系，帶1 頂微亮 → 帶2 底微深）
constexpr COLORREF kFrostTop = RGB(0x2a, 0x2e, 0x35);
constexpr COLORREF kFrostBottom = RGB(0x19, 0x1c, 0x20);

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
    // 窗先滅/tid 已死 = 呼叫無效返回,不卡。
    // **重試上限 3**:正常路徑(使用者剛在 UI 點開,attach 前景 thread 後第
    // 一次就成了;失敗也在第 2~3 次內定局)。probe 實測對上不讓路的系統件
    // 時,20 次 x 50ms 的 attach/SetForeground 循環 = 開窗後整整 1 秒的
    // caption/佇列抖動(瘋狂閃爍的下半場)。拿不到前景就停手:編輯器仍在
    // 最上層看得見,使用者點一下即可,不值得用閃爍換。
    std::thread([wnd] {
        for (int i = 0; i < 3; ++i) {
            if (!IsWindow(wnd)) return;  // 窗已關:dance 立即停,不留死 hwnd 重試
            if (GetForegroundWindow() == wnd) break;
            const HWND fg = GetForegroundWindow();
            const DWORD fg_tid = fg != nullptr ? GetWindowThreadProcessId(fg, nullptr) : 0;
            const DWORD my_tid = GetCurrentThreadId();
            if (fg_tid != 0 && fg_tid != my_tid) AttachThreadInput(my_tid, fg_tid, TRUE);
            const BOOL ok = SetForegroundWindow(wnd);
            if (fg_tid != 0 && fg_tid != my_tid) AttachThreadInput(my_tid, fg_tid, FALSE);
            if (ok) break;
            Sleep(80);  // 被拒(對方 thread 忙/沒 pump)→ 等一輪再試,共 ~160ms
        }
    }).detach();
}

// IPlugFrame:plugin 要求 resize editor → 調 host 視窗外框(client 高 + strip)。
// host 持有一個,同時最多一個 view 在附著
class EditorPlugFrame final : public IPlugFrame {
public:
    HWND hwnd{};        // host top-level(視窗銷毀時清 null,擋遲到的 resizeView)
    int extra_cy = kCaptionH + kStripH;

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

// 帶2 控制項的矩形(client 座標;帶高 36,控制項上下留 5~6px)
RECT power_rect(int /*cw*/) noexcept { return {8, kTabH + 5, 36, kStripH - 5}; }
RECT save_btn_rect(int /*cw*/) noexcept { return {44, kTabH + 6, 148, kStripH - 6}; }
RECT load_btn_rect(int /*cw*/) noexcept { return {156, kTabH + 6, 260, kStripH - 6}; }
RECT preset_name_rect(int cw) noexcept { return {270, kTabH + 4, cw - 10, kStripH - 4}; }

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

COLORREF lerp_rgb(COLORREF a, COLORREF b, int num, int den) noexcept {
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * num / den,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * num / den,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * num / den);
}

// 帶列霜面底紋：垂直漸層（帶1 頂亮 → 帶2 底深）+ 固定種子細噪點（霜的
// 顆粒感；固定種子 = 每次重繪同一張圖，不會因重建而 shimmer）。以寬度快取，
// 重繪只剩一次 BitBlt。寬度變了才重建（resize 中 ~64 行 FillRect + 數千
// SetPixel，一次性 <1ms）。
// 霜面底紋本體：垂直漸層（kFrostTop 經 kStripBg 折點下探 kFrostBottom）+
// 固定種子細噪點（霜的顆粒感；固定種子 = 每次重繪同一張圖，不會因重建而
// shimmer）。以寬度快取，重繪只剩一次 BitBlt。mid_y = kStripBg 折點；
// top_highlight = 面板頂緣 1px 高光（只有視窗最頂 = 標題列要）。
void paint_frost(HDC mem, int w, int h, int mid_y, bool top_highlight) noexcept {
    for (int y = 0; y < h; ++y) {
        const COLORREF c = y < mid_y ? lerp_rgb(kFrostTop, kStripBg, y, mid_y)
                                     : lerp_rgb(kStripBg, kFrostBottom, y - mid_y,
                                                h - mid_y);
        const RECT row{0, y, w, y + 1};
        const HBRUSH b = CreateSolidBrush(c);
        FillRect(mem, &row, b);
        DeleteObject(b);
    }
    if (top_highlight) {
        const RECT hl{0, 0, w, 1};
        const HBRUSH hi = CreateSolidBrush(lerp_rgb(kFrostTop, kTextActive, 1, 8));
        FillRect(mem, &hl, hi);
        DeleteObject(hi);
    }
    const RECT lo{0, h - 1, w, h};
    const HBRUSH shade = CreateSolidBrush(kFrostBottom);
    FillRect(mem, &lo, shade);
    DeleteObject(shade);
    // 霜噪：LCG 固定種子，±3 灑在 2x2 格
    unsigned seed = 0x1234abcdu;
    const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return (seed >> 16) & 0xffu; };
    for (int y = 1; y < h - 1; y += 2) {
        for (int x = 0; x < w; x += 2) {
            const int d = static_cast<int>(next() % 7u) - 3;
            if (d == 0) continue;
            const COLORREF c = GetPixel(mem, x, y);
            const int r = GetRValue(c) + d, g = GetGValue(c) + d, bl = GetBValue(c) + d;
            SetPixel(mem, x, y, RGB(static_cast<COLORREF>(r < 0 ? 0 : r > 255 ? 255 : r),
                                    static_cast<COLORREF>(g < 0 ? 0 : g > 255 ? 255 : g),
                                    static_cast<COLORREF>(bl < 0 ? 0 : bl > 255 ? 255 : bl)));
        }
    }
}

HBITMAP frost_cached(int w, int h, int mid_y, bool top_highlight,
                     HBITMAP& bmp, int& cached_w, int& cached_h) noexcept {
    if (bmp != nullptr && w == cached_w && h == cached_h) return bmp;
    if (bmp != nullptr) DeleteObject(bmp);
    cached_w = w;
    cached_h = h;
    const HDC screen = GetDC(nullptr);
    bmp = CreateCompatibleBitmap(screen, w, h);
    const HDC mem = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    const HGDIOBJ old = SelectObject(mem, bmp);
    paint_frost(mem, w, h, mid_y, top_highlight);
    SelectObject(mem, old);
    DeleteDC(mem);
    return bmp;
}

HBITMAP frost_strip_bg(int w) noexcept {
    static HBITMAP bmp = nullptr;
    static int cached_w = -1, cached_h = -1;
    return frost_cached(w, kStripH, kTabH, false, bmp, cached_w, cached_h);
}

HBITMAP frost_caption_bg(int w) noexcept {
    static HBITMAP bmp = nullptr;
    static int cached_w = -1, cached_h = -1;
    return frost_cached(w, kCaptionH, kCaptionH / 2, true, bmp, cached_w, cached_h);
}

HBRUSH tab_active_brush() noexcept {
    static const HBRUSH b = CreateSolidBrush(kTabActiveBg);
    return b;
}

HBRUSH accent_brush() noexcept {
    static const HBRUSH b = CreateSolidBrush(kAccent);
    return b;
}

void ensure_gdiplus() noexcept {
    static const ULONG_PTR gdip_token = [] {
        Gdiplus::GdiplusStartupInput in;
        ULONG_PTR t = 0;
        Gdiplus::GdiplusStartup(&t, &in, nullptr);
        return t;
    }();
    (void)gdip_token;
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
    // 帶列 hover 狀態(tabs 視窗座標;WM_MOUSEMOVE 維護、WM_MOUSELEAVE 清除)
    int hover_tab = -1;     // 可點(未活)tab index;-1 無
    int hover_btn = 0;      // 1 電源、2 儲存、3 載入;0 無
    bool hover_tracked = false;
    bool cap_hover = false;      // 關閉鈕 hover(自繪 caption)
    bool cap_hover_tracked = false;
    bool active = false;         // 視窗是否前景(標題文字亮度)
    std::wstring title;          // 自繪 caption 文字快取
    int last_view_w = -1, last_view_h = -1;  // 上次餵給 view 的 client 尺寸(擋回聲)

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
        MoveWindow(tabs, 0, kCaptionH, cw, ch - kCaptionH, FALSE);  // 不先擦:WM_PAINT 單趟自畫,resize 不閃
        MoveWindow(client, 0, kCaptionH + kStripH, cw, ch - kCaptionH - kStripH,
                   TRUE);  // CLIPCHILDREN:擦不到 plugin 區
        if (const auto* slot = find_slot(active_id); slot != nullptr && slot->plugin->editor_open()) {
            const int vh = ch - kCaptionH - kStripH;
            // 只在 client 尺寸真的變了才餵 onSize:plugin 收 onSize 回 resizeView
            // → host WM_SIZE → 又 onSize 的回聲迴圈(閃爍/暴衝)從這裡斷掉
            if (cw != last_view_w || vh != last_view_h) {
                last_view_w = cw;
                last_view_h = vh;
                slot->plugin->editor_resize_view(cw, vh);
            }
            center_editor_child();
        }
    }

    void resize_to_client(int w, int h) noexcept {
        RECT rc{0, 0, w, h + kCaptionH + kStripH};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(wnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void set_title(std::uint32_t id) {
        std::wstring title = L"RoudaMix";
        if (engine != nullptr) {
            for (const auto& t : engine->plugin_tabs()) {
                if (t.instance_id != id) continue;
                // 標題帶 plugin 名 + 軌道名(專業 host 慣例:一看知是哪顆插件)
                title += L" - " + to_wide(t.label) + L" - " + to_wide(t.track_name);
                break;
            }
        }
        this->title = title;  // 自繪 caption 文字快取
        SetWindowTextW(wnd, title.c_str());
        // 標題也畫在自繪 caption 上：重繪 caption 帶
        RECT rc{};
        GetClientRect(wnd, &rc);
        const RECT cap{0, 0, rc.right, kCaptionH};
        InvalidateRect(wnd, &cap, FALSE);
    }

    // 沒窗就建(含子視窗)。回傳「本次新建」(呼叫端在 attach 失敗時要拆)
    bool ensure_window() {
        if (wnd != nullptr) return false;
        // class 只註冊一次(RegisterClassExW 對重複註冊回 0,非錯誤訊號)。
        // 不帶 CS_HREDRAW/CS_VREDRAW:client 全被 tabs+client 子視窗蓋滿,這兩旗標
        // 只會在每次 resize 強制整窗失效+擦底 = 閃爍來源;子視窗自己管理重繪。
        static const ATOM host_atom =
            register_class(kHostClassName, &host_wnd_proc, dark_brush(), 0);
        if (host_atom == 0) return false;
        static bool children_registered = [] {
            return register_class(kTabsClassName, &tabs_wnd_proc, dark_brush(), 0) != 0 &&
                   register_class(kClientClassName, &client_wnd_proc, dark_brush(), 0) != 0;
        }();
        if (!children_registered) return false;
        if (frame == nullptr) frame = owned(new EditorPlugFrame());
        // WS_EX_TOOLWINDOW:不進工作列/Alt-Tab(Studio Pro 浮動視窗感);
        // WS_EX_TOPMOST:浮在主程式之上(取代舊 owner 綁定的置頂效果 ——
        // 跨進程 owner 會接合兩邊 input queue,已移除,見檔頭註解)。
        // 去掉 WS_MAXIMIZEBOX:自繪 caption 不做最大化(NCCALCSIZE 移框後
        // 最大化要另處理工作區內縮,編輯器浮窗用不到)。
        // WS_CLIPCHILDREN:parent 擦底/重繪剪掉子視窗範圍 = 不閃子視窗內容
        // (anti-flicker 標配)。**不帶 WS_VISIBLE**:建好→定尺寸→attach→
        // activate 一次顯示;開窗途中每步都是一次可見的組合變化 = 開窗閃爍。
        wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kHostClassName,
                              L"RoudaMix",
                              (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 480, 360, nullptr,
                              nullptr, GetModuleHandleW(nullptr), this);
        if (wnd == nullptr) return false;
        // spawn 的 STARTUPINFO 帶 SW_HIDE 時,本視窗「第一次」ShowWindow 的參數
        // 會被替換成 SW_HIDE(視窗永遠 hidden)。先祭一枚 SW_HIDE 消耗掉替換額度,
        // 之後 activate 的 SW_SHOW 才是真的。(create 不帶 WS_VISIBLE 才有此坑)
        ShowWindow(wnd, SW_HIDE);
        // 深色視窗：caption 已自繪，這裡只留 immersive dark + 邊框色
        // （Win11 DWM 會沿自繪視窗外圍畫 1px 邊框）。舊系統自動略過。
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(wnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const COLORREF brc = kBtnBorder;
        DwmSetWindowAttribute(wnd, DWMWA_BORDER_COLOR, &brc, sizeof(brc));
        frame->hwnd = wnd;
        frame->extra_cy = kCaptionH + kStripH;
        tabs = CreateWindowExW(0, kTabsClassName, L"", WS_CHILD | WS_VISIBLE,
                               0, kCaptionH, 480, kStripH, wnd, nullptr,
                               GetModuleHandleW(nullptr), this);
        // WS_CLIPCHILDREN:client 擦深色底時剪掉 plugin 子視窗範圍,resize 不閃 plugin
        client = CreateWindowExW(0, kClientClassName, L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                 0, kCaptionH + kStripH, 480, 300, wnd, nullptr,
                                 GetModuleHandleW(nullptr), this);
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
                (void)eng->set_param(id, param_id, value);
            });
        int w = 0, h = 0;
        if (!plugin->attach_editor(client, frame.get(), w, h)) {
            err = plugin->last_error();
            if (fresh) destroy_window();
            render_tabs();
            return false;
        }
        active_id = id;
        last_view_w = last_view_h = -1;  // 新 view:強制首次 layout 餵 onSize
        if (std::find(opened_ids.begin(), opened_ids.end(), id) == opened_ids.end())
            opened_ids.push_back(id);
        resize_to_client(w, h);
        layout_children();  // 同尺寸重開不觸發 WM_SIZE → 補一次置中/onSize
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

void EditorHost::set_owner(HWND /*owner*/) noexcept {
    // 相容保留:set_editor_owner 命令仍回 ok,但**不再把 UI 主視窗掛成 owner**。
    // 跨進程 ownership 會接合兩 process 的 input queue(焦點/前景共用),
    // 彈出視窗關閉時的 close 手勢可能誤落在主視窗 = 主視窗連帶被關。
    // 主視窗 HWND 從此不跨進邊界。(舊版:GWL_HWNDPARENT)
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
    if (impl_->active_id != 0) impl_->set_title(impl_->active_id);
}

void EditorHost::shutdown() noexcept {
    impl_->destroy_window();
}

namespace {

// ---- host top-level ----

// 自繪 caption 的關閉鈕：24x?;方塊內 × 與帶列控制同語彙（hover 淡底亮字）
RECT close_btn_rect(int cw) noexcept { return {cw - kCaptionH, 0, cw, kCaptionH}; }

void draw_close_glyph(HDC dc, int cw, bool hover, bool pressed) noexcept {
    const RECT r = close_btn_rect(cw);
    if (hover) {
        const HBRUSH bg = CreateSolidBrush(pressed ? kFrostBottom : kTabActiveBg);
        FillRect(dc, &r, bg);
        DeleteObject(bg);
    }
    ensure_gdiplus();
    const Gdiplus::Color color = (hover || pressed)
                                     ? Gdiplus::Color(255, 0xd8, 0xdc, 0xe2)
                                     : Gdiplus::Color(255, 0x8a, 0x91, 0x9b);
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(color, 1.6f);
    const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
    constexpr float half = 5.0f;
    g.DrawLine(&pen, cx - half, cy - half, cx + half, cy + half);
    g.DrawLine(&pen, cx - half, cy + half, cx + half, cy - half);
}

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
    case WM_NCCALCSIZE:
        // 自繪 caption：wp=TRUE 時回 0 = 整窗皆工作區，DWM 標題列（含紅色
        // 關閉鈕的 activate 淡入/重繪動畫 = 開窗閃爍）從此不存在。
        // 不支援最大化（無 WS_MAXIMIZEBOX），不需處理最大化內縮。
        if (wp) return 0;
        break;
    case WM_NCHITTEST: {
        // 邊框已移除，拖曳/縮放全自訂：邊緣 6px 縮放熱區；caption 帶回
        // HTCAPTION（可拖曳；無最大化 = 雙擊無作用），關閉鈕回 HTCLIENT
        // 收滑鼠事件。
        auto* slf = reinterpret_cast<EditorHost::Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (slf != nullptr && slf->wnd != nullptr) {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT wr{};
            GetWindowRect(h, &wr);
            const int x = pt.x - wr.left, y = pt.y - wr.top;
            const int w = wr.right - wr.left, hh = wr.bottom - wr.top;
            constexpr int edge = 6;
            const bool L = x < edge, R = x >= w - edge, T = y < edge, B = y >= hh - edge;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            if (y < kCaptionH) {
                RECT rc{}, cr{};
                GetClientRect(h, &rc);
                cr = close_btn_rect(rc.right);
                if (PtInRect(&cr, pt)) return HTCLIENT;
                return HTCAPTION;
            }
        }
        break;
    }
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
    case WM_PAINT: {
        // 自繪 caption：霜面底紋 + 標題 + 關閉鈕（單趟繪製，無 DWM 動畫）
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        const int cw = rc.right - rc.left;
        const HDC mem = CreateCompatibleDC(dc);
        const HGDIOBJ old_bmp = SelectObject(mem, frost_caption_bg(cw));
        BitBlt(dc, 0, 0, cw, kCaptionH, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old_bmp);
        DeleteDC(mem);
        // caption/帶列 分隔線（同帶1/帶2 分隔線語彙）
        RECT div{0, kCaptionH - 1, cw, kCaptionH};
        FillRect(dc, &div, tab_active_brush());
        // 標題：前景亮、失焦暗（WM_ACTIVATE 重繪一次，無動畫）
        if (self != nullptr && !self->title.empty()) {
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, strip_font());
            RECT tr{10, 0, cw - kCaptionH - 8, kCaptionH - 1};
            SetTextColor(dc, self->active ? kTextActive : kTextIdle);
            DrawTextW(dc, self->title.c_str(), -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (self != nullptr)
            draw_close_glyph(dc, cw, self->cap_hover, self->cap_hover);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        if (self != nullptr) {
            self->active = LOWORD(wp) != WA_INACTIVE;
            RECT rc{};
            GetClientRect(h, &rc);
            const RECT cap{0, 0, rc.right, kCaptionH};
            InvalidateRect(h, &cap, FALSE);
        }
        break;
    case WM_MOUSEMOVE: {
        // 只需要追關閉鈕（caption 其餘區域 = HTCAPTION，收不到 client 滑鼠）
        if (self == nullptr) break;
        if (!self->cap_hover_tracked) {
            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            self->cap_hover_tracked = true;
        }
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT rc{}, cr{};
        GetClientRect(h, &rc);
        cr = close_btn_rect(rc.right);
        const bool over = PtInRect(&cr, pt) != FALSE;
        if (over != self->cap_hover) {
            self->cap_hover = over;
            RECT rc2{};
            GetClientRect(h, &rc2);
            const RECT cap{0, 0, rc2.right, kCaptionH};
            InvalidateRect(h, &cap, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self != nullptr && self->cap_hover) {
            self->cap_hover = false;
            RECT rc{};
            GetClientRect(h, &rc);
            const RECT cap{0, 0, rc.right, kCaptionH};
            InvalidateRect(h, &cap, FALSE);
        }
        self->cap_hover_tracked = false;
        return 0;
    case WM_LBUTTONDOWN: {
        // 關閉鈕：按下即關（自繪 chrome 慣例；caption 其餘 = 拖曳區不走這）
        if (self != nullptr) {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc{}, cr{};
            GetClientRect(h, &rc);
            cr = close_btn_rect(rc.right);
            if (pt.y < kCaptionH && PtInRect(&cr, pt)) {
                PostMessageW(h, WM_CLOSE, 0, 0);
                return 0;
            }
        }
        break;
    }
    case WM_SIZE:
        if (self != nullptr && wp != SIZE_MINIMIZED) self->layout_children();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        // 寬下限 = 帶2 全控能完整顯示(不是 editor 需求 —— editor 小就置中留深色底)
        RECT rc{0, 0, kHostMinClientW, kClientMinH + kCaptionH + kStripH};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }
    case WM_ERASEBKGND:
        // client 被 caption(自繪)+ tabs+client 蓋滿:不擦底。擦了只會在 resize
        // 新露出的帶上閃一塊實色,子視窗隨後蓋回 = 閃爍。
        return 1;
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
    // 電源鍵(比照 UI 端使用者提供 SVG):開 = 綠 #16A34A、bypass = 暗灰
    // (原畫黑 = 深色底上隱形,改 kTextIdle 暗但可辨識)。
    // GDI 筆無反鋸齒 + 折線頂點取整數 = 線條抖;改 GDI+(AA + 浮點座標)。
    // 圓(頂部 90° 開口)+ 豎線從頂穿到圓心;筆寬 = SVG 40/236 比例。
    ensure_gdiplus();
    const Gdiplus::Color color = bypassed ? Gdiplus::Color(255, 0x8a, 0x91, 0x9b)
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

HPEN accent_pen() noexcept {
    static const HPEN p = CreatePen(PS_SOLID, 1, kAccent);
    return p;
}

// 平面按鈕:無填色(= 與帶同底)+ 1px 邊框 + 暗文字;hover 邊框/文字轉亮 ——
// 同主程式 button:hover 只換 border-color 的靜音語彙,不再是厚實實體按鈕
void draw_preset_button(HDC dc, RECT r, const wchar_t* text, bool hover) {
    const HGDIOBJ old_pen = SelectObject(dc, hover ? accent_pen() : btn_border_pen());
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    SetTextColor(dc, hover ? kTextActive : kTextIdle);
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
    case WM_ERASEBKGND:
        // 底紋由 WM_PAINT 單趟 BitBlt 蓋滿：跳過 class brush 擦底（擦了只會
        // 在擦底→填底之間閃一塊）
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        const int cw = rc.right - rc.left;
        // 霜面底紋：快取好的漸層+噪點一 blit 蓋滿，其上再畫文字/控制項
        const HDC mem = CreateCompatibleDC(dc);
        const HGDIOBJ old_bmp = SelectObject(mem, frost_strip_bg(cw));
        BitBlt(dc, 0, 0, cw, kStripH, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old_bmp);
        DeleteDC(mem);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, strip_font());
        const auto& tabs = self->tabs_data;
        int active_idx = -1;
        if (!tabs.empty()) {
            const int tw = (std::min)(160, cw / static_cast<int>(tabs.size()));
            for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
                const auto& t = tabs[static_cast<size_t>(i)];
                if (t.id == self->active_id) active_idx = i;
                // 文字即分頁:無框無底(方框 = 表單感);active/hover 亮文字,
                // active 另有 accent 底線(在分隔線之後蓋上 = 連續直線,
                // 同主程式 tabs 的 2px border-bottom 語彙)
                RECT tr{i * tw, 0, (i + 1) * tw, kTabH};
                RECT text = tr;
                text.left += 8;
                text.right -= 8;
                SetTextColor(dc, t.dim ? kTextDim
                                       : (active_idx == i || self->hover_tab == i)
                                             ? kTextActive
                                             : kTextIdle);
                DrawTextW(dc, t.name.c_str(), -1, &text,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        // 帶1/帶2 分隔線
        RECT div{0, kTabH - 1, cw, kTabH};
        FillRect(dc, &div, tab_active_brush());
        if (active_idx >= 0) {
            const int tw = (std::min)(160, cw / static_cast<int>(tabs.size()));
            RECT line{active_idx * tw, kTabH - 2, (active_idx + 1) * tw, kTabH};
            FillRect(dc, &line, accent_brush());
        }
        if (self->active_id != 0) {
            bool bypassed = false;
            if (const auto* slot = self->find_slot(self->active_id); slot != nullptr)
                bypassed = slot->bypass;
            const RECT pr = power_rect(cw);
            if (self->hover_btn == 1) FillRect(dc, &pr, tab_active_brush());  // hover 淡底
            draw_power_icon(dc, (pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2,
                            (pr.bottom - pr.top) / 2.0 - 4.0, bypassed);
            if (save_btn_rect(cw).right < cw)
                draw_preset_button(dc, save_btn_rect(cw), L"儲存 Preset", self->hover_btn == 2);
            if (load_btn_rect(cw).right < cw)
                draw_preset_button(dc, load_btn_rect(cw), L"載入 Preset", self->hover_btn == 3);
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
    case WM_MOUSEMOVE: {
        if (self == nullptr) break;
        if (!self->hover_tracked) {
            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            self->hover_tracked = true;
        }
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        RECT rc{};
        GetClientRect(h, &rc);
        const int cw = rc.right - rc.left;
        const auto& tabs = self->tabs_data;
        int ntab = -1;
        int nbtn = 0;
        if (y < kTabH) {
            if (!tabs.empty()) {
                const int tw = (std::min)(160, cw / static_cast<int>(tabs.size()));
                const int idx = x / tw;
                if (idx >= 0 && idx < static_cast<int>(tabs.size())) {
                    const auto& t = tabs[static_cast<size_t>(idx)];
                    if (!t.dim && t.id != self->active_id) ntab = idx;  // 可點才回饋
                }
            }
        } else if (self->active_id != 0) {
            const POINT pt{x, y};
            const RECT pr = power_rect(cw);
            const RECT sr = save_btn_rect(cw);
            const RECT lr = load_btn_rect(cw);
            if (PtInRect(&pr, pt)) nbtn = 1;
            else if (sr.right < cw && PtInRect(&sr, pt)) nbtn = 2;
            else if (lr.right < cw && PtInRect(&lr, pt)) nbtn = 3;
        }
        if (ntab != self->hover_tab || nbtn != self->hover_btn) {
            self->hover_tab = ntab;
            self->hover_btn = nbtn;
            InvalidateRect(h, nullptr, FALSE);
        }
        // 可點區 = 手型游標(原生視窗的專業工具手感)。
        // IDC_* 在未定義 UNICODE 時是 LPSTR,沿 register_class 既有轉型手法
        SetCursor(LoadCursorW(nullptr, (ntab >= 0 || nbtn != 0)
                                            ? reinterpret_cast<LPCWSTR>(IDC_HAND)
                                            : reinterpret_cast<LPCWSTR>(IDC_ARROW)));
        return 0;
    }
    case WM_MOUSELEAVE:
        self->hover_tracked = false;
        self->hover_tab = -1;
        self->hover_btn = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
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
