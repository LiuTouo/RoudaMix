// Studio Pro 式 editor host 實作。單一 top-level 視窗,自繪 caption(無 DWM
// 標題列)+ 兩個子視窗:
//   caption(視窗自己畫)= 頂部 32px:視窗標題 + 關閉鈕,平面細框與主程式一致
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
#include <utility>

namespace rmx {

namespace {

using namespace Steinberg;

constexpr int kTabH = 28;       // 帶1:plugin tabs(每列高)
constexpr int kTabPadX = 8;     // 帶1:變寬分頁左右 padding(名稱完整不截斷)
constexpr int kCaptionH = 32;   // 自繪標題列(取代 DWM caption,同高)
constexpr int kBand2H = 36;     // 帶2:bypass 電源鈕 + preset 控制
constexpr int kClientMinW = 80;
constexpr int kClientMinH = 60;
constexpr int kHostMinClientW = 282;  // 帶2 全控(電源+兩鈕,load 右緣 268 + 邊距)
constexpr UINT kEditorResizeMessage = WM_APP + 10;

// 只換算宿主 chrome；不更動 process/thread DPI awareness 或插件座標。
int ui_scale_percent = 100;
int chrome_px(int value) noexcept { return MulDiv(value, ui_scale_percent, 100); }

constexpr wchar_t kHostClassName[] = L"RmxVST3Editor";   // 沿用:m3b probe 以 class 數窗
constexpr wchar_t kTabsClassName[] = L"RmxEditorTabs";
constexpr wchar_t kClientClassName[] = L"RmxEditorClient";

// (跨進程 owner 已移除,不再需要 GWL_HWNDPARENT)

// 深色主題:對齊主程式 ui/src/console.css(Approved C 極簡控制台)色票。
// 全平面:視窗/caption/帶列/client 同一塊 --flat-0,分隔只靠 1px --border
// 髮絲線。舊版自繪霜面(漸層+噪點,當年為取代會閃爍的 OS blur 而做)已隨
// 主程式改版一併移除;純 FillRect 單趟繪製,同樣結構上不可能閃。
constexpr COLORREF kStripBg = RGB(0x0d, 0x0f, 0x10);     // = --flat-0
constexpr COLORREF kTabActiveBg = RGB(0x20, 0x24, 0x28); // = --flat-4:淡底(hover/active 分頁)
constexpr COLORREF kTabHoverBg = RGB(0x18, 0x1b, 0x1e);  // = --flat-2:分頁 hover
constexpr COLORREF kAccent = RGB(0x6e, 0xa8, 0xfe);      // = --accent
constexpr COLORREF kTextActive = RGB(0xe2, 0xe5, 0xe8);  // = --text
constexpr COLORREF kTextIdle = RGB(0x9c, 0xa5, 0xae);    // = --text-dim
constexpr COLORREF kTextDim = RGB(0x56, 0x5b, 0x64);
constexpr COLORREF kOk = RGB(0x55, 0xc2, 0xa3);          // = --ok(電源 on,同機架圓點)
constexpr COLORREF kWarn = RGB(0xd9, 0xa4, 0x41);        // = --warn(bypass,同機架 off 圓點)
constexpr COLORREF kBtnBorder = RGB(0x29, 0x2d, 0x31);   // = --border(細框/髮絲線)

HFONT scaled_font(HFONT& font, int base_height, const wchar_t* face) {
    const int height = -chrome_px(base_height);
    LOGFONTW current{};
    if (font != nullptr && GetObjectW(font, sizeof(current), &current) != 0 &&
        current.lfHeight == height) return font;
    const HFONT next = CreateFontW(height, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   0, 0, CLEARTYPE_QUALITY, 0, face);
    if (next != nullptr) {
        if (font != nullptr) DeleteObject(font);
        font = next;
    }
    return font != nullptr ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

HFONT strip_font() {
    static HFONT f = nullptr;
    return scaled_font(f, 12, L"Segoe UI");
}

// 主程式 label 同級 11px(帶2 按鈕文字)
HFONT label_font() {
    static HFONT f = nullptr;
    return scaled_font(f, 11, L"Segoe UI");
}

// 主程式 meta 資訊同款等寬字(--mono: Cascadia Mono/Consolas 系)
HFONT mono_font() {
    static HFONT f = nullptr;
    return scaled_font(f, 11, L"Consolas");
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
        ViewRect clamped{0, 0, w, h};
        return static_cast<tresult>(SendMessageW(hwnd, kEditorResizeMessage,
            reinterpret_cast<WPARAM>(view), reinterpret_cast<LPARAM>(&clamped)));
    }

private:
    std::atomic<uint32> references_{1};
};

// 帶2 控制項的矩形(ty = 帶2 頂 = 帶1 列數 × kTabH;列數隨分頁換行變動)
RECT power_rect(int ty) noexcept { return {chrome_px(8), ty + chrome_px(5), chrome_px(36), ty + chrome_px(kBand2H - 5)}; }
RECT save_btn_rect(int ty) noexcept { return {chrome_px(44), ty + chrome_px(6), chrome_px(148), ty + chrome_px(kBand2H - 6)}; }
RECT load_btn_rect(int ty) noexcept { return {chrome_px(156), ty + chrome_px(6), chrome_px(260), ty + chrome_px(kBand2H - 6)}; }
RECT preset_name_rect(int ty, int cw) noexcept { return {chrome_px(270), ty + chrome_px(4), cw - chrome_px(10), ty + chrome_px(kBand2H - 4)}; }

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

// caption/帶列 分隔的 1px 髮絲線(同主程式 --border)
HBRUSH hairline_brush() noexcept {
    static const HBRUSH b = CreateSolidBrush(kBtnBorder);
    return b;
}

HBRUSH tab_hover_brush() noexcept {
    static const HBRUSH b = CreateSolidBrush(kTabHoverBg);
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
        int x = 0, y = 0, w = 0;  // 換行版面 strip 座標(y = 列頂;layout_tabs 填)
    };
    std::vector<Tab> tabs_data;
    bool tabs_measured = false;  // 分頁文字寬已量(名單變更時重置)
    int rows = 1;                // 帶1 換行列數(寬度不足時延伸下一列)

