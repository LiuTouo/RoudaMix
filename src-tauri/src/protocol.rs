//! 協議 envelope 與 table-driven control-plane validation。
//! 命令、結果、錯誤碼與事件 kind 的唯一手寫權威是 contracts/command_contract.json。

use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::command_contract;

pub const PROTOCOL_VERSION: u32 = 2;
pub const MAX_FRAME_BYTES: usize = 1024 * 1024;
pub const PIPE_NAME: &str = r"\\.\pipe\roudamix-engine";

pub fn error_codes() -> Vec<&'static str> {
    command_contract::error_codes()
}

#[derive(Debug, thiserror::Error)]
pub enum ProtocolError {
    #[error("parse error: {0}")]
    Parse(String),
    #[error("bad command: {0}")]
    BadCommand(String),
    #[error("unsupported version: {0}")]
    UnsupportedVersion(String),
}

impl ProtocolError {
    pub fn code(&self) -> &'static str {
        match self {
            Self::Parse(_) => "bad_frame",
            Self::BadCommand(_) => "bad_command",
            Self::UnsupportedVersion(_) => "unsupported_version",
        }
    }
}

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
        Self {
            protocol_version: PROTOCOL_VERSION,
            id,
            kind: kind.into(),
            payload,
        }
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

pub fn parse_frame(json: Value) -> Result<Frame, ProtocolError> {
    let object = json
        .as_object()
        .ok_or_else(|| parse_error("frame must be object"))?;
    if object.contains_key("protocolVersion") {
        let command: CommandEnvelope = serde_json::from_value(json)
            .map_err(|error| parse_error(format!("bad command envelope: {error}")))?;
        if command.protocol_version != PROTOCOL_VERSION {
            return Err(ProtocolError::UnsupportedVersion(format!(
                "unsupported protocolVersion: {}",
                command.protocol_version
            )));
        }
        command_contract::validate_command_payload(&command.kind, &command.payload)
            .map_err(ProtocolError::BadCommand)?;
        return Ok(Frame::Command(command));
    }
    if object.contains_key("ok") {
        let reply: ReplyEnvelope = serde_json::from_value(json)
            .map_err(|error| parse_error(format!("bad reply envelope: {error}")))?;
        return match (reply.ok, &reply.result, &reply.error) {
            (true, Some(_), None) => Ok(Frame::Reply(reply)),
            (false, None, Some(error)) if command_contract::is_error_code(&error.code) => {
                Ok(Frame::Reply(reply))
            }
            (true, _, _) => Err(parse_error("ok reply must carry result and no error")),
            (false, _, _) => Err(parse_error(
                "error reply must carry known error code and no result",
            )),
        };
    }
    if object.contains_key("kind") {
        let event: EventEnvelope = serde_json::from_value(json)
            .map_err(|error| parse_error(format!("bad event envelope: {error}")))?;
        if !command_contract::is_event_kind(&event.kind) {
            return Err(parse_error(format!("unknown event kind: {}", event.kind)));
        }
        if !event.payload.is_object() {
            return Err(parse_error("event payload must be object"));
        }
        return Ok(Frame::Event(event));
    }
    Err(parse_error("frame matches no schema variant"))
}

fn parse_error(message: impl Into<String>) -> ProtocolError {
    ProtocolError::Parse(message.into())
}

pub fn make_command(id: u64, kind: &str, payload: Value) -> Result<Value, ProtocolError> {
    command_contract::validate_command_payload(kind, &payload)
        .map_err(ProtocolError::BadCommand)?;
    serde_json::to_value(CommandEnvelope::new(id, kind, payload))
        .map_err(|error| parse_error(format!("command envelope serialization failed: {error}")))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::fs;
    use std::path::Path;

    fn check_dir(dir: &Path, must_pass: bool) -> usize {
        let mut fail = 0;
        for entry in fs::read_dir(dir).expect("fixtures dir") {
            let path = entry.unwrap().path();
            if !path.is_file() || path.extension().is_none_or(|extension| extension != "json") {
                continue;
            }
            let raw = fs::read_to_string(&path).unwrap();
            let json: Value = match serde_json::from_str(&raw) {
                Ok(value) => value,
                Err(_) => {
                    if must_pass {
                        eprintln!("FAIL(valid, bad json): {}", path.display());
                        fail += 1;
                    }
                    continue;
                }
            };
            let passed = parse_frame(json).is_ok();
            if passed != must_pass {
                eprintln!(
                    "FAIL({}): {}",
                    if must_pass { "valid" } else { "invalid" },
                    path.display()
                );
                fail += 1;
            }
        }
        fail
    }

    #[test]
    fn protocol_conformance() {
        let root = concat!(env!("CARGO_MANIFEST_DIR"), "/../fixtures/protocol");
        let valid = check_dir(Path::new(root).join("valid").as_path(), true);
        let invalid = check_dir(Path::new(root).join("invalid").as_path(), false);
        assert_eq!((valid, invalid), (0, 0), "fixture conformance failures");
    }

    #[test]
    fn reply_envelope_carries_epoch() {
        let json = json!({"id": 9, "ok": true, "epoch": 5, "result": {"revision": 12}});
        let Frame::Reply(reply) = parse_frame(json).unwrap() else {
            panic!("not a reply");
        };
        assert_eq!(reply.epoch, 5);
        assert_eq!(reply.result.unwrap()["revision"], json!(12));
    }

    #[test]
    fn scan_event_kinds_parse() {
        for kind in [
            "scan_progress",
            "scan_done",
            "scan_failed",
            "scan_cancelled",
        ] {
            let json = json!({"kind": kind, "payload": {"jobId": 3}});
            let Frame::Event(event) = parse_frame(json).unwrap() else {
                panic!("not an event: {kind}");
            };
            assert_eq!(event.kind, kind);
        }
    }

    #[test]
    fn inconsistent_reply_rejected() {
        let json = json!({"id": 1, "ok": true, "epoch": 0, "result": {"a": 1}, "error": {"code": "internal", "message": "x"}});
        assert!(parse_frame(json).is_err());
        let unknown_code = json!({"id": 1, "ok": false, "epoch": 0, "error": {"code": "no_such_code", "message": "x"}});
        assert!(parse_frame(unknown_code).is_err());
    }

    #[test]
    fn make_command_rejects_invalid_outgoing_payload() {
        let error = make_command(
            7,
            "start",
            json!({"deviceKey": "asio:test", "sampleRate": null, "bufferSize": "256"}),
        )
        .unwrap_err();
        assert_eq!(error.code(), "bad_command");
        assert!(error.to_string().contains("payload.bufferSize"));
    }
}
