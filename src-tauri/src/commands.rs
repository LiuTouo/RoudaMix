//! UI→bridge Tauri commands。一條泛用 pass-through(kind+payload),typed 包裝在前端 ipc.ts。
//! 契約 §6 的 16 種 kind 都走這裡;bridge 只做 framing + correlation。
//! engine_command 必須 async:send 等 reply 最長 30s,sync command 會卡住 thread。

use serde_json::Value;
use tauri::State;

use crate::bridge::{Bridge, SharedState};

#[tauri::command]
pub fn connect_status(b: State<Bridge>) -> SharedState {
    b.shared()
}

#[tauri::command]
pub async fn engine_command(
    b: State<'_, Bridge>,
    kind: String,
    payload: Value,
) -> Result<Value, String> {
    b.send(&kind, payload).await
}

/// P1-L:UI 手動 retry(spawn_failed 時)。冪等:engine 已在跑 = singleton 擋下。
#[tauri::command]
pub fn respawn_engine(app: tauri::AppHandle, b: State<Bridge>) {
    b.respawn(app);
}
