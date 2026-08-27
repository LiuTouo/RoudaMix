//! engine exe 尋找 + detached spawn — UI 死不帶走 engine。

use std::path::PathBuf;
use std::process::Command;

use std::os::windows::process::CommandExt;

const CREATE_NEW_PROCESS_GROUP: u32 = 0x0000_0200;
const DETACHED_PROCESS: u32 = 0x0000_0008;
const CREATE_BREAKAWAY_FROM_JOB: u32 = 0x0100_0000;

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

pub fn spawn_detached() -> std::io::Result<()> {
    let exe = engine_exe_path().ok_or_else(|| {
        std::io::Error::new(std::io::ErrorKind::NotFound, "roudamix-engine.exe not found")
    })?;
    // BREAKAWAY 需要 job 允許;被拒就降級(仍 detached,不繼承 parent 死亡)
    let base = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    let mut spawned = Command::new(&exe).creation_flags(base | CREATE_BREAKAWAY_FROM_JOB).spawn();
    if spawned.is_err() {
        spawned = Command::new(&exe).creation_flags(base).spawn();
    }
    spawned.map(|_| ())
}
