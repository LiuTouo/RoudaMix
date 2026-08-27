//! 協議型別 — 契約權威:contracts/protocol.md + protocol.schema.json
//! 兩階段解析:先 envelope(ProtocolVersion/Id/Kind/Payload 或 Reply 或 Event),
//! 再 per-kind payload 轉型(與 C++ side 同構)。

use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const PROTOCOL_VERSION: u32 = 1;
pub const MAX_FRAME_BYTES: usize = 1024 * 1024;
pub const PIPE_NAME: &str = r"\\.\pipe\roudamix-engine";

#[derive(Debug, thiserror::Error)]
pub enum ProtocolError {
    #[error("parse error: {0}")]
    Parse(String),
    #[error("engine error {code}: {message}")]
    Engine { code: String, message: String },
}

// ---------- envelope ----------

#[derive(Debug, Serialize, Deserialize)]
pub struct CommandEnvelope {
    #[serde(rename = "protocolVersion")]
    pub protocol_version: u32,
    pub id: u64,
    pub kind: String,
    pub payload: Value,
}

impl CommandEnvelope {
    pub fn new(id: u64, kind: &str, payload: Value) -> Self {
        Self { protocol_version: PROTOCOL_VERSION, id, kind: kind.into(), payload }
    }
}

#[derive(Debug, Deserialize)]
pub struct ErrorBody {
    pub code: String,
    pub message: String,
}

#[derive(Debug, Deserialize)]
pub struct ReplyEnvelope {
    pub id: u64,
    pub ok: bool,
    pub epoch: u64,
    #[serde(default)]
    pub result: Option<Value>,
    #[serde(default)]
    pub error: Option<ErrorBody>,
}

#[derive(Debug, Deserialize)]
pub struct EventEnvelope {
    pub kind: String,
    pub payload: Value,
}

#[derive(Debug)]
pub enum Frame {
    Command(CommandEnvelope),
    Reply(ReplyEnvelope),
    Event(EventEnvelope),
}

/// 契約分派:command 四鍵 / reply 三鍵 / event 兩鍵;reply 的 ok↔result/error 一致性在此驗。
pub fn parse_frame(j: Value) -> Result<Frame, ProtocolError> {
    let obj = j.as_object().ok_or_else(|| err("frame must be object"))?;
    if obj.contains_key("protocolVersion") {
        let c: CommandEnvelope = serde_json::from_value(j)
            .map_err(|e| err(format!("bad command envelope: {e}")))?;
        if c.protocol_version != PROTOCOL_VERSION {
            return Err(err(format!(
                "unsupported protocolVersion: {}",
                c.protocol_version
            )));
        }
        validate_command_payload(&c)?;
        return Ok(Frame::Command(c));
    }
    if obj.contains_key("ok") {
        let r: ReplyEnvelope = serde_json::from_value(j)
            .map_err(|e| err(format!("bad reply envelope: {e}")))?;
        return match (r.ok, &r.result, &r.error) {
            (true, Some(_), None) => Ok(Frame::Reply(r)),
            (false, None, Some(e)) if known_error_code(&e.code) => Ok(Frame::Reply(r)),
            (true, _, _) => Err(err("ok reply must carry result and no error")),
            (false, _, _) => Err(err("error reply must carry known error code and no result")),
        }
    }
    if obj.contains_key("kind") {
        let e: EventEnvelope = serde_json::from_value(j)
            .map_err(|e| err(format!("bad event envelope: {e}")))?;
        if !known_event_kind(&e.kind) {
            return Err(err(format!("unknown event kind: {}", e.kind)));
        }
        return Ok(Frame::Event(e));
    }
    Err(err("frame matches no schema variant"))
}

fn err<S: Into<String>>(s: S) -> ProtocolError {
    ProtocolError::Parse(s.into())
}

// ---------- per-kind payload(§6)----------

pub const COMMAND_KINDS: &[&str] = &[
    "ping", "get_snapshot", "list_devices", "start", "stop", "set_source", "scan_plugins",
    "add_plugin", "remove_plugin", "move_plugin", "set_bypass", "set_param", "get_params",
    "save_session", "load_session", "shutdown_engine",
];

