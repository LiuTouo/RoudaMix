//! UI→bridge Tauri transport。前端 typed interface 由 command_contract.json 生成，
//! bridge 在送出前以同一張表驗證 payload，再負責 framing + correlation。
//! engine_command 必須 async:send 等 reply 最長 30s,sync command 會卡住 thread。

use serde_json::Value;
use tauri::State;

use crate::bridge::{Bridge, CommandError, SharedState};

#[tauri::command]
pub fn connect_status(b: State<Bridge>) -> SharedState {
    b.shared()
}

#[tauri::command]
pub async fn engine_command(
    b: State<'_, Bridge>,
    kind: String,
    payload: Value,
) -> Result<Value, CommandError> {
    b.send(&kind, payload).await
}

/// P1-L:UI 手動 retry(spawn_failed 時)。冪等:engine 已在跑 = singleton 擋下。
#[tauri::command]
pub fn respawn_engine(app: tauri::AppHandle, b: State<Bridge>) {
    b.respawn(app);
}

/// 響度面板釘選切換:釘選 = 解除 owner(獨立 topmost 視窗,點擊不再把主視窗拉到
/// 前景 —— owner/owned 同屬一個 activation group,不解除則點面板必拉起 RoudaMix);
/// 解除釘選 = 掛回主視窗 owner(「僅浮在 RoudaMix 上」的預設語意)。僅 Windows。
#[cfg(target_os = "windows")]
#[tauri::command]
pub fn set_loudness_owner(
    app: tauri::AppHandle,
    window: tauri::WebviewWindow,
    owned: bool,
) -> Result<(), String> {
    use tauri::Manager;
    use windows_sys::Win32::UI::WindowsAndMessaging::{GWL_HWNDPARENT, SetWindowLongPtrW};
    let hwnd = window.hwnd().map_err(|e| e.to_string())?.0 as isize;
    let owner = if owned {
        let main = app.get_webview_window("main").ok_or("main window not found")?;
        main.hwnd().map_err(|e| e.to_string())?.0 as isize
    } else {
        0
    };
    unsafe { SetWindowLongPtrW(hwnd as _, GWL_HWNDPARENT, owner) };
    Ok(())
}

/// 更新面板狀態:portable(zip)沒有 NSIS 安裝器,UI 擋自動更新、留瀏覽器下載。
#[derive(serde::Serialize)]
pub struct UpdaterState {
    pub portable: bool,
}

#[tauri::command]
pub fn updater_state() -> UpdaterState {
    UpdaterState {
        portable: crate::portable::root().is_some(),
    }
}