    int strip_h() const noexcept { return chrome_px(kTabH) * rows + chrome_px(kBand2H); }
    int band2_y() const noexcept { return chrome_px(kTabH) * rows; }
    IPtr<EditorPlugFrame> frame;
    // 帶列 hover 狀態(tabs 視窗座標;WM_MOUSEMOVE 維護、WM_MOUSELEAVE 清除)
    int hover_tab = -1;     // 可點(未活)tab index;-1 無
    int hover_btn = 0;      // 1 電源、2 儲存、3 載入;0 無
    bool hover_tracked = false;
    bool cap_hover = false;      // 關閉鈕 hover(自繪 caption)
    bool cap_hover_tracked = false;
    bool active = false;         // 視窗是否前景(標題文字亮度)
    std::wstring title;          // 自繪 caption 文字快取
    int last_view_w = -1, last_view_h = -1;  // 插件的原生尺寸，與 chrome 最小寬度分開。
    int last_client_w = -1, last_client_h = -1; // Windows 可能在 ShowWindow 重送同尺寸 WM_SIZE。
    int fit_w = 0, fit_h = 0;    // active 插件 attach 回報的原生尺寸(固定版面 = 視窗貼合尺寸)
    bool resizing_host = false; // 宿主 chrome/插件請求改外框，不向插件回送容器尺寸。

    ~Impl() {
        if (wnd != nullptr) DestroyWindow(wnd);  // WM_DESTROY 內 detach + 清欄位
    }

    const RackSlot* find_slot(std::uint32_t id) const noexcept {
        return engine != nullptr ? engine->find_slot(id) : nullptr;
    }

