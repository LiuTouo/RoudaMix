//! 應用層設定(bridge 自己的偏好,engine 不涉入)— JSON 存 app data dir。
//! 淺合併寫入:未知鍵保留,向前相容。

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use tauri::{AppHandle, Manager};

#[derive(Serialize, Deserialize, Clone)]
#[serde(rename_all = "camelCase", default)]
pub struct Settings {
    /// blank | last | folder
    pub startup_mode: String,
    pub session_dir: Option<String>,
    /// folder 模式:session_dir 內選定的檔名(空 = 空白 session)
    pub startup_file: Option<String>,
    /// last 模式:上次存/載的路徑
    pub last_session_path: Option<String>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            startup_mode: "blank".into(),
            session_dir: None,
            startup_file: None,
            last_session_path: None,
        }
    }
}

fn path(app: &AppHandle) -> Option<std::path::PathBuf> {
    app.path().app_data_dir().ok().map(|d| d.join("settings.json"))
}

fn load(app: &AppHandle) -> Value {
    let Ok(s) = fs::read_to_string(path(app).unwrap_or_default()) else {
        return serde_json::to_value(Settings::default()).expect("default settings");
    };
    serde_json::from_str(&s).unwrap_or_else(|_| {
        serde_json::to_value(Settings::default()).expect("default settings")
    })
}

fn save(app: &AppHandle, v: &Value) -> Result<(), String> {
    let p = path(app).ok_or("no app data dir")?;
    if let Some(dir) = p.parent() {
        fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    fs::write(&p, serde_json::to_string_pretty(v).expect("serialize settings"))
        .map_err(|e| e.to_string())
}

#[tauri::command]
pub fn get_settings(app: AppHandle) -> Value {
    load(&app)
}

/// 淺合併:只覆蓋 patch 帶的鍵
#[tauri::command]
pub fn set_settings(app: AppHandle, patch: Value) -> Result<Value, String> {
    let mut cur = load(&app);
    if let (Value::Object(cur), Value::Object(patch)) = (&mut cur, patch) {
        for (k, v) in patch {
            cur.insert(k, v);
        }
    }
    save(&app, &cur)?;
    Ok(cur)
}

/// 列出資料夾內 .rmsession(檔名,排序)
#[tauri::command]
pub fn list_sessions(dir: String) -> Result<Vec<String>, String> {
    let mut names: Vec<String> = fs::read_dir(&dir)
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok())
        .filter(|e| e.path().extension().is_some_and(|x| x == "rmsession"))
        .filter_map(|e| e.file_name().into_string().ok())
        .collect();
    names.sort();
    Ok(names)
}
