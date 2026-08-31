//! RoudaMix Tauri bridge — 可拋棄的 UI 橋接層。
//! engine(獨立 process)生死與此 process 無關;關視窗只斷 pipe,不影響 engine。

mod bridge;
mod commands;
mod protocol;
mod settings;
mod shm;
mod spawn;

use std::ffi::OsStr;
use tauri::{
    menu::{MenuBuilder, MenuItemBuilder},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    AppHandle, Emitter, Manager,
};

const AUTOSTART_ARG: &str = "--autostart";

fn has_autostart_arg<I, S>(args: I) -> bool
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    args.into_iter()
        .any(|arg| arg.as_ref() == OsStr::new(AUTOSTART_ARG))
}

fn should_start_hidden(autostart_launch: bool, start_minimized_on_autostart: bool) -> bool {
    autostart_launch && start_minimized_on_autostart
}

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
        .plugin(
            tauri_plugin_autostart::Builder::new()
                .app_name("RoudaMix")
                .args([AUTOSTART_ARG])
                .build(),
        )
        .manage(bridge::Bridge::new())
        .setup(|app| {
            setup_tray(app)?;
            let app_settings = settings::get_settings(app.handle().clone()).settings;
            let autostart_launch = has_autostart_arg(std::env::args_os());
            if !should_start_hidden(autostart_launch, app_settings.start_minimized_on_autostart) {
                show_dashboard(app.handle());
            }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_only_explicit_autostart_argument() {
        assert!(has_autostart_arg(["RoudaMix.exe", AUTOSTART_ARG]));
        assert!(!has_autostart_arg(["RoudaMix.exe"]));
        assert!(!has_autostart_arg(["RoudaMix.exe", "--autostart=false"]));
    }

    #[test]
    fn hides_only_when_autostart_launch_and_preference_are_both_enabled() {
        assert!(!should_start_hidden(false, false));
        assert!(!should_start_hidden(false, true));
        assert!(!should_start_hidden(true, false));
        assert!(should_start_hidden(true, true));
    }
}
