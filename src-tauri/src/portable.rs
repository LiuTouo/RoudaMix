//! Portable 模式路徑判定。
//! 僅在主程式旁存在 `portable.flag` 時啟用，避免改變既有 dev/NSIS 行為。

use std::path::{Path, PathBuf};
use tauri::{AppHandle, Manager};

pub const MARKER_FILE: &str = "portable.flag";

fn root_from_exe(exe: &Path) -> Option<PathBuf> {
    let dir = exe.parent()?;
    dir.join(MARKER_FILE).is_file().then(|| dir.to_path_buf())
}

pub fn root() -> Option<PathBuf> {
    std::env::current_exe()
        .ok()
        .and_then(|exe| root_from_exe(&exe))
}

pub fn app_data_dir(app: &AppHandle) -> tauri::Result<PathBuf> {
    match root() {
        Some(root) => Ok(root.join("data")),
        None => app.path().app_data_dir(),
    }
}

pub fn webview_data_dir(app: &AppHandle) -> tauri::Result<PathBuf> {
    match root() {
        Some(root) => Ok(root.join("data").join("webview").join("main")),
        None => app.path().app_local_data_dir(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn marker_next_to_executable_enables_portable_root() {
        let dir =
            std::env::temp_dir().join(format!("roudamix-portable-test-{}", std::process::id()));
        let exe = dir.join("RoudaMix.exe");
        let marker = dir.join(MARKER_FILE);
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();

        assert_eq!(root_from_exe(&exe), None);
        fs::write(&marker, b"RoudaMix portable mode\n").unwrap();
        assert_eq!(root_from_exe(&exe), Some(dir.clone()));

        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn marker_must_be_a_regular_file() {
        let dir = std::env::temp_dir().join(format!(
            "roudamix-portable-marker-dir-test-{}",
            std::process::id()
        ));
        let exe = dir.join("RoudaMix.exe");
        let marker = dir.join(MARKER_FILE);
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&marker).unwrap();

        assert_eq!(root_from_exe(&exe), None);

        fs::remove_dir_all(&dir).unwrap();
    }
}
