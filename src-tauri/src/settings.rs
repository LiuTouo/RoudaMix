//! 應用層設定(bridge 自己的偏好,engine 不涉入)— JSON 存 app data dir。
//! P1-E:typed schema + 版本 migration + 原子寫入。
//! - 讀檔必經 `normalize`:known field 型別錯/值非法 → 回預設 + warning(UI 可呈現);
//!   未知鍵保留(向前相容,寫回時合併)。
//! - 寫檔 = 同目錄 temp → sync_all → 舊檔搬 `.bak` → rename 替換
//!   (與 engine session.cpp 同款契約:任何時刻中斷,正式檔要嘛舊版要嘛新版)。
//! - `last_working_device`/`last_working_buffer`(P1-D):只在 start 成功後由 UI 寫入,
//!   engine 裝置啟動偏好用。

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use tauri::{AppHandle, Manager};

pub const SCHEMA_VERSION: u32 = 3;

/// blank | last | folder(不接受任意字串;非法值 normalize 回 blank + warning)
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum StartupMode {
    Blank,
    Last,
    Folder,
}

/// 使用者按下主視窗關閉按鈕時的動作。None 代表尚未選擇，首次關閉時由 UI 詢問。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum CloseBehavior {
    Tray,
    Exit,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase", default)]
pub struct Settings {
    pub schema_version: u32,
    pub startup_mode: StartupMode,
    pub session_dir: Option<String>,
    /// folder 模式:session_dir 內選定的檔名(空 = 空白 session)
    pub startup_file: Option<String>,
    /// last 模式:上次存/載的路徑
    pub last_session_path: Option<String>,
    /// P1-D:最後「成功啟動」的裝置/Buffer —— 只有成功才可寫入(失敗選擇不成偏好)
    pub last_working_device: Option<String>,
    pub last_working_buffer: Option<u32>,
    pub close_behavior: Option<CloseBehavior>,
    /// 僅由 Windows 登入自動啟動時，讓主視窗保持隱藏並常駐系統匣。
    pub start_minimized_on_autostart: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            schema_version: SCHEMA_VERSION,
            startup_mode: StartupMode::Blank,
            session_dir: None,
            startup_file: None,
            last_session_path: None,
            last_working_device: None,
            last_working_buffer: None,
            close_behavior: None,
            start_minimized_on_autostart: false,
        }
    }
}

fn path(app: &AppHandle) -> Option<PathBuf> {
    app.path().app_data_dir().ok().map(|d| d.join("settings.json"))
}

/// 版本 migration 階梯:vN → vN+1。目前為 v3；未來欄位搬移
/// 在這裡加階段,`normalize` 拿到的永遠是當版形狀的原始 JSON。
fn migrate(mut raw: Value) -> Value {
    let cur = raw
        .get("schemaVersion")
        .and_then(Value::as_u64)
        .unwrap_or(0);
    if cur < SCHEMA_VERSION as u64 && raw.is_object() {
        if let Some(obj) = raw.as_object_mut() {
            // v0/v1 沒有關閉行為；保留未選狀態，讓 UI 在首次關閉時詢問。
            if cur < 2 {
                obj.entry("closeBehavior").or_insert(Value::Null);
            }
            // v0～v2 沒有自動啟動時縮小偏好；預設仍顯示主視窗。
            if cur < 3 {
                obj.entry("startMinimizedOnAutostart")
                    .or_insert(Value::Bool(false));
            }
            obj.insert("schemaVersion".into(), Value::from(SCHEMA_VERSION));
        }
    }
    raw
}

