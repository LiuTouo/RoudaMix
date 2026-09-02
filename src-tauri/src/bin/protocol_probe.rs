use roudamix_app_lib::protocol;
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};

fn probe_dir(directory: &Path, group: &str) {
    let mut files: Vec<PathBuf> = fs::read_dir(directory)
        .expect("fixture directory")
        .filter_map(|entry| entry.ok().map(|entry| entry.path()))
        .filter(|path| path.is_file() && path.extension().is_some_and(|ext| ext == "json"))
        .collect();
    files.sort();

    for path in files {
        let raw = fs::read_to_string(&path).expect("fixture file");
        let (accepted, code) = match serde_json::from_str::<Value>(&raw) {
            Ok(frame) => match protocol::parse_frame(frame) {
                Ok(_) => (true, ""),
                Err(error) => (false, error.code()),
            },
            Err(_) => (false, "bad_frame"),
        };
        println!(
            "{}",
            json!({
                "file": format!("{group}/{}", path.file_name().unwrap().to_string_lossy()),
                "accepted": accepted,
                "code": code,
            })
        );
    }
}

fn main() {
    let root = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("fixtures/protocol"));
    probe_dir(&root.join("valid"), "valid");
    probe_dir(&root.join("invalid"), "invalid");
}
