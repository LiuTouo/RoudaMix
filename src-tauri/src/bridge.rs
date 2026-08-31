//! pipe client + 連線狀態機 + command correlation + event 轉發。
//! UI 可拋棄:engine 是權威、detached、pipe 斷線照跑;這層只負責接回。
//! 契約:contracts/protocol.md(斷線即失敗、不透明重發;重連靠 snapshot 事件重對齊)。
//!
//! I/O 用 tokio NamedPipeClient(overlapped):同步 File + try_clone 的雙執行緒讀寫
//! 在 Windows named pipe 上會死鎖 — 同一 file object 一次只容一個同步 I/O,
//! reader 卡在 ReadFile 時 writer 的 WriteFile 永遠排隊(M1 實測重現)。

use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, Manager};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::windows::named_pipe::{ClientOptions, NamedPipeClient};
use tokio::sync::{oneshot, Mutex as AsyncMutex};
type WriteHalf = tokio::io::WriteHalf<NamedPipeClient>;

use crate::protocol::{make_command, parse_frame, Frame, MAX_FRAME_BYTES, PIPE_NAME};
use crate::spawn;

const REPLY_TIMEOUT: Duration = Duration::from_secs(30);
const CONNECT_RETRY: Duration = Duration::from_millis(500);

#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SharedState {
    pub connected: bool,
    pub epoch: u64,
    pub engine_version: String,
    /// P1-L 連線細分狀態:connecting | spawning | connected | spawn_failed | disconnected
    /// (connected 與 bool 重複保留 = UI 現有判斷不破壞;phase 給精確顯示)
    pub phase: String,
    /// phase 詳情(spawn 失敗原因等;空 = 無)
    pub detail: String,
}

impl SharedState {
    fn new() -> Self {
        Self {
            connected: false,
            epoch: 0,
            engine_version: String::new(),
            phase: "connecting".into(),
            detail: String::new(),
        }
    }
}

/// pending reply correlation(G 測試標的):register → resolve(reply id 對上)或
/// fail_all(斷線)。逾時後晚到的 reply 在 send 端已 remove → resolve 回 false 丟棄,
/// 不會誤寫新連線狀態。
struct PendingMap {
    map: Mutex<HashMap<u64, oneshot::Sender<Value>>>,
}

impl PendingMap {
    fn new() -> Self {
        Self { map: Mutex::new(HashMap::new()) }
    }
    fn register(&self, id: u64, tx: oneshot::Sender<Value>) {
        self.map.lock().unwrap().insert(id, tx);
    }
    fn take(&self, id: u64) -> Option<oneshot::Sender<Value>> {
        self.map.lock().unwrap().remove(&id)
    }
    fn fail_all(&self) {
        for (_, tx) in self.map.lock().unwrap().drain() {
            let _ = tx.send(json!({
                "id": 0, "ok": false, "epoch": 0,
                "error": { "code": "disconnected", "message": "engine pipe closed" }
            }));
        }
    }
}

struct Inner {
    state: Mutex<SharedState>,
    pending: PendingMap,
    write: AsyncMutex<Option<WriteHalf>>,
    next_id: AtomicU64,
}

#[derive(Clone)]
pub struct Bridge {
    inner: Arc<Inner>,
}

