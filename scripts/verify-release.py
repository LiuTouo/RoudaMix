"""Check actual staged PE files and required runtime/license files before publishing."""
import argparse
import struct
from pathlib import Path


def verify(stage):
    for name in ("RoudaMix.exe", "roudamix-engine.exe", "roudamix-worker.exe"):
        data = (stage / name).read_bytes()
        if data[:2] != b"MZ":
            raise ValueError(f"Not a PE executable: {name}")
        offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[offset:offset + 4] != b"PE\0\0" or struct.unpack_from("<H", data, offset + 4)[0] != 0x8664:
            raise ValueError(f"Not Windows x64: {name}")
        # Imported CRT DLL names remain visible in PE import tables.
        if any(dll in data.lower() for dll in (b"vcruntime140.dll", b"vcruntime140_1.dll", b"msvcp140.dll")):
            raise ValueError(f"Requires external Visual C++ runtime: {name}")
    for name in ("portable.flag", "licenses/LICENSE.txt",
                 "licenses/THIRD-PARTY-NOTICES.txt", "licenses/dependencies.json", "licenses/BUILD-INFO.json"):
        if not (stage / name).is_file():
            raise ValueError(f"Missing release file: {name}")
    for path in stage.rglob("*"):
        if path.name == "data" or path.suffix.lower() in {".pdb", ".map"}:
            raise ValueError(f"Local data/debug artifact in release: {path}")
    print("Portable PE architecture, CRT dependencies and package contents verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True, type=Path)
    verify(parser.parse_args().stage)