    // 以 tracks 現況重建 tab 快取 + 重繪(main thread 直接讀,無需鎖)。
    // 名稱前加全域連續流水號(1.、2.…):分頁列順序 = tracks × chain,chain 內
    // 即聲音實際進入順序;與主畫面機架列表編號同源同號。
    void render_tabs() {
        tabs_data.clear();
        tabs_measured = false;  // 名單變了,待重新量測
        if (engine == nullptr) return;
        int n = 0;
        for (const auto& t : engine->plugin_tabs()) {
            tabs_data.push_back({t.instance_id,
                                 std::to_wstring(++n) + L". " + to_wide(t.label),
                                 !t.editor_capable, t.bypass});
        }
        if (tabs != nullptr) InvalidateRect(tabs, nullptr, FALSE);
    }

    // 量測變寬分頁文字寬(+ padding);名單不變只量一次
    void ensure_measured() {
        if (tabs_measured || tabs_data.empty() || tabs == nullptr) return;
        const HDC dc = GetDC(tabs);
        if (dc == nullptr) return;
        const HGDIOBJ old = SelectObject(dc, strip_font());
        for (auto& t : tabs_data) {
            SIZE sz{};
            GetTextExtentPoint32W(dc, t.name.c_str(), static_cast<int>(t.name.size()), &sz);
            t.w = sz.cx + chrome_px(kTabPadX) * 2;
        }
        SelectObject(dc, old);
        ReleaseDC(tabs, dc);
        tabs_measured = true;
    }

    // 換行版面:寬度不足時延伸下一列(名稱完整不截斷、不需捲動);
    // 列首分頁再寬也獨佔一列(只能靠加寬視窗)。回傳列數。
    int layout_tabs(int cw) {
        ensure_measured();
        int x = 0, y = 0;
        rows = 1;
        for (auto& t : tabs_data) {
            if (x > 0 && x + t.w > cw) {
                x = 0;
                y += chrome_px(kTabH);
                ++rows;
            }
            t.x = x;
            t.y = y;
            x += t.w;
        }
        return rows;
    }

    // plugin attach 後自建的 child HWND 實際尺寸 = 原生編輯器的真實大小。
    // getSize() 回報的可能與它不同(JUCE 系會自行 DPI 縮放/約束,甚至 attach
    // 中就 resizeView 一個比 getSize 大的尺寸)。找不到 child(非同步建 UI)
    // → false,呼叫端沿用 getSize 值
    bool editor_child_size(int& w, int& h) const noexcept {
        if (client == nullptr) return false;
        HWND child = FindWindowExW(client, nullptr, nullptr, nullptr);
        if (child == nullptr) return false;
        RECT wr{};
        GetWindowRect(child, &wr);
        w = wr.right - wr.left;
        h = wr.bottom - wr.top;
        return w > 0 && h > 0;
    }

