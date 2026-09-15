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
