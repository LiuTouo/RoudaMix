//! P0 修復(run-1 稽核 `router.fs-commands-no-final-resource-auth`):
//! webview 明確沒有 fs 權限(capabilities 無 fs plugin),但 engine 的
//! save/load_session、save/load_preset 以任意絕對路徑讀寫檔案,等於經
//! bridge 隧道重新授予 webview 檔案系統能力。此模組把「路徑」綁回
//! **原生對話框的使用者手勢**:對話框改由 Rust 端開(同一個已註冊的
//! tauri-plugin-dialog),回傳路徑即記入本 launch 的核准表;engine_command
//! 送出前驗證路徑已核准(讀/寫分級、15 分鐘 TTL)。攻陷的 webview 拿不到
//! 對話框手勢,就帶不出核准路徑。
//! shortcut: start_scan roots 不在此限 —— 掃描自訂根目錄是產品功能,
//! 且 worker 預檢+registry 核准閘門已涵蓋其風險;若日後要收緊再擴充。

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{LazyLock, Mutex};
use std::time::{Duration, Instant};

use serde_json::Value;
use tauri::AppHandle;
use tauri_plugin_dialog::DialogExt;

const APPROVAL_TTL: Duration = Duration::from_secs(15 * 60);
const MAX_PATH_LEN: usize = 1024;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Access {
    Read,
    Write,
}

#[derive(Default)]
struct Guard {
    // key = 對話框回傳的原始路徑字串(engine 收到同一字串,精確比對即可)
    approved: Mutex<HashMap<String, (Instant, Access)>>,
    // 啟動還原用:settings.lastSessionPath(精確)與 sessionDir(前經)
    // —— 這些是使用者先前經對話框選過並持久化的路徑,重啟後没有新對話框手勢,
    // 但仍屬使用者意國。僅授讀(load_session);寫一律要求新對話框核准。
    seeded_path: Mutex<Option<String>>,
    seeded_dir: Mutex<Option<String>>,
}

fn normalize_key(p: &str) -> String {
    // Windows 路徑大小寫不敏感;統一小寫 + 正斜線以容許 UI 端正負斜線混用
    let mut s = p.replace('\\', "/");
    s.truncate(s.len().min(MAX_PATH_LEN * 4)); // 僅防極端;真正長度上限在 approve/authorize 檢
    s.to_lowercase()
}

impl Guard {
    fn approve(&self, path: &str, access: Access) {
        if path.len() > MAX_PATH_LEN || path.is_empty() {
            return;
        }
        let mut m = self.approved.lock().unwrap_or_else(|e| e.into_inner());
        let now = Instant::now();
        m.retain(|_, (t, _)| now.duration_since(*t) < APPROVAL_TTL);
        m.insert(normalize_key(path), (now, access));
    }

    fn check(&self, path: &str, access: Access) -> bool {
        if path.len() > MAX_PATH_LEN || path.is_empty() {
            return false;
        }
        let m = self.approved.lock().unwrap_or_else(|e| e.into_inner());
        match m.get(&normalize_key(path)) {
            // 寫核准隱含讀(存過的檔案本 launch 可再載);讀不跨用為寫
            Some((t, a)) => {
                t.elapsed() < APPROVAL_TTL && (*a == access || (access == Access::Read && *a == Access::Write))
            }
            None => false,
        }
    }

    /// load_session 專用:seed(lastSessionPath 精確 / sessionDir 前綴)命中。
    fn seed_matches(&self, path: &str) -> bool {
        let key = normalize_key(path);
        if let Some(p) = self.seeded_path.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
            if *p == key {
                return true;
            }
        }
        if let Some(d) = self.seeded_dir.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
            let dk = d.trim_end_matches('/');
            if !dk.is_empty() && key.starts_with(&format!("{}/", dk)) {
                return true;
            }
        }
        false
    }
}

// 單一 app 實例(single-instance plugin 保證),全域表可接受(HashMap::new 非 const)
static GUARD: LazyLock<Guard> = LazyLock::new(Guard::default);