/// 讀到的原始 JSON → typed Settings。known field 型別錯/值非法 = 回預設 + warning;
/// 未知鍵不擋(寫回時由 merge 保留)。
pub fn normalize(raw: Value) -> (Settings, Vec<String>) {
    let mut s = Settings::default();
    let mut warnings = Vec::new();
    let Value::Object(obj) = raw else {
        return (s, vec!["settings 檔不是 JSON 物件,已回復全部預設".into()]);
    };
    for (k, v) in &obj {
        match k.as_str() {
            "schemaVersion" => {
                if v.as_u64() != Some(SCHEMA_VERSION as u64) {
                    if let Some(n) = v.as_u64() {
                        if n > SCHEMA_VERSION as u64 {
                            warnings.push(format!(
                                "settings 檔版本(v{n})比本程式(v{SCHEMA_VERSION})新,已知欄位照讀"
                            ));
                        }
                    } else {
                        warnings.push("settings.schemaVersion 型別錯誤,已回復預設".into());
                    }
                }
            }
            "startupMode" => match v.as_str() {
                Some("blank") => s.startup_mode = StartupMode::Blank,
                Some("last") => s.startup_mode = StartupMode::Last,
                Some("folder") => s.startup_mode = StartupMode::Folder,
                Some(_) | None => warnings.push(format!(
                    "settings.startupMode 值不合法({v}),已回復預設 blank"
                )),
            },
            "sessionDir" => match opt_str(v) {
                Ok(v) => s.session_dir = v,
                Err(()) => warnings.push("settings.sessionDir 型別錯誤,已回復預設".into()),
            },
            "startupFile" => match opt_str(v) {
                Ok(v) => s.startup_file = v,
                Err(()) => warnings.push("settings.startupFile 型別錯誤,已回復預設".into()),
            },
            "lastSessionPath" => match opt_str(v) {
                Ok(v) => s.last_session_path = v,
                Err(()) => {
                    warnings.push("settings.lastSessionPath 型別錯誤,已回復預設".into())
                }
            },
            "lastWorkingDevice" => match opt_str(v) {
                Ok(v) => s.last_working_device = v,
                Err(()) => {
                    warnings.push("settings.lastWorkingDevice 型別錯誤,已回復預設".into())
                }
            },
            "lastWorkingBuffer" => match v.as_u64() {
                Some(n) if n <= 8192 => s.last_working_buffer = Some(n as u32),
                Some(_) | None => {
                    warnings.push("settings.lastWorkingBuffer 值不合法,已回復預設".into())
                }
            },
            "closeBehavior" => match v.as_str() {
                Some("tray") => s.close_behavior = Some(CloseBehavior::Tray),
                Some("exit") => s.close_behavior = Some(CloseBehavior::Exit),
                None if v.is_null() => s.close_behavior = None,
                Some(_) | None => warnings.push(format!(
                    "settings.closeBehavior 值不合法({v}),已回復為首次關閉時詢問"
                )),
            },
            "startMinimizedOnAutostart" => match v.as_bool() {
                Some(value) => s.start_minimized_on_autostart = value,
                None => warnings
                    .push("settings.startMinimizedOnAutostart 型別錯誤,已回復預設 false".into()),
            },
            _ => {} // 未知鍵:保留策略(merge 寫回)
        }
    }
    (s, warnings)
}

fn opt_str(v: &Value) -> Result<Option<String>, ()> {
    match v {
        Value::Null => Ok(None),
        Value::String(s) => Ok(Some(s.clone())),
        _ => Err(()),
    }
}

/// typed 欄位覆蓋進原始 JSON(未知鍵保留),寫檔用
pub fn merge(mut raw: Value, s: &Settings) -> Value {
    if !raw.is_object() {
        raw = Value::Object(Default::default());
    }
    if let Some(obj) = raw.as_object_mut() {
        let sv = serde_json::to_value(s).unwrap_or_default();
        if let Value::Object(sv_obj) = sv {
            for (k, v) in sv_obj {
                obj.insert(k, v);
            }
        }
    }
    raw
}

/// 原子寫入:同目錄 temp → flush(sync_all)→ 舊檔搬 `.bak` → rename 替換。
/// 失敗 = 舊檔不動、Err 帶原因(UI 顯示,不靜默)。
pub fn save_to(p: &Path, v: &Value) -> Result<(), String> {
    let text = serde_json::to_string_pretty(v).map_err(|e| e.to_string())?;
    if let Some(dir) = p.parent() {
        fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let mut tmp = p.to_path_buf();
    tmp.set_extension("json.tmp");
    let mut bak = p.to_path_buf();
    bak.set_extension("json.bak");
    {
        let mut f = fs::File::create(&tmp).map_err(|e| format!("建立暫存檔失敗: {e}"))?;
        f.write_all(text.as_bytes())
            .and_then(|()| f.sync_all())
            .map_err(|e| {
                let _ = fs::remove_file(&tmp);
                format!("寫入暫存檔失敗: {e}")
            })?;
    }
    let had_old = p.exists();
    if had_old {
        let _ = fs::remove_file(&bak); // 上輪 .bak 讓位(只保一版)
        fs::rename(p, &bak).map_err(|e| {
            let _ = fs::remove_file(&tmp);
            format!("備份舊設定失敗: {e}")
        })?;
    }
    fs::rename(&tmp, p).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        if had_old {
            let _ = fs::rename(&bak, p); // 搬回復原
        }
        format!("取代設定檔失敗: {e}")
    })
}

