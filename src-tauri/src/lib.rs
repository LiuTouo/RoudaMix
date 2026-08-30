//! RoudaMix Tauri bridge — 可拋棄的 UI 橋接層。
//! engine(獨立 process)生死與此 process 無關;關視窗只斷 pipe,不影響 engine。

mod bridge;
mod commands;
mod protocol;
mod settings;
mod shm;
mod spawn;

use tauri::Manager;

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(bridge::Bridge::new())
        .setup(|app| {
            let b = app.state::<bridge::Bridge>().inner().clone();
            b.start(app.handle().clone());
            shm::start(app.handle().clone());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::connect_status,
            commands::engine_command,
            settings::get_settings,
            settings::set_settings,
            settings::list_sessions
        ])
        .run(tauri::generate_context!())
        .expect("tauri run failed");
}