fn to_string(p: Option<tauri_plugin_dialog::FilePath>) -> Option<String> {
    let fp = p?;
    let pb: PathBuf = fp.into_path().ok()?;
    Some(pb.to_string_lossy().into_owned())
}

/// 開啟 Session 載入對話框;回傳路徑計為「已核准讀」。
#[tauri::command]
pub fn pick_session_open(app: AppHandle) -> Option<String> {
    let picked = app
        .dialog()
        .file()
        .add_filter("RoudaMix Session", &["rmsession"])
        .set_title("載入 Session")
        .blocking_pick_file();
    let s = to_string(picked)?;
    GUARD.approve(&s, Access::Read);
    Some(s)
}

/// 開啟 Session 儲存對話框;回傳路徑計為「已核准寫」。
/// default_dir/default_file:對話框起始落點(UI 傳目前 session 檔的資料夾/檔名,
/// 退化到設定的 Session 資料夾);僅影響落點,不影響核准語意。
#[tauri::command]
pub fn pick_session_save(
    app: AppHandle,
    default_dir: Option<String>,
    default_file: Option<String>,
) -> Option<String> {
    // 目錄不存在(已被刪)就退回系統預設,避免對話框開不了
    let dir = default_dir.filter(|d| !d.is_empty() && Path::new(d).exists());
    let mut dlg = app
        .dialog()
        .file()
        .add_filter("RoudaMix Session", &["rmsession"])
        .set_title("儲存 Session")
        .set_file_name(default_file.filter(|f| !f.is_empty()).unwrap_or_else(|| "session.rmsession".into()));
    if let Some(d) = dir {
        dlg = dlg.set_directory(d);
    }
    let picked = dlg.blocking_save_file();
    let s = to_string(picked)?;
    GUARD.approve(&s, Access::Write);
    Some(s)
}

/// 開啟 Session 資料夾選擇對話框;回傳資料夾計為「已核准讀」(list_sessions 用)。
#[tauri::command]
pub fn pick_session_folder(app: AppHandle) -> Option<String> {
    let picked = app.dialog().file().set_title("選擇 Session 資料夾").blocking_pick_folder();
    let s = to_string(picked)?;
    GUARD.approve(&s, Access::Read);
    Some(s)
}

/// list_sessions 白名單：本 launch 經對話框核准的資料夾。
pub fn folder_approved(dir: &str) -> bool {
    GUARD.check(dir, Access::Read)
}

/// UI 送 save_session 前檢查既有路徑是否仍具寫核准(對話框命中且未過期)。
/// false = seed 還原路徑或核准已過期,UI 應改開 Save As 對話框而非直接覆寫。
#[tauri::command]
pub fn session_write_approved(path: String) -> bool {
    GUARD.check(&path, Access::Write)
}

/// 啟動時由 lib.rs 播種:持久化的還原路徑/資料夾僅授 load_session 讀取。
/// shortcut: 攻陷 webview 可寫入 settings 再重啟播種讀路徑,殘餘面為
/// 「load_session 讀取並回報 missing 診斷」;寫入路徑不受此影響。
pub fn seed_restore(last_session_path: Option<&str>, session_dir: Option<&str>) {
    *GUARD.seeded_path.lock().unwrap_or_else(|e| e.into_inner()) =
        last_session_path.filter(|p| !p.is_empty() && p.len() <= MAX_PATH_LEN)
            .map(normalize_key);
    *GUARD.seeded_dir.lock().unwrap_or_else(|e| e.into_inner()) =
        session_dir.filter(|p| !p.is_empty() && p.len() <= MAX_PATH_LEN)
            .map(normalize_key);
}

