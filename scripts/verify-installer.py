"""Inspect NSIS's extracted payload, not just the files next to the installer."""
import argparse
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def verify_installer(directory):
    expected = {
        "roudamix-app.exe": ROOT / "target/release/roudamix-app.exe",
        "roudamix-engine.exe": ROOT / "engine/build/Release/roudamix-engine.exe",
        "roudamix-worker.exe": ROOT / "engine/build/Release/roudamix-worker.exe",
        "licenses/LICENSE.txt": ROOT / "LICENSE",
        "licenses/THIRD-PARTY-NOTICES.txt": ROOT / "target/release-legal/THIRD-PARTY-NOTICES.txt",
        "licenses/BUILD-INFO.json": ROOT / "target/release-meta/build.json",
    }
    for relative, source in expected.items():
        installed = directory / relative
        expected_bytes = source.read_bytes()
        if relative == "roudamix-app.exe":
            # Tauri stamps NSS in the installer, then restores UNK in target/release.
            marker = b"__TAURI_BUNDLE_TYPE_VAR_UNK"
            if expected_bytes.count(marker) != 1:
                raise ValueError("Unexpected Tauri bundle type marker")
            expected_bytes = expected_bytes.replace(marker, b"__TAURI_BUNDLE_TYPE_VAR_NSS")
        if not installed.is_file() or hashlib.sha256(installed.read_bytes()).digest() != hashlib.sha256(expected_bytes).digest():
            raise ValueError(f"NSIS payload missing or different: {relative}")
    for path in directory.rglob("*"):
        if path.name in {"protocol_probe.exe", "portable.flag", "data"} or path.suffix.lower() in {".pdb", ".map"}:
            raise ValueError(f"Unexpected installer payload: {path}")
    print("NSIS payload matches application, sidecars, licenses and release metadata")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    verify_installer(parser.parse_args().directory)