    // 原生 editor 比 client 小(plugin 不縮放,如 kHs Gain)→ 置中,周圍留深色底。
    // offset 夾在 ≥ 0:child 比 client 大時(JUCE 系無視 onSize 自行定尺寸),
    // 負 offset = 編輯器四邊被 client 邊界裁切/藏進帶列(實測 Denoiser Classic
    // 586×344 回報 vs 700×408 實際);此時保持 0,由 layout_children 的
    // fit-to-child 把 client 撑到 child 實際尺寸
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
        const int x = (std::max)(0, static_cast<int>(cr.right - cr.left - w) / 2);
        const int y = (std::max)(0, static_cast<int>(cr.bottom - cr.top - h) / 2);
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
        layout_tabs(cw);  // 視窗寬變了 → 列數/帶高跟著變
        const int sh = strip_h();
        // tabs 只佔帶高:若讓它延伸到窗底,會蓋住 client(第一個建立的
        // 子視窗 = z 最上) —— plugin 區所有滑鼠輸入被 tabs 吃掉,編輯器開了卻
        // 點不動。不重疊 = hit-test 直達 client,不受 sibling z-order 影響。
        const int caption_h = chrome_px(kCaptionH);
        const int vh = ch - caption_h - sh;
        if (vh < kClientMinH) {
            resize_to_client(cw, kClientMinH);
            return;
        }
        const bool client_changed = cw != last_client_w || vh != last_client_h;
        last_client_w = cw;
        last_client_h = vh;
        MoveWindow(tabs, 0, caption_h, cw, sh, FALSE);  // 不先擦:WM_PAINT 單趟自畫,resize 不閃
        MoveWindow(client, 0, caption_h + sh, cw, vh, TRUE);
        if (const auto* slot = find_slot(active_id); slot != nullptr && slot->plugin->editor_open()) {
            if (resizing_host) { center_editor_child(); return; }
            if (slot->plugin->editor_can_resize()) {
                // 只在 client 尺寸真的變了才餵 onSize:plugin 收 onSize 回 resizeView
                // → host WM_SIZE → 又 onSize 的回聲迴圈(閃爍/暴衝)從這裡斷掉
                if (client_changed) {
                    last_view_w = cw;
                    last_view_h = vh;
                    slot->plugin->editor_resize_view(cw, vh);
                }
            } else {
                // 固定版面插件(canResize=false):onSize 只會把插件視窗背景拉開、
                // 控制項留在原位 = 右側死空間。不餵,直接把窗貼回原生尺寸。
                // 原生尺寸以 **plugin 實際 child 為準**(取 max):只信 getSize
                // 會把 client 壓得比 plugin 實際視窗小 = 原生編輯器四邊被裁切
                // (實測 Denoiser Classic:getSize 586×344,child 實際 700×408,
                // 開窗即裁)。client ⊇ child 後尺寸已符不再動 → 收斂無迴圈
                int tw = fit_w, th = fit_h;
                int cw2 = 0, ch2 = 0;
                if (editor_child_size(cw2, ch2)) {
                    tw = (std::max)(tw, cw2);
                    th = (std::max)(th, ch2);
                }
                tw = (std::max)(tw, chrome_px(kHostMinClientW));
                if (cw != tw || vh != th) resize_to_client(tw, th);
            }
            center_editor_child();
        }
    }

