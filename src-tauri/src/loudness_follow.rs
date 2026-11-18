//! 響度視窗跟隨主視窗拖曳 —— 與 engine 編輯器視窗 on_ui_moved(editor_host.cpp)
//! 同語意:同量平移保留相對位置;再把「平移後的目標矩形」夾進與其交集最大
//! 螢幕的工作區(MonitorFromRect,完全無交集時回退最近螢幕)→ 永遠完整
//! 顯示在單一螢幕內;跨螢幕時沿舊螢幕邊滑、過半即整窗換邊,不卡螢幕邊角。
//! 主窗最小化不夾限:owned 視窗隨主窗隱藏,還原時 delta 帶回原相對位置。
//! 釘選(解除 owner、跨程式置頂)不影響跟隨 —— 與編輯器視窗釘選仍跟隨一致。

use std::sync::Mutex;
use tauri::{Manager, Window};
use windows_sys::Win32::Foundation::RECT;

/// 上一個主視窗位置基準(i32::MIN = 尚未建立基準的哨兵,不可能是真實座標)。
static LAST_MAIN_POS: Mutex<(i32, i32)> = Mutex::new((i32::MIN, i32::MIN));

/// 夾進工作區:保證 w×h 視窗完整可見;工作區比視窗小時夾序退化為貼齊左上。
fn clamp_into_work_area(x: i32, y: i32, w: i32, h: i32, wa: RECT) -> (i32, i32) {
    (x.clamp(wa.left, wa.left.max(wa.right - w)), y.clamp(wa.top, wa.top.max(wa.bottom - h)))
}

/// 主視窗 Moved 事件入口(lib.rs on_window_event 轉發;x/y = physical px)。
pub fn on_main_moved(main: &Window, x: i32, y: i32) {
    let mut last = LAST_MAIN_POS.lock().unwrap();
    let (lx, ly) = *last;
    *last = (x, y);
    if lx == i32::MIN { return; } // 首個事件只建立基準,不動視窗
    let (dx, dy) = (x - lx, y - ly);
    if dx == 0 && dy == 0 { return; }
    let Some(loudness) = main.app_handle().get_webview_window("loudness") else { return; };
    let (Ok(loud_hwnd), Ok(main_hwnd)) = (loudness.hwnd(), main.hwnd()) else { return; };
    // tauri/windows crate 的 HWND newtype(.0 = *mut c_void)→ windows-sys 同型指標
    let (loud_hwnd, main_hwnd) = (loud_hwnd.0, main_hwnd.0);
    use windows_sys::Win32::Graphics::Gdi::{
        GetMonitorInfoW, MonitorFromRect, MONITORINFO, MONITOR_DEFAULTTONEAREST,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        GetWindowRect, IsIconic, SetWindowPos, SWP_NOACTIVATE, SWP_NOSIZE, SWP_NOZORDER,
    };
    let mut wr = RECT { left: 0, top: 0, right: 0, bottom: 0 };
    if unsafe { GetWindowRect(loud_hwnd, &mut wr) } == 0 { return; }
    let (w, h) = (wr.right - wr.left, wr.bottom - wr.top);
    let (mut nx, mut ny) = (wr.left + dx, wr.top + dy);
    if unsafe { IsIconic(main_hwnd) } == 0 {
        let target = RECT { left: nx, top: ny, right: nx + w, bottom: ny + h };
        let monitor = unsafe { MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST) };
        let mut mi: MONITORINFO = unsafe { std::mem::zeroed() };
        mi.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
        if unsafe { GetMonitorInfoW(monitor, &mut mi) } != 0 {
            (nx, ny) = clamp_into_work_area(nx, ny, w, h, mi.rcWork);
        }
    }
    if nx != wr.left || ny != wr.top {
        unsafe {
            SetWindowPos(loud_hwnd, std::ptr::null_mut(), nx, ny, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::clamp_into_work_area as clamp;
    use windows_sys::Win32::Foundation::RECT;


    fn wa(left: i32, top: i32, right: i32, bottom: i32) -> RECT {
        RECT { left, top, right, bottom }
    }

    #[test]
    fn clamps_offscreen_position_fully_into_work_area() {
        assert_eq!(clamp(-50, -20, 200, 100, wa(0, 0, 1920, 1040)), (0, 0));
        assert_eq!(clamp(1800, 1000, 200, 100, wa(0, 0, 1920, 1040)), (1720, 940));
    }

    #[test]
    fn keeps_visible_position_untouched() {
        assert_eq!(clamp(100, 100, 200, 100, wa(0, 0, 1920, 1040)), (100, 100));
    }

    #[test]
    fn window_larger_than_work_area_aligns_top_left() {
        assert_eq!(clamp(500, 500, 5000, 4000, wa(0, 0, 1920, 1040)), (0, 0));
    }
}
