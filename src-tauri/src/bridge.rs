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
use tauri::{AppHandle, Emitter};
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
}

struct Inner {
    state: Mutex<SharedState>,
    pending: Mutex<HashMap<u64, oneshot::Sender<Value>>>,
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
                state: Mutex::new(SharedState {
                    connected: false,
                    epoch: 0,
                    engine_version: String::new(),
                }),
                pending: Mutex::new(HashMap::new()),
                write: AsyncMutex::new(None),
                next_id: AtomicU64::new(0),
            }),
        }
    }

    pub fn shared(&self) -> SharedState {
        self.inner.state.lock().unwrap().clone()
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
        self.inner.pending.lock().unwrap().insert(id, tx);

        let frame = make_command(id, kind, payload);
        let buf = match serde_json::to_vec(&frame) {
            Ok(b) => b,
            Err(e) => {
                self.inner.pending.lock().unwrap().remove(&id);
                return Err(format!("serialize: {e}"));
            }
        };
        if buf.len() > MAX_FRAME_BYTES {
            self.inner.pending.lock().unwrap().remove(&id);
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
                self.inner.pending.lock().unwrap().remove(&id);
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
                self.inner.pending.lock().unwrap().remove(&id);
                Err("timeout".into())
            }
        }
    }

    fn fail_pending_all(&self) {
        let mut p = self.inner.pending.lock().unwrap();
        for (_, tx) in p.drain() {
            let _ = tx.send(json!({
                "id": 0, "ok": false, "epoch": 0,
                "error": { "code": "disconnected", "message": "engine pipe closed" }
            }));
        }
    }

    fn on_snapshot(&self, app: &AppHandle, payload: &Value) {
        {
            let mut s = self.inner.state.lock().unwrap();
            s.connected = true;
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
        self.inner.state.lock().unwrap().connected = false;
        *self.inner.write.lock().await = None;
        self.fail_pending_all();
        let _ = app.emit("connection", self.shared());
    }
}

/// 連線主迴圈:接不上就 spawn engine 再試(engine mutex 防 double-spawn)。
async fn run(app: AppHandle, b: Bridge) {
    let mut tries: u32 = 0;
    loop {
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
                    match spawn::spawn_supervised() {
                        Ok(()) => eprintln!("[bridge] spawned engine"),
                        Err(se) => eprintln!("[bridge] spawn engine failed: {se}"),
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
                if let Some(tx) = b.inner.pending.lock().unwrap().remove(&id) {
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