impl Bridge {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Inner {
                state: Mutex::new(SharedState::new()),
                pending: PendingMap::new(),
                write: AsyncMutex::new(None),
                next_id: AtomicU64::new(0),
            }),
        }
    }

    pub fn shared(&self) -> SharedState {
        self.inner.state.lock().unwrap().clone()
    }

    fn set_phase(&self, app: &AppHandle, phase: &str, detail: &str) {
        {
            let mut s = self.inner.state.lock().unwrap();
            s.phase = phase.into();
            s.detail = detail.into();
        }
        let _ = app.emit("connection", self.shared());
    }

    /// UI 手動 retry(P1-L):強制重 spawn engine(冪等 —— engine 已在跑 =
    /// singleton mutex 擋下,spawn 出的執行個體立即退出,現有連線不動)。
    pub fn respawn(&self, app: AppHandle) {
        let b = self.clone();
        tauri::async_runtime::spawn(async move {
            b.set_phase(&app, "spawning", "");
            match spawn::spawn_supervised() {
                Ok(()) => b.set_phase(&app, "connecting", ""),
                Err(se) => b.set_phase(&app, "spawn_failed", &format!("{se}")),
            }
        });
    }

    /// 啟動連線 task(背景永遠在接)。
    pub fn start(&self, app: AppHandle) {
        let b = self.clone();
        tauri::async_runtime::spawn(async move { run(app, b).await });
    }

    /// 送 command 等 reply。斷線/逾時即失敗,不重發(契約 §7)。
    pub async fn send(&self, kind: &str, payload: Value) -> Result<Value, String> {
        let id = self.inner.next_id.fetch_add(1, Ordering::Relaxed) + 1;
        let (tx, rx) = oneshot::channel();
        self.inner.pending.register(id, tx);

        let frame = make_command(id, kind, payload);
        let buf = match serde_json::to_vec(&frame) {
            Ok(b) => b,
            Err(e) => {
                self.inner.pending.take(id);
                return Err(format!("serialize: {e}"));
            }
        };
        if buf.len() > MAX_FRAME_BYTES {
            self.inner.pending.take(id);
            return Err("frame too large".into());
        }

        let w = {
            let mut guard = self.inner.write.lock().await;
            match guard.as_mut() {
                Some(h) => {
                    let mut framed = (buf.len() as u32).to_le_bytes().to_vec();
                    framed.extend_from_slice(&buf);
                    if h.write_all(&framed).await.is_err() {
                        None // 寫失敗:當斷線
                    } else {
                        Some(())
                    }
                }
                None => None, // 未連線
            }
        };

        match w {
            Some(()) => {}
            None => {
                self.inner.pending.take(id);
                return Err("not connected".into());
            }
        }

        match tokio::time::timeout(REPLY_TIMEOUT, rx).await {
            Ok(Ok(rep)) => {
                if rep["ok"].as_bool().unwrap_or(false) {
                    Ok(rep["result"].clone())
                } else {
                    let code = rep["error"]["code"].as_str().unwrap_or("internal");
                    let msg = rep["error"]["message"].as_str().unwrap_or("");
                    Err(format!("{code}: {msg}"))
                }
            }
            Ok(Err(_)) => Err("disconnected".into()),
            Err(_) => {
                self.inner.pending.take(id);  // 晚到 reply 由此丟棄(resolve 不到)
                Err("timeout".into())
            }
        }
    }

    fn on_snapshot(&self, app: &AppHandle, payload: &Value) {
        {
            let mut s = self.inner.state.lock().unwrap();
            s.connected = true;
            s.phase = "connected".into();
            s.detail.clear();
            if let Some(e) = payload["epoch"].as_u64() {
                s.epoch = e;
            }
            if let Some(v) = payload["engineVersion"].as_str() {
                s.engine_version = v.to_string();
            }
        }
        let _ = app.emit("connection", self.shared());
        let _ = app.emit("engine-snapshot", payload);
    }

    async fn set_disconnected(&self, app: &AppHandle) {
        {
            let mut s = self.inner.state.lock().unwrap();
            s.connected = false;
            s.phase = "disconnected".into();
        }
        *self.inner.write.lock().await = None;
        self.inner.pending.fail_all();
        let _ = app.emit("connection", self.shared());
    }
}

/// 連線主迴圈:接不上就 spawn engine 再試(engine mutex 防 double-spawn)。
/// P1-L:phase 隨進度推進(connecting → spawning → connected / spawn_failed),
/// UI 據此顯示細分狀態與 retry。
async fn run(app: AppHandle, b: Bridge) {
    let mut tries: u32 = 0;
    loop {
        b.set_phase(&app, "connecting", "");
        match ClientOptions::new().open(PIPE_NAME) {
            Ok(pipe) => {
                tries = 0;
                serve(&app, &b, pipe).await;
                b.set_disconnected(&app).await;
            }
            Err(e) => {
                tries += 1;
                eprintln!("[bridge] connect fail (try {tries}): {e}");
                if tries == 1 || tries % 20 == 0 {
                    b.set_phase(&app, "spawning", "");
                    match spawn::spawn_supervised() {
                        Ok(()) => {
                            eprintln!("[bridge] spawned engine");
                            b.set_phase(&app, "connecting", "");
                        }
                        Err(se) => {
                            eprintln!("[bridge] spawn engine failed: {se}");
                            b.set_phase(&app, "spawn_failed", &format!("{se}"));
                        }
                    }
                }
                tokio::time::sleep(CONNECT_RETRY).await;
            }
        }
    }
}

