//! Telemetry SHM 讀取端 — 契約:contracts/telemetry_abi.md(v4)。
//! `Local\roudamix-telemetry`,seqlock 讀(odd=寫入中),45Hz 輪詢 → emit "meters"。
//! 結構必須與 engine/src/telemetry.hpp TelemetryBlockShm(#pragma pack(8), 7232B)同構。

use std::ptr;
use std::time::Duration;

use serde_json::{json, Value};
use tauri::{AppHandle, Emitter};

const FILE_MAP_READ: u32 = 0x0004;
const TELEMETRY_NAME: &str = "Local\\roudamix-telemetry";
const MAGIC: u32 = 0x524D_5854; // "RMXT"
const ABI_VERSION: u32 = 4;
const STRIPS: usize = 64;
const SPECTRUM_BINS: usize = 256;
const PLUGIN_LOADS: usize = 256;

#[repr(C)]
struct StripShm {
    instance_id: u32,
    kind: u32,
    peak_l: f32,
    peak_r: f32,
    rms_l: f32,
    rms_r: f32,
    reserved: [u32; 2],
}

#[repr(C)]
struct PluginLoadShm {
    instance_id: u32,
    variant: u32,
    process_load: f32,
    reserved: f32,
}

#[repr(C)]
struct BlockShm {
    magic: u32,
    abi_version: u32,
    sequence: u32,
    strip_count: u32,
    xruns: u64,
    callback_load: f32,
    sample_rate: f32,
    buffer_size: u32,
    input_latency: u32,
    output_latency: u32,
    reserved0: u32,
    strips: [StripShm; STRIPS],
    spectrum_count: u32, // v3:offset 2096 起
    spectrum_db: [f32; SPECTRUM_BINS],
    plugin_load_count: u32,
    plugin_load_reserved: u32,
    plugin_loads: [PluginLoadShm; PLUGIN_LOADS],
}

#[link(name = "kernel32")]
extern "system" {
    fn OpenFileMappingW(
        desired_access: u32,
        inherit: i32,
        name: *const u16,
    ) -> *mut core::ffi::c_void;
    fn MapViewOfFile(
        mapping: *mut core::ffi::c_void,
        desired_access: u32,
        off_hi: u32,
        off_lo: u32,
        bytes: usize,
    ) -> *mut core::ffi::c_void;
    fn UnmapViewOfFile(base: *const core::ffi::c_void) -> i32;
    fn CloseHandle(handle: *mut core::ffi::c_void) -> i32;
}

/// 背景 45Hz 輪詢。SHM 出現(= engine 至少 start 過一次)即訂閱。
/// sequence 停滯過久 = engine 換代重建了 SHM(舊 view 成孤兒)→ 重開 view。
pub fn start(app: AppHandle) {
    std::thread::spawn(move || {
        let mut view: *mut BlockShm = ptr::null_mut();
        let mut last_seq: u32 = u32::MAX;
        let mut stale_polls: u32 = 0;
        let mut reported_incompatible: Option<u32> = None;
        loop {
            std::thread::sleep(Duration::from_millis(22)); // ~45Hz
            if view.is_null() {
                match unsafe { open_view() } {
                    OpenView::Ready(ready) => {
                        view = ready;
                        reported_incompatible = None;
                    }
                    OpenView::Missing => {
                        last_seq = u32::MAX;
                        stale_polls = 0;
                        continue;
                    }
                    OpenView::Incompatible(found) => {
                        if reported_incompatible != Some(found) {
                            let _ = app.emit(
                                "telemetry-abi-mismatch",
                                json!({ "expected": ABI_VERSION, "found": found }),
                            );
                            reported_incompatible = Some(found);
                        }
                        last_seq = u32::MAX;
                        stale_polls = 0;
                        continue;
                    }
                }
            }
            let block = match unsafe { read_snapshot(view) } {
                Some(b) => b,
                None => continue,
            };
            if block.sequence == last_seq {
                // engine 未跑時 sequence 本來就凍著;但 2 秒以上不動也可能是
                // engine 重生後舊 view 讀到孤兒物件 —— 丟掉重開(代價 = 一次 open)
                stale_polls += 1;
                if stale_polls >= 90 {
                    unsafe { drop_view(view) };
                    view = ptr::null_mut();
                    stale_polls = 0;
                }
                continue;
            }
            stale_polls = 0;
            last_seq = block.sequence;
            let _ = app.emit("meters", to_json(&block));
        }
    });
}