/// engine_command 送出前的路徑核准閘。回 Err(訊息) = 拒絕送出。
/// null path(engine 預設路徑)不受限 —— 不攜帶攻擊者選擇的資源。
pub fn authorize(kind: &str, payload: &Value) -> Result<(), String> {
    let path = match payload.get("path") {
        None | Some(Value::Null) => return Ok(()),
        Some(Value::String(p)) => p.clone(),
        Some(_) => return Ok(()), // 非 string 交給契約驗證器拒絕
    };
    let deny = || Err(format!("path not approved by a file dialog: {path}"));
    match kind {
        "save_session" | "save_preset" => {
            if !GUARD.check(&path, Access::Write) {
                return deny();
            }
            // 命中即刷新 TTL:長編輯階段的重複存檔不因 15min 過期而斷
            GUARD.approve(&path, Access::Write);
            Ok(())
        }
        "load_session" | "load_preset" => {
            // 對話框核准(讀,或寫隱含讀)或啟動 seed 任一命中
            let dialog_hit = GUARD.check(&path, Access::Read);
            let seeded = !dialog_hit && GUARD.seed_matches(&path);
            if !dialog_hit && !seeded {
                return deny();
            }
            // 對話框開檔 = 使用者手勢已立 → 升級授寫回(開檔後編輯直接存回)。
            // seed 命中不升級:避免「寫 settings → 重啟播種 → load → save」
            // 經持久化重開寫入鏈(seed 僅讀,存回走一次 Save As)。
            if dialog_hit {
                GUARD.approve(&path, Access::Write);
            }
            Ok(())
        }
        _ => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn authorize_binds_path_to_dialog_approval() {
        let w = "C:\\Sessions\\a.rmsession";
        GUARD.approve(w, Access::Write);
        // 核准(寫)可寫
        assert!(authorize("save_session", &json!({"path": "C:\\Sessions\\a.rmsession"})).is_ok());
        // 寫核准隱含讀：存過的檔案同 launch 可再載入
        assert!(authorize("load_session", &json!({"path": w})).is_ok());
        // 未核准路徑拒絕
        let other = "C:\\Windows\\win.ini";
        assert!(authorize("save_session", &json!({"path": other})).is_err());
        // null path(engine 預設)放行
        assert!(authorize("save_session", &json!({"path": null})).is_ok());
        // 大小寫/斜線差異視同一路徑(對話框與命令送同一字串,僅防 UI 正規化漂移)
        let r = "C:/SessIONS/b.rmsession";
        GUARD.approve(r, Access::Read);
        let r2 = "c:\\sessions\\B.RMSESSION";
        assert!(authorize("load_session", &json!({"path": r2})).is_ok());
        // 啟動播種:lastSessionPath 精確 + sessionDir 前綴僅授讀
        let last = "C:\\Users\\me\\last.rmsession".to_string();
        seed_restore(Some(&last), Some("C:\\Sessions"));
        let in_dir = "c:/SESSIONS/deep/nested/x.rmSession".to_string();
        assert!(authorize("load_session", &json!({"path": in_dir})).is_ok());
        // 播種讀路徑不可跨用為寫
        assert!(authorize("save_session", &json!({"path": last})).is_err());
        // 前綴外路徑仍拒絕
        let outside = "C:/Other/y.rmsession";
        assert!(authorize("load_session", &json!({"path": outside})).is_err());
        seed_restore(None, None);
        // 對話框開檔(load)後升級寫回；seed 路徑不升級
        let opened = "C:\\Sessions\\opened.rmsession";
        GUARD.approve(opened, Access::Read);
        assert!(authorize("load_session", &json!({"path": opened})).is_ok());
        assert!(authorize("save_session", &json!({"path": opened})).is_ok());
        let seeded_last = "C:\\Users\\me\\seed.rmsession";
        seed_restore(Some(seeded_last), None);
        assert!(authorize("load_session", &json!({"path": seeded_last})).is_ok());
        assert!(authorize("save_session", &json!({"path": seeded_last})).is_err());
        seed_restore(None, None);
        // 其他 kind 不受影響
        assert!(authorize("track_add", &json!({"name": "x"})).is_ok());
        // 超長路徑直接拒絕
        let long = "C:\\".to_string() + &"a".repeat(2000);
        assert!(authorize("load_session", &json!({"path": long})).is_err());
    }
}