fn validate_command_payload(c: &CommandEnvelope) -> Result<(), ProtocolError> {
    use Value as V;
    if !COMMAND_KINDS.contains(&c.kind.as_str()) {
        return Err(err(format!("unknown command kind: {}", c.kind)));
    }
    let p = c.payload.as_object().ok_or_else(|| err("payload must be object"))?;
    let s = |k: &str| p.get(k).and_then(|v| v.as_str()).map(|_| ()).ok_or_else(|| err(format!("payload.{k} must be string")));
    match c.kind.as_str() {
        "start" => {
            s("deviceKey")?;
            match p.get("sampleRate") {
                None | Some(V::Null) => Ok(()),
                Some(v) if v.as_u64().map(|n| n <= u32::MAX as u64).unwrap_or(false) => Ok(()),
                _ => Err(err("payload.sampleRate must be u32 or null")),
            }
        }
        "set_source" => {
            match p.get("source") {
                Some(V::String(s)) if s == "sine" || s == "passthrough" => {}
                _ => return Err(err("payload.source must be sine|passthrough")),
            }
            match p.get("sineFreq") {
                Some(V::Number(_)) => Ok(()),
                _ => Err(err("payload.sineFreq must be number")),
            }
        }
        "scan_plugins" => match p.get("roots") {
            None => Ok(()),
            Some(V::Array(a)) if a.iter().all(|v| v.is_string()) => Ok(()),
            _ => Err(err("payload.roots must be string[] when present")),
        },
        "add_plugin" => s("path"),
        "remove_plugin" | "get_params" => u32_field(p, "instanceId"),
        "move_plugin" => {
            u32_field(p, "instanceId")?;
            u32_field(p, "newIndex")
        }
        "set_bypass" => {
            u32_field(p, "instanceId")?;
            match p.get("bypassed") {
                Some(V::Bool(_)) => Ok(()),
                _ => Err(err("payload.bypassed must be bool")),
            }
        }
        "set_param" => {
            u32_field(p, "instanceId")?;
            u32_field(p, "paramId")?;
            match p.get("value") {
                Some(V::Number(n)) if n.as_f64().map(|v| (0.0..=1.0).contains(&v)).unwrap_or(false) => {
                    Ok(())
                }
                _ => Err(err("payload.value must be number in [0,1]")),
            }
        }
        "save_session" => {
            match p.get("path") {
                Some(V::String(_)) | Some(V::Null) => Ok(()),
                _ => return Err(err("payload.path must be string or null")),
            }?;
            match p.get("deviceKey") {
                None | Some(V::Null) | Some(V::String(_)) => Ok(()),
                _ => Err(err("payload.deviceKey must be string or null")),
            }?;
            match p.get("sampleRate") {
                None | Some(V::Null) => Ok(()),
                Some(v) if v.as_u64().map(|n| n <= u32::MAX as u64).unwrap_or(false) => Ok(()),
                _ => Err(err("payload.sampleRate must be u32 or null")),
            }
        }
        "load_session" => s("path"),
        "ping" | "get_snapshot" | "list_devices" | "stop" | "shutdown_engine" => {
            if p.is_empty() { Ok(()) } else { Err(err("payload must be empty object")) }
        }
        _ => unreachable!(),
    }
}

fn u32_field(p: &serde_json::Map<String, Value>, k: &str) -> Result<(), ProtocolError> {
    match p.get(k) {
        Some(v) if v.as_u64().map(|n| n <= u32::MAX as u64).unwrap_or(false) => Ok(()),
        _ => Err(err(format!("payload.{k} must be u32"))),
    }
}

fn known_error_code(c: &str) -> bool {
    [
        "unsupported_version", "bad_frame", "bad_command", "not_running", "already_running",
        "device_open_failed", "device_lost", "plugin_not_found", "plugin_load_failed",
        "param_not_found", "session_io", "internal",
    ]
    .contains(&c)
}

fn known_event_kind(k: &str) -> bool {
    ["snapshot", "status", "scan_done", "rack_changed", "plugin_event"].contains(&k)
}

// ---------- 建構 helpers(server→client 方向 bridge 只解析,不建構)----------

pub fn make_command(id: u64, kind: &str, payload: Value) -> Value {
    serde_json::to_value(CommandEnvelope::new(id, kind, payload)).expect("envelope serializes")
}

// ---------- §8 共用結構(bridge→UI 用)----------

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct DeviceInfo {
    pub device_key: String,
    pub name: String,
    pub max_in: u16,
    pub max_out: u16,
    pub sample_rates: Vec<u32>,
    pub preferred_buffer_size: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct ParamValue {
    pub param_id: u32,
    pub normalized: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct EngineStatus {
    pub running: bool,
    pub device_key: Option<String>,
    pub sample_rate: f32,
    pub buffer_size: Option<u32>,
    pub input_latency: Option<u32>,
    pub output_latency: Option<u32>,
    pub xruns: u64,
    pub source: String, // "sine" | "passthrough"
    pub sine_freq: f32,
    pub plugin_fails: u32,
    pub rack: Vec<RackSlot>,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct ParamInfo {
    pub param_id: u32,
    pub name: String,
    pub normalized: f32,
    pub default: f32,
    pub bypass: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct RackSlot {
    pub instance_id: u32,
    pub name: String,
    pub plugin_path: String,
    pub class_id: String,
    pub bypassed: bool,
    pub params: Vec<ParamValue>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct PluginClass {
    pub uid: String,
    pub name: String,
    pub vendor: String,
    pub version: String,
    pub subcategories: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct ScanModule {
    pub path: String,
    pub classes: Vec<PluginClass>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct Snapshot {
    pub epoch: u64,
    pub engine_version: String,
    pub status: EngineStatus,
    pub rack: Vec<RackSlot>,
    pub last_scan: Option<Vec<ScanModule>>,
}

// ---------- conformance 測試 ----------
#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::Path;

    fn check_dir(dir: &Path, must_pass: bool) -> usize {
        let mut fail = 0;
        for entry in fs::read_dir(dir).expect("fixtures dir") {
            let p = entry.unwrap().path();
            let raw = fs::read_to_string(&p).unwrap();
            let j: Value = match serde_json::from_str(&raw) {
                Ok(v) => v,
                Err(_) => {
                    if must_pass {
                        eprintln!("FAIL(valid, bad json): {}", p.display());
                        fail += 1;
                    }
                    continue;
                }
            };
            let passed = parse_frame(j).is_ok();
            if passed != must_pass {
                eprintln!("FAIL({}): {}", if must_pass { "valid" } else { "invalid" }, p.display());
                fail += 1;
            }
        }
        fail
    }

    #[test]
    fn protocol_conformance() {
        let root = concat!(env!("CARGO_MANIFEST_DIR"), "/../fixtures/protocol");
        let v = check_dir(Path::new(root).join("valid").as_path(), true);
        let i = check_dir(Path::new(root).join("invalid").as_path(), false);
        assert_eq!((v, i), (0, 0), "fixture conformance failures");
    }
}