    void resize_to_client(int w, int h) noexcept {
        w = (std::max)(w, chrome_px(kHostMinClientW));
        layout_tabs(w);  // 以目標寬度定列數(strip 高隨之)
        const bool previous = std::exchange(resizing_host, true);
        // WM_NCCALCSIZE 已讓整窗成為 client，不能再加 Windows 原生 caption 邊距。
        SetWindowPos(wnd, nullptr, 0, 0, w, h + chrome_px(kCaptionH) + strip_h(),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        layout_children(); // 尺寸相同也要重新配置子視窗。
        resizing_host = previous;
    }

    // 列數變化(插件增刪/重新編號/視窗寬變)→ 以目前 client 尺寸重設外框高,
    // plugin editor 高度不被帶列加高吃掉;WM_SIZE → layout_children 收尾
    void sync_strip_height() {
        if (wnd == nullptr || client == nullptr) return;
        RECT hc{}, cc{};
        GetClientRect(wnd, &hc);
        GetClientRect(client, &cc);
        const int old_strip = strip_h();
        layout_tabs(hc.right);
        if (strip_h() != old_strip) {
            resize_to_client(hc.right, cc.bottom);
        }
        if (tabs != nullptr) InvalidateRect(tabs, nullptr, FALSE);
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
        const RECT cap{0, 0, rc.right, chrome_px(kCaptionH)};
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
        // 去掉 WS_SYSMENU:NCCALCSIZE 回 0 自繪 caption 後,DWM 對帶此樣式的
        // 視窗**仍會**在右上角畫自己的最小化/關閉鈕,且 WM_SIZE 不重繪 caption
        // 帶時舊位置的按鈕像素殘留累積 = 截圖中標題列一整排幽靈關閉鈕;從
        // 樣式移除 = DWM 根本不畫。自繪關閉鈕/拖曳/邊緣縮放(THICKFRAME)不受影響。
        // WS_CLIPCHILDREN:parent 擦底/重繪剪掉子視窗範圍 = 不閃子視窗內容
        // (anti-flicker 標配)。**不帶 WS_VISIBLE**:建好→定尺寸→attach→
        // activate 一次顯示;開窗途中每步都是一次可見的組合變化 = 開窗閃爍。
        wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kHostClassName,
                              L"RoudaMix",
                              (WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_CLIPCHILDREN,
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
        tabs = CreateWindowExW(0, kTabsClassName, L"", WS_CHILD | WS_VISIBLE,
                               0, chrome_px(kCaptionH), 480, strip_h(), wnd, nullptr,
                               GetModuleHandleW(nullptr), this);
        // WS_CLIPCHILDREN:client 擦深色底時剪掉 plugin 子視窗範圍,resize 不閃 plugin
        client = CreateWindowExW(0, kClientClassName, L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                 0, chrome_px(kCaptionH) + strip_h(), 480, 300, wnd, nullptr,
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
        fit_w = w;
        fit_h = h;
        // getSize 只是 plugin 自報尺寸;attach 後實際 child 可能更大(JUCE 系
        // 自行縮放/約束)—— 以實際 child 為 fit 基準,否則首次 layout 就把
        // client 壓得比 plugin 小,開窗即四邊裁切(實測 586×344 vs 700×408)
        int cw2 = 0, ch2 = 0;
        if (editor_child_size(cw2, ch2)) {
            fit_w = (std::max)(fit_w, cw2);
            fit_h = (std::max)(fit_h, ch2);
        }
        if (std::find(opened_ids.begin(), opened_ids.end(), id) == opened_ids.end())
            opened_ids.push_back(id);
        last_view_w = fit_w;
        last_view_h = fit_h;
        resize_to_client(fit_w, fit_h);  // chrome 再寬也不拉伸 plugin 原生 editor
        set_title(id);
        ShowWindow(wnd, SW_SHOW);
        foreground_window(wnd);
        render_tabs();
        sync_strip_height();
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

void EditorHost::set_ui_scale(int percent) noexcept {
    percent = std::clamp(percent, 75, 150);
    if (ui_scale_percent == percent) return;
    ui_scale_percent = percent;
    impl_->tabs_measured = false;
    impl_->hover_tab = -1;
    impl_->hover_btn = 0;
    impl_->cap_hover = false;
    if (impl_->wnd == nullptr) return;
    int w = impl_->last_view_w > 0 ? impl_->last_view_w : impl_->fit_w;
    int h = impl_->last_view_h > 0 ? impl_->last_view_h : impl_->fit_h;
    int child_w = 0, child_h = 0;
    if (impl_->editor_child_size(child_w, child_h)) {
        w = (std::max)(w, child_w);
        h = (std::max)(h, child_h);
    }
    impl_->resize_to_client(w, h);
    InvalidateRect(impl_->wnd, nullptr, FALSE);
    InvalidateRect(impl_->tabs, nullptr, FALSE);
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
    impl_->sync_strip_height();
    if (impl_->active_id != 0) impl_->set_title(impl_->active_id);
}

void EditorHost::shutdown() noexcept {
    impl_->destroy_window();
}

namespace {

// ---- host top-level ----

// 自繪 caption 的關閉鈕：24x?;方塊內 × 與帶列控制同語彙（hover 淡底亮字）
RECT close_btn_rect(int cw) noexcept { return {cw - chrome_px(kCaptionH), 0, cw, chrome_px(kCaptionH)}; }

void draw_close_glyph(HDC dc, int cw, bool hover, bool pressed) noexcept {
    const RECT r = close_btn_rect(cw);
    if (hover) {
        const HBRUSH bg = CreateSolidBrush(pressed ? kTabHoverBg : kTabActiveBg);
        FillRect(dc, &r, bg);
        DeleteObject(bg);
    }
    ensure_gdiplus();
    const Gdiplus::Color color = (hover || pressed)
                                     ? Gdiplus::Color(255, 0xd8, 0xdc, 0xe2)
                                     : Gdiplus::Color(255, 0x8a, 0x91, 0x9b);
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float scale = ui_scale_percent / 100.0f;
    Gdiplus::Pen pen(color, 1.6f * scale);
    const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
    const float half = 5.0f * scale;
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
            const int edge = chrome_px(6);
            const bool L = x < edge, R = x >= w - edge, T = y < edge, B = y >= hh - edge;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            if (y < chrome_px(kCaptionH)) {
                RECT rc{}, cr{};
                GetClientRect(h, &rc);
                cr = close_btn_rect(rc.right);
                const POINT cpt{x, y};  // client 座標（pt 是螢幕座標，不能直接比）
                if (PtInRect(&cr, cpt)) return HTCLIENT;
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
        // 自繪 caption：平面底 + 標題 + 關閉鈕（單趟繪製，無 DWM 動畫）
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        const int saved_dc = SaveDC(dc);
        RECT rc{};
        GetClientRect(h, &rc);
        const int cw = rc.right - rc.left;
        const int caption_h = chrome_px(kCaptionH);
        // 平面底:整條 caption 填 --flat-0(單趟 FillRect,無漸層無快取)
        const RECT capfill{0, 0, cw, caption_h};
        FillRect(dc, &capfill, dark_brush());
        // caption/帶列 分隔線（同帶1/帶2 分隔線語彙）
        RECT div{0, caption_h - 1, cw, caption_h};
        FillRect(dc, &div, hairline_brush());
        // 標題：前景亮、失焦暗（WM_ACTIVATE 重繪一次，無動畫）
        if (self != nullptr && !self->title.empty()) {
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, strip_font());
            RECT tr{chrome_px(10), 0, cw - caption_h - chrome_px(8), caption_h - 1};
            SetTextColor(dc, self->active ? kTextActive : kTextIdle);
            DrawTextW(dc, self->title.c_str(), -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (self != nullptr)
            draw_close_glyph(dc, cw, self->cap_hover, self->cap_hover);
        RestoreDC(dc, saved_dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        if (self != nullptr) {
            self->active = LOWORD(wp) != WA_INACTIVE;
            RECT rc{};
            GetClientRect(h, &rc);
            const RECT cap{0, 0, rc.right, chrome_px(kCaptionH)};
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
            const RECT cap{0, 0, rc2.right, chrome_px(kCaptionH)};
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
            const RECT cap{0, 0, rc.right, chrome_px(kCaptionH)};
            InvalidateRect(h, &cap, FALSE);
        }
        if (self != nullptr) self->cap_hover_tracked = false;
        return 0;
    case WM_LBUTTONDOWN: {
        // 關閉鈕：按下即關（自繪 chrome 慣例；caption 其餘 = 拖曳區不走這）
        if (self != nullptr) {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc{}, cr{};
            GetClientRect(h, &rc);
            cr = close_btn_rect(rc.right);
            if (pt.y < chrome_px(kCaptionH) && PtInRect(&cr, pt)) {
                PostMessageW(h, WM_CLOSE, 0, 0);
                return 0;
            }
        }
        break;
    }
    case kEditorResizeMessage: {
        if (self == nullptr || wp == 0 || lp == 0) return kInvalidArgument;
        auto* view = reinterpret_cast<IPlugView*>(wp);
        auto* size = reinterpret_cast<ViewRect*>(lp);
        self->fit_w = self->last_view_w = size->right - size->left;
        self->fit_h = self->last_view_h = size->bottom - size->top;
        const bool previous = std::exchange(self->resizing_host, true);
        self->resize_to_client(self->fit_w, self->fit_h);
        view->onSize(size);
        self->center_editor_child();
        self->resizing_host = previous;
        return kResultOk;
    }
    case WM_SIZE:
        if (self != nullptr && wp != SIZE_MINIMIZED) {
            self->layout_children();
            // resize 後重繪整條 caption:帶上任何殘留像素(如歷史遺跡)立即被
            // WM_PAINT 的單趟 BitBlt 蓋掉,不隨尺寸變動累積
            RECT rc{};
            GetClientRect(h, &rc);
            const RECT cap{0, 0, rc.right, chrome_px(kCaptionH)};
            InvalidateRect(h, &cap, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        // 寬下限 = 帶2 全控能完整顯示(不是 editor 需求 —— editor 小就置中留深色底)
        // 高下限用單列帶高:列數增加時 sync_strip_height 會自動調高外框。
        mmi->ptMinTrackSize.x = chrome_px(kHostMinClientW);
        mmi->ptMinTrackSize.y = kClientMinH + chrome_px(kCaptionH) +
                               chrome_px(kTabH) + chrome_px(kBand2H);
        // 固定版面插件(canResize=false):上限 = 原生尺寸 + 現行帶高 → 使用者
        // 拉不動,不出現死空間/被拉開的插件背景(可縮放插件不受限,照舊跟隨)。
        // 上限不低於下限(max<min 時拖曳行為未定義)。建立途中 self 尚為 null → 略過
        if (self != nullptr && self->wnd != nullptr) {
            if (const auto* slot = self->find_slot(self->active_id);
                slot != nullptr && slot->plugin->editor_open() &&
                !slot->plugin->editor_can_resize()) {
                const int width = (std::max)(self->fit_w, chrome_px(kHostMinClientW));
                self->layout_tabs(width);
                mmi->ptMaxTrackSize.x = width;
                mmi->ptMaxTrackSize.y = (std::max<LONG>)(mmi->ptMinTrackSize.y,
                    self->fit_h + chrome_px(kCaptionH) + self->strip_h());
            }
        }
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
    // switch 內 break 的路徑(NCCALCSIZE wp=FALSE、NCHITTEST 非熱區、非關閉鈕的
    // LBUTTONDOWN 等)必須落到 DefWindowProc:少了這行 = 落到函式末尾無回傳值,
    // MSVC C4715,回傳 eax 垃圾(NCHITTEST 回 0x96 這種非 HT 碼的來源)。
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- 頂部列:帶1 tabs + 帶2 bypass/preset(全部自繪 + hit-test,單一 HWND)----

void draw_power_icon(HDC dc, int cx, int cy, double rad, bool bypassed) {
    // 電源鍵:同主程式機架圓點語彙 —— on = --ok #55C2A3、bypass = --warn
    // #D9A441(機架 off 圓點同色)。
    // GDI 筆無反鋸齒 + 折線頂點取整數 = 線條抖;改 GDI+(AA + 浮點座標)。
    // 圓(頂部 90° 開口)+ 豎線從頂穿到圓心;筆寬 = SVG 40/236 比例。
    ensure_gdiplus();
    // 色值照抄上方 kOk/kWarn(Gdiplus::Color 吃 BYTE,經 Get?Value 取 constexpr
    // 會被 MSVC C4310 噪,字面值最平事)
    const Gdiplus::Color on_color(255, 0x55, 0xc2, 0xa3);   // kOk
    const Gdiplus::Color off_color(255, 0xd9, 0xa4, 0x41);  // kWarn
    const Gdiplus::Color color = bypassed ? off_color : on_color;
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

// 平面按鈕:無填色(= 與帶同底)+ 1px --border 細框 + 11px 暗文字;hover =
// #202428 淡底亮字(同主程式 button:hover 語彙,框不變);圓角同 --flat-radius 3px
void draw_preset_button(HDC dc, RECT r, const wchar_t* text, bool hover) {
    if (hover) FillRect(dc, &r, tab_active_brush());
    const HGDIOBJ old_pen = SelectObject(dc, btn_border_pen());
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, chrome_px(3), chrome_px(3));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    SelectObject(dc, label_font());
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
        const int saved_dc = SaveDC(dc);
        RECT rc{};
        GetClientRect(h, &rc);
        const int cw = rc.right - rc.left;
        // 換行版面(列數隨寬度);整條帶列平面填 --flat-0
        self->layout_tabs(cw);
        const int ty = self->band2_y();
        const int sh = self->strip_h();
        const RECT bandfill{0, 0, cw, sh};
        FillRect(dc, &bandfill, dark_brush());
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, strip_font());
        const auto& tabs = self->tabs_data;
        int active_idx = -1;
        // 文字即分頁:無框;active = 主程式 .tabs button.on 語彙(#202428 底 +
        // accent 文字 + 2px 底線),hover = flat-2 淡底亮字,dim = 最暗灰
        // (無編輯器)。名稱完整不截斷:寬度不足時換行延伸下一列,不出現省略號。
        for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
            const auto& t = tabs[static_cast<size_t>(i)];
            if (t.id == self->active_id) active_idx = i;
            RECT tr{t.x, t.y, t.x + t.w, t.y + chrome_px(kTabH)};
            if (active_idx == i) {
                FillRect(dc, &tr, tab_active_brush());
                SetTextColor(dc, kAccent);
            } else if (self->hover_tab == i) {
                FillRect(dc, &tr, tab_hover_brush());
                SetTextColor(dc, kTextActive);
            } else {
                SetTextColor(dc, t.dim ? kTextDim : kTextIdle);
            }
            DrawTextW(dc, t.name.c_str(), -1, &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // 帶1/帶2 分隔線
        RECT div{0, ty - 1, cw, ty};
        FillRect(dc, &div, hairline_brush());
        if (active_idx >= 0) {
            const auto& t = tabs[static_cast<size_t>(active_idx)];
            RECT line{t.x, t.y + chrome_px(kTabH) - chrome_px(2), t.x + t.w, t.y + chrome_px(kTabH)};
            FillRect(dc, &line, accent_brush());
        }
        if (self->active_id != 0) {
            bool bypassed = false;
            if (const auto* slot = self->find_slot(self->active_id); slot != nullptr)
                bypassed = slot->bypass;
            const RECT pr = power_rect(self->band2_y());
            if (self->hover_btn == 1) FillRect(dc, &pr, tab_active_brush());  // hover 淡底
            draw_power_icon(dc, (pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2,
                            (pr.bottom - pr.top) / 2.0 - chrome_px(4), bypassed);
            const int by = self->band2_y();
            if (save_btn_rect(by).right < cw)
                draw_preset_button(dc, save_btn_rect(by), L"儲存 Preset", self->hover_btn == 2);
            if (load_btn_rect(by).right < cw)
                draw_preset_button(dc, load_btn_rect(by), L"載入 Preset", self->hover_btn == 3);
            RECT nr = preset_name_rect(by, cw);
            SelectObject(dc, mono_font());
            SetTextColor(dc, kTextIdle);
            DrawTextW(dc, self->preset_name.c_str(), -1, &nr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        RestoreDC(dc, saved_dc);
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
        if (y < self->band2_y()) {
            if (tabs.empty()) break;
            int hit = -1;
            for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
                const auto& t = tabs[static_cast<size_t>(i)];
                if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + chrome_px(kTabH)) {
                    hit = i;
                    break;
                }
            }
            if (hit < 0) break;
            const auto& t = tabs[static_cast<size_t>(hit)];
            if (t.dim || t.id == self->active_id) break;
            std::string err;
            self->activate(t.id, err);  // 失敗 = dim + 空 client(文件化行為)
        } else if (self->active_id != 0) {
            const POINT pt{x, y};
            const int by = self->band2_y();
            RECT pr = power_rect(by);
            RECT sr = save_btn_rect(by);
            RECT lr = load_btn_rect(by);
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
        if (y < self->band2_y() && !tabs.empty()) {
            for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
                const auto& t = tabs[static_cast<size_t>(i)];
                if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + chrome_px(kTabH)) {
                    if (!t.dim && t.id != self->active_id) ntab = i;  // 可點才回饋
                    break;
                }
            }
        } else if (self->active_id != 0) {
            const POINT pt{x, y};
            const int by = self->band2_y();
            const RECT pr = power_rect(by);
            const RECT sr = save_btn_rect(by);
            const RECT lr = load_btn_rect(by);
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
