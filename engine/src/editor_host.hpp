// Studio Pro 式 editor host:單一 top-level 視窗(engine process),頂部自繪
// tab 列(rack 全部 plugin 點擊切換 + bypass 電源鈕 + 載入 preset),下方內嵌
// 當前 plugin 的原生 VST3 editor(IPlugView attached 到 client 子視窗)。
// 視窗所有權在這,Vst3Plugin 只管 view 生命週期(見 vst3_host.hpp)。
// main-thread only(JUCE 系 plugin 假設 host 單一 UI thread,別處會死鎖)。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace rmx {

class AudioEngine;

// host → Router 的內部指令:POST 到 main window 排隊，由 Router 序列化
// 後走 engine 正規路徑(host 的 wnd_proc 可能在 dispatch 持鎖中被同步重入,
// host 自身絕不鎖 —— 非遞迴 mutex 會自死鎖)
constexpr int kHostBypass = 1;       // instance_id 切換 bypass
constexpr int kHostPreset = 2;       // instance_id 載入 path(.vstpreset)
constexpr int kHostSavePreset = 3;   // instance_id 存到 path(.vstpreset)
struct EditorHostCmd {
    int kind{};
    std::uint32_t instance_id{};
    std::wstring path;
};

// LPARAM = EditorHostCmd*(handler 處理後 delete);WM_APP+1/2 已被 Task/QUIT 用掉
constexpr UINT WM_APP_HOSTCMD = WM_APP + 3;

class EditorHost {
public:
    static EditorHost& instance() noexcept;
    ~EditorHost();
    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;

    void set_engine(AudioEngine* engine) noexcept;   // main() 接線一次
    void set_command_target(HWND post_to) noexcept;  // EditorHostCmd 投遞目標
    // 僅縮放自繪 caption/tabs/控制項(75–150%)，保留插件 view 的原生尺寸。
    void set_ui_scale(int percent) noexcept;
    // 測試專用:視窗照常建立與幾何驗證,但不顯示、不搶前景 —— ctest 跑批
    // (離線發布腳本)時編輯器視窗不再閃現/搶使用者焦點。僅 editor_host_scale_test 使用。
    void hide_windows_for_tests() noexcept;
    // 舊版曾掛 UI 主視窗當跨進程 owner —— 會接合兩 process 的 input queue,
    // 關彈出視窗時 close 手勢可能誤落主視窗。已改為獨立視窗(見 editor_host.cpp
    // 檔頭);此命令保留接受、HWND 只用於前景判定與預設位置,不當 owner。
    void set_owner(HWND owner) noexcept;
    // 以給定前景視窗重算動態置頂(前景 hook 與測試共用的單一入口)
    void update_topmost(HWND fg) noexcept;
    // UI 主視窗 LOCATIONCHANGE(拖曳/縮放/最小化;hwnd=主視窗、idObject=
    // OBJID_WINDOW 才有效)→ 編輯器同量平移跟隨,保留相對位置
    // (hook 與測試共用的單一入口)
    void ui_moved(HWND hwnd, LONG id_object) noexcept;
    // open_editor 命令:開/切換到該 plugin(已顯示 = 帶前景);err 帶原因
    bool open(std::uint32_t instance_id, std::string& err);
    // close_editor 命令:active = detach + 自動切下一個開著的,沒有則關窗;
    // 非 active = 只從清單移除。冪等
    void close(std::uint32_t instance_id) noexcept;
    // tracks 異動後(main.cpp after_mutation):同步 tab 列;active 被移除時自動切換/關窗
    void notify_tracks_changed();
    // message loop 結束、engine 拆解前呼叫(detach view、摧毀視窗)
    void shutdown() noexcept;

    // public:wnd_proc(editor_host.cpp 匿名 namespace)以 GWLP_USERDATA 存取
    struct Impl;

private:
    EditorHost() = default;
    Impl* impl_ = nullptr;
};

}  // namespace rmx
