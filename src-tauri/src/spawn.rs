//! engine exe 尋找 + spawn 進 kill-on-close job — UI 退出(含 crash)= engine 跟著結束。

use std::path::PathBuf;
use std::process::Command;
use std::sync::OnceLock;

use std::os::windows::io::AsRawHandle;
use std::os::windows::process::CommandExt;

const CREATE_NEW_PROCESS_GROUP: u32 = 0x0000_0200;
const DETACHED_PROCESS: u32 = 0x0000_0008;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateJobObjectW(attrs: *const core::ffi::c_void, name: *const u16) -> *mut core::ffi::c_void;
    fn SetInformationJobObject(
        h: *mut core::ffi::c_void,
        info_class: i32,
        info: *const core::ffi::c_void,
        len: u32,
    ) -> i32;
    fn AssignProcessToJobObject(h: *mut core::ffi::c_void, process: *mut core::ffi::c_void) -> i32;
}

const JOB_OBJECT_EXTENDED_LIMIT_INFORMATION: i32 = 9;
const JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: u32 = 0x0000_2000;

/// app 生命週期共享的 job:handle 存 static 不關 —— app 活著 job 活著,
/// app 退出(含 crash/工作管理員結束)handle 收 = job 內 engine 全滅。
static JOB: OnceLock<isize> = OnceLock::new();

fn kill_on_close_job() -> Option<isize> {
    if let Some(h) = JOB.get() {
        return Some(*h);
    }
    unsafe {
        let job = CreateJobObjectW(std::ptr::null(), std::ptr::null());
        if job.is_null() {
            return None;
        }
        // JOBOBJECT_EXTENDED_LIMIT_INFORMATION(x64 sizeof = 144,須精確否則
        // SetInformationJobObject 回 ERROR_BAD_LENGTH);LimitFlags 在 offset 16
        // (兩個 i64 之後);歸零緩衝只設 KILL_ON_JOB_CLOSE
        let mut info = [0u8; 144];
        let flags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        info[16..20].copy_from_slice(&flags.to_le_bytes());
        if SetInformationJobObject(
            job,
            JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
            info.as_ptr().cast(),
            info.len() as u32,
        ) == 0
        {
            return None;
        }
        let _ = JOB.set(job as isize);
        Some(job as isize)
    }
}

fn candidates() -> Vec<PathBuf> {
    let mut v = Vec::new();
    if let Ok(p) = std::env::var("ROUDAMIX_ENGINE_EXE") {
        v.push(PathBuf::from(p));
    }
    // dev:workspace 內 engine build 輸出
    v.push(
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../engine/build/Release/roudamix-engine.exe"),
    );
    // prod:安裝包內與 app exe 同目錄
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            v.push(dir.join("roudamix-engine.exe"));
        }
    }
    v
}

pub fn engine_exe_path() -> Option<PathBuf> {
    candidates().into_iter().find(|p| p.is_file())
}

pub fn spawn_supervised() -> std::io::Result<()> {
    let exe = engine_exe_path().ok_or_else(|| {
        std::io::Error::new(std::io::ErrorKind::NotFound, "roudamix-engine.exe not found")
    })?;
    let spawned = Command::new(&exe)
        .creation_flags(DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP)
        .spawn()?;
    // 進 job(app 退出 = engine 結束)。engine 已在別的 job(沙箱/測試環境)會失敗
    // —— 降級為不管控,engine 至少能用
    if let Some(job) = kill_on_close_job() {
        unsafe {
            let _ = AssignProcessToJobObject(
                job as *mut core::ffi::c_void,
                spawned.as_raw_handle() as *mut core::ffi::c_void,
            );
        }
    }
    Ok(())
}