/// 讀 + migrate + normalize。檔案不見/讀不動 = 預設值(乾淨啟動,不算 warning);
/// 壞 JSON/壞欄位 = 預設 + warning。
pub fn load_from(p: &Path) -> (Settings, Vec<String>) {
    let Ok(text) = fs::read_to_string(p) else {
        return (Settings::default(), Vec::new());
    };
    match serde_json::from_str::<Value>(&text) {
        Ok(raw) => {
            let migrated = migrate(raw);
            normalize(migrated)
        }
        Err(e) => (
            Settings::default(),
            vec![format!("settings 檔損壞({e}),已回復全部預設")],
        ),
    }
}

fn load_raw(app: &AppHandle) -> (Value, Settings, Vec<String>) {
    let p = path(app).unwrap_or_default();
    let (s, w) = load_from(&p);
    let raw = fs::read_to_string(&p)
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_else(|| Value::Object(Default::default()));
    (raw, s, w)
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SettingsReply {
    pub settings: Settings,
    pub warnings: Vec<String>,
}

#[tauri::command]
pub fn get_settings(app: AppHandle) -> SettingsReply {
    let (_, s, w) = load_raw(&app);
    SettingsReply { settings: s, warnings: w }
}

/// 淺合併:只覆蓋 patch 帶的鍵;寫入前重 normalize(known field 壞值回預設+warning)
#[tauri::command]
pub fn set_settings(app: AppHandle, patch: Value) -> Result<SettingsReply, String> {
    let p = path(&app).ok_or("no app data dir")?;
    let (mut raw, _, mut warnings) = load_raw(&app);
    // patch 先進 raw(未知鍵保留、known 鍵覆蓋),再一次 normalize 全量驗證
    if let (Value::Object(dst), Value::Object(src)) = (&mut raw, patch) {
        for (k, v) in src {
            dst.insert(k, v);
        }
    }
    let (cur, w2) = normalize(migrate(raw));
    warnings.extend(w2);
    save_to(&p, &merge(serde_json::to_value(&cur).unwrap_or_default(), &cur))?;
    Ok(SettingsReply { settings: cur, warnings })
}

/// 列出資料夾內 .rmsession(檔名,排序)。副檔名不分大小寫;只列 regular file。
#[tauri::command]
pub fn list_sessions(dir: String) -> Result<Vec<String>, String> {
    let md = fs::metadata(&dir).map_err(|e| format!("資料夾無法存取({dir}):{e}"))?;
    if !md.is_dir() {
        return Err(format!("不是資料夾:{dir}"));
    }
    let mut names: Vec<String> = fs::read_dir(&dir)
        .map_err(|e| format!("讀取資料夾失敗({dir}):{e}"))?
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().map(|t| t.is_file()).unwrap_or(false))
        .filter(|e| {
            e.path()
                .extension()
                .is_some_and(|x| x.eq_ignore_ascii_case("rmsession"))
        })
        .filter_map(|e| e.file_name().into_string().ok())
        .collect();
    names.sort();
    Ok(names)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn tmp_path(tag: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("rmx-settings-test-{tag}.json"));
        let _ = fs::remove_file(&p);
        let mut bak = p.clone();
        bak.set_extension("json.bak");
        let _ = fs::remove_file(&bak);
        p
    }

    #[test]
    fn normalize_defaults_on_bad_types() {
        let (s, w) = normalize(json!({
            "startupMode": 123,
            "sessionDir": 42,
            "lastWorkingBuffer": "big",
            "startMinimizedOnAutostart": "yes",
            "unknownKey": "kept"
        }));
        assert_eq!(s.startup_mode, StartupMode::Blank);
        assert_eq!(s.session_dir, None);
        assert_eq!(s.last_working_buffer, None);
        assert_eq!(w.len(), 4); // 四個壞欄位各一條 warning
    }

    #[test]
    fn normalize_accepts_valid() {
        let (s, w) = normalize(json!({
            "startupMode": "folder",
            "sessionDir": "C:/x",
            "startupFile": "a.rmsession",
            "lastWorkingDevice": "asio:dev1",
            "lastWorkingBuffer": 256,
            "startMinimizedOnAutostart": true
        }));
        assert_eq!(s.startup_mode, StartupMode::Folder);
        assert_eq!(s.session_dir.as_deref(), Some("C:/x"));
        assert_eq!(s.last_working_device.as_deref(), Some("asio:dev1"));
        assert_eq!(s.last_working_buffer, Some(256));
        assert!(s.start_minimized_on_autostart);
        assert!(w.is_empty());
    }

    #[test]
    fn normalize_rejects_unknown_mode_value() {
        let (s, w) = normalize(json!({"startupMode": "yolo"}));
        assert_eq!(s.startup_mode, StartupMode::Blank);
        assert_eq!(w.len(), 1);
    }

    #[test]
    fn normalize_non_object_is_default() {
        let (s, w) = normalize(json!("oops"));
        assert_eq!(s, Settings::default());
        assert_eq!(w.len(), 1);
    }

    #[test]
    fn migrate_v0_adds_version() {
        let m = migrate(json!({"startupMode": "last"}));
        assert_eq!(m["schemaVersion"], json!(SCHEMA_VERSION));
        assert_eq!(m["startupMode"], json!("last"));
        assert_eq!(m["closeBehavior"], Value::Null);
        assert_eq!(m["startMinimizedOnAutostart"], json!(false));
    }

    #[test]
    fn migrate_v1_adds_unselected_close_behavior() {
        let m = migrate(json!({"schemaVersion": 1, "startupMode": "blank"}));
        assert_eq!(m["schemaVersion"], json!(SCHEMA_VERSION));
        assert_eq!(m["closeBehavior"], Value::Null);
        assert_eq!(m["startMinimizedOnAutostart"], json!(false));
    }

    #[test]
    fn migrate_v2_adds_start_minimized_default() {
        let m = migrate(json!({
            "schemaVersion": 2,
            "closeBehavior": "tray"
        }));
        assert_eq!(m["schemaVersion"], json!(SCHEMA_VERSION));
        assert_eq!(m["closeBehavior"], json!("tray"));
        assert_eq!(m["startMinimizedOnAutostart"], json!(false));
    }

    #[test]
    fn normalize_close_behavior() {
        let (tray, tray_warnings) = normalize(json!({"closeBehavior": "tray"}));
        assert_eq!(tray.close_behavior, Some(CloseBehavior::Tray));
        assert!(tray_warnings.is_empty());

        let (exit, exit_warnings) = normalize(json!({"closeBehavior": "exit"}));
        assert_eq!(exit.close_behavior, Some(CloseBehavior::Exit));
        assert!(exit_warnings.is_empty());

        let (invalid, invalid_warnings) = normalize(json!({"closeBehavior": "later"}));
        assert_eq!(invalid.close_behavior, None);
        assert_eq!(invalid_warnings.len(), 1);
    }

    #[test]
    fn merge_keeps_unknown_keys() {
        let raw = json!({"unknownKey": "kept", "startupMode": "last"});
        let mut s = Settings::default();
        s.startup_mode = StartupMode::Folder;
        let out = merge(raw, &s);
        assert_eq!(out["unknownKey"], json!("kept"));
        assert_eq!(out["startupMode"], json!("folder"));
    }

    #[test]
    fn save_to_atomic_bak_and_restore() {
        let p = tmp_path("atomic");
        save_to(&p, &json!({"v": 1})).expect("first save");
        assert!(p.exists());
        save_to(&p, &json!({"v": 2})).expect("second save");
        let mut bak = p.clone();
        bak.set_extension("json.bak");
        assert_eq!(
            fs::read_to_string(&bak).unwrap().contains("\"v\": 1"),
            true,
            "bak = 舊版"
        );
        assert!(fs::read_to_string(&p).unwrap().contains("\"v\": 2"));
        let _ = fs::remove_file(&p);
        let _ = fs::remove_file(&bak);
    }

    #[test]
    fn save_to_bad_dir_fails_with_reason() {
        // 父路徑被一個「檔案」佔住 → create_dir_all 必敗(不是被建目錄救回)
        let blocker = std::env::temp_dir().join("rmx-settings-test-blocker");
        fs::write(&blocker, "x").unwrap();
        let p = blocker.join("a.json");
        let e = save_to(&p, &json!({})).unwrap_err();
        assert!(!e.is_empty(), "錯誤訊息須帶原因");
        let _ = fs::remove_file(&blocker);
    }

    #[test]
    fn load_from_missing_file_is_clean_default() {
        let p = std::env::temp_dir().join("rmx-settings-test-missing-xyz.json");
        let (s, w) = load_from(&p);
        assert_eq!(s, Settings::default());
        assert!(w.is_empty());
    }

    #[test]
    fn load_from_corrupt_file_warns() {
        let p = tmp_path("corrupt");
        fs::write(&p, "not json{").unwrap();
        let (s, w) = load_from(&p);
        assert_eq!(s, Settings::default());
        assert_eq!(w.len(), 1);
        let _ = fs::remove_file(&p);
    }
}