unsafe fn drop_view(view: *mut BlockShm) {
    if !view.is_null() {
        unsafe { UnmapViewOfFile(view as *const core::ffi::c_void) };
    }
}

enum OpenView {
    Ready(*mut BlockShm),
    Missing,
    Incompatible(u32),
}

unsafe fn open_view() -> OpenView {
    let mut name: Vec<u16> = TELEMETRY_NAME.encode_utf16().collect();
    name.push(0);
    unsafe {
        let mapping = OpenFileMappingW(FILE_MAP_READ, 0, name.as_ptr());
        if mapping.is_null() {
            return OpenView::Missing;
        }
        // 先只 map 固定 8-byte header，避免用 v4 大小直接 map 舊版較小區塊而
        // 無法分辨「尚未建立」與「ABI 不相容」。
        let header = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 8) as *const u32;
        if header.is_null() {
            CloseHandle(mapping);
            return OpenView::Missing;
        }
        let magic = ptr::read_volatile(header);
        let abi = ptr::read_volatile(header.add(1));
        UnmapViewOfFile(header as *const core::ffi::c_void);
        if magic != MAGIC || abi != ABI_VERSION {
            CloseHandle(mapping);
            return OpenView::Incompatible(abi);
        }
        let view = MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            0,
            0,
            std::mem::size_of::<BlockShm>(),
        );
        CloseHandle(mapping); // view 保住映射,handle 可關
        if view.is_null() {
            OpenView::Missing
        } else {
            OpenView::Ready(view as *mut BlockShm)
        }
    }
}

/// seqlock 讀:seq 偶數且前後一致才算數;重試上限防 writer 卡死。
unsafe fn read_snapshot(view: *mut BlockShm) -> Option<BlockShm> {
    unsafe {
        for _ in 0..64 {
            let s1 = ptr::read_volatile(&(*view).sequence);
            if s1 % 2 != 0 {
                std::hint::spin_loop();
                continue;
            }
            std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);
            let copy = ptr::read(view);
            std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);
            let s2 = ptr::read_volatile(&(*view).sequence);
            if s1 == s2 && copy.magic == MAGIC && copy.abi_version == ABI_VERSION {
                return Some(copy);
            }
        }
        None
    }
}

fn to_json(b: &BlockShm) -> Value {
    let n = (b.strip_count as usize).min(STRIPS);
    let strips: Vec<Value> = b.strips[..n]
        .iter()
        .map(|s| {
            json!({
                "instanceId": s.instance_id,
                "kind": s.kind,
                "peakL": s.peak_l,
                "peakR": s.peak_r,
                "rmsL": s.rms_l,
                "rmsR": s.rms_r,
            })
        })
        .collect();
    let spectrum_count = (b.spectrum_count as usize).min(SPECTRUM_BINS);
    let plugin_load_count = (b.plugin_load_count as usize).min(PLUGIN_LOADS);
    let plugin_loads: Vec<Value> = b.plugin_loads[..plugin_load_count]
        .iter()
        .map(|load| {
            json!({
                "instanceId": load.instance_id,
                "variant": load.variant,
                "processLoad": load.process_load,
            })
        })
        .collect();
    json!({
        "sequence": b.sequence,
        "xruns": b.xruns,
        "callbackLoad": b.callback_load,
        "sampleRate": b.sample_rate,
        "bufferSize": b.buffer_size,
        "inputLatency": b.input_latency,
        "outputLatency": b.output_latency,
        "strips": strips,
        "pluginLoads": plugin_loads,
        "spectrum": b.spectrum_db[..spectrum_count],
    })
}

// 佈局與 C++ 契約同構(static_assert 對應)
const _: () = {
    assert!(std::mem::size_of::<StripShm>() == 32);
    assert!(std::mem::size_of::<PluginLoadShm>() == 16);
    assert!(std::mem::size_of::<BlockShm>() == 7232);
    assert!(std::mem::offset_of!(BlockShm, strips) == 48);
    assert!(std::mem::offset_of!(BlockShm, sequence) == 8);
    assert!(std::mem::offset_of!(BlockShm, spectrum_count) == 2096);
    assert!(std::mem::offset_of!(BlockShm, plugin_load_count) == 3124);
    assert!(std::mem::offset_of!(BlockShm, plugin_loads) == 3132);
};
