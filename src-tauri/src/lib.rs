//! RoudaMix Tauri bridge — 可拋棄的 UI 橋接層。
//! engine(獨立 process)生死與此 process 無關;關視窗只斷 pipe,不影響 engine。

mod bridge;
mod commands;
mod protocol;
mod settings;
mod shm;
mod spawn;

use tauri::{
    menu::{MenuBuilder, MenuItemBuilder},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    AppHandle, Emitter, Manager,
};

fn show_dashboard(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.unminimize();
        let _ = window.show();
        let _ = window.set_focus();
    }
}

fn setup_tray(app: &mut tauri::App) -> tauri::Result<()> {
    let show = MenuItemBuilder::with_id("show-dashboard", "顯示儀表板").build(app)?;
    let quit = MenuItemBuilder::with_id("quit", "結束程式").build(app)?;
    let menu = MenuBuilder::new(app).items(&[&show, &quit]).build()?;

    let mut tray = TrayIconBuilder::with_id("main-tray")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .tooltip("RoudaMix")
        .on_menu_event(|app, event| match event.id().as_ref() {
            "show-dashboard" => show_dashboard(app),
            "quit" => {
                let _ = app.emit("tray-exit-requested", ());
            }
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let TrayIconEvent::Click {
                button: MouseButton::Left,
                button_state: MouseButtonState::Up,
                ..
            } = event
            {
                show_dashboard(tray.app_handle());
            }
        });
    if let Some(icon) = app.default_window_icon() {
        tray = tray.icon(icon.clone());
    }
    tray.build(app)?;
    Ok(())
}

#[tauri::command]
fn quit_app(app: AppHandle) {
    app.exit(0);
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(bridge::Bridge::new())
        .setup(|app| {
            setup_tray(app)?;
            let b = app.state::<bridge::Bridge>().inner().clone();
            b.start(app.handle().clone());
            shm::start(app.handle().clone());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::connect_status,
            commands::engine_command,
            commands::respawn_engine,
            settings::get_settings,
            settings::set_settings,
            settings::list_sessions,
            quit_app
        ])
        .run(tauri::generate_context!())
        .expect("tauri run failed");
}