/// 讀 loop:reply 分發 pending、event 轉發 Tauri。EOF/錯誤即返回(上層重連)。
async fn serve(app: &AppHandle, b: &Bridge, pipe: NamedPipeClient) {
    let (mut read, write) = tokio::io::split(pipe);
    *b.inner.write.lock().await = Some(write);

    // 主視窗 HWND → engine:editor host 掛成 owned 浮動視窗(無工作列項、
    // 隨主程式最小化)。fire-and-forget:engine 會回 ok,pending 沒登記即丟。
    // UI 重啟 → engine 被 job object 帶走重 spawn → 重連時自然重送
    let owner = app
        .get_webview_window("main")
        .and_then(|w| w.hwnd().ok())
        .map(|h| h.0 as u64);
    if let Some(hwnd) = owner {
        let id = b.inner.next_id.fetch_add(1, Ordering::Relaxed) + 1;
        let frame = make_command(id, "set_editor_owner", json!({ "hwnd": hwnd }));
        if let Ok(buf) = serde_json::to_vec(&frame) {
            let mut framed = (buf.len() as u32).to_le_bytes().to_vec();
            framed.extend_from_slice(&buf);
            let mut w = b.inner.write.lock().await;
            if let Some(h) = w.as_mut() {
                let _ = h.write_all(&framed).await;
            }
        }
    }

    let mut len = [0u8; 4];
    let mut buf = Vec::new();
    loop {
        if read.read_exact(&mut len).await.is_err() {
            return;
        }
        let n = u32::from_le_bytes(len) as usize;
        if n == 0 || n > MAX_FRAME_BYTES {
            return;
        }
        buf.resize(n, 0);
        if read.read_exact(&mut buf).await.is_err() {
            return;
        }
        let j: Value = match serde_json::from_slice(&buf) {
            Ok(v) => v,
            Err(_) => continue, // 壞 JSON 容錯:丟 frame 不斷線(契約 §9)
        };
        match parse_frame(j.clone()) {
            Ok(Frame::Reply(_)) => {
                let id = j["id"].as_u64().unwrap_or(0);
                if let Some(tx) = b.inner.pending.take(id) {
                    let _ = tx.send(j);
                }
            }
            Ok(Frame::Event(e)) => {
                if e.kind == "snapshot" {
                    b.on_snapshot(app, &e.payload);
                }
                let _ = app.emit("engine-event", json!({ "kind": e.kind, "payload": e.payload }));
            }
            _ => continue, // client 不收 command;壞 frame 容錯
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// P1-G:命令在飛時斷線 → fail_all 把所有 pending 用 disconnected reply 收掉
    /// (呼叫端收到明確錯誤,不是乾等 timeout)。
    #[test]
    fn disconnect_during_command_fails_pending() {
        let p = PendingMap::new();
        let (tx1, mut rx1) = oneshot::channel();
        let (tx2, mut rx2) = oneshot::channel();
        p.register(1, tx1);
        p.register(2, tx2);
        p.fail_all();
        for rx in [&mut rx1, &mut rx2] {
            let rep = rx.try_recv().expect("pending resolved by fail_all");
            assert_eq!(rep["error"]["code"], json!("disconnected"));
            assert_eq!(rep["ok"], json!(false));
        }
        // drain 過了:之後的 late reply 找不到人
        assert!(p.take(1).is_none());
    }

    /// P1-G:逾時後晚到的 reply —— send 已把 pending 移除,late resolve 對不上
    /// 任何註冊(take = None)→ 丟棄,不會寫進新連線的任何狀態。
    #[test]
    fn late_reply_after_timeout_is_dropped() {
        let p = PendingMap::new();
        let (tx, _rx) = oneshot::channel(); // _rx 保 sender 活著(模擬在飛命令)
        p.register(7, tx);
        let removed = p.take(7); // send 逾時路徑:移除登記
        assert!(removed.is_some());
        drop(removed); // sender 釋放
        // 晚到的 reply(id=7)再來:take = None = 丟棄
        assert!(p.take(7).is_none());
        // 新命令註冊新 id,不受舊 reply 影響
        let (tx2, rx2) = oneshot::channel();
        p.register(8, tx2);
        assert!(p.take(8).is_some());
        drop(rx2);
    }

    /// P1-G:正常路徑 —— reply id 對上註冊,ok reply 原樣送達。
    #[test]
    fn reply_correlates_by_id() {
        let p = PendingMap::new();
        let (tx, mut rx) = oneshot::channel();
        p.register(42, tx);
        let sender = p.take(42).expect("registered id resolves");
        sender
            .send(json!({"id": 42, "ok": true, "epoch": 3, "result": {"x": 1}}))
            .unwrap();
        let rep = rx.try_recv().expect("reply delivered");
        assert_eq!(rep["result"]["x"], json!(1));
        assert_eq!(rep["epoch"], json!(3));
    }
}
