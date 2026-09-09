"""Release metadata, attribution and corresponding source; Python standard library only."""

import argparse
import base64
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "https://github.com/LiuTouo/RoudaMix"
VERSION = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-(alpha|beta|rc)\.(0|[1-9]\d*))?")
SOURCE_DIRS = {"engine", "src-tauri", "ui", "contracts", "fixtures", "scripts", "packaging", ".github"}
SOURCE_FILES = {"LICENSE", "README.md", "BUILDING.md", "Cargo.toml", "Cargo.lock", "package.json", "package-lock.json", ".gitmodules", ".gitignore", ".gitattributes"}


def run(*args, cwd=ROOT):
    return subprocess.check_output(args, cwd=cwd, text=True, encoding="utf-8").strip()


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, value):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def validate_version(value):
    if not VERSION.fullmatch(value):
        raise ValueError("版本須為 X.Y.Z 或 X.Y.Z-rc.N / beta.N / alpha.N（不可加 v）")
    if any(int(part) > 65535 for part in value.split("-")[0].split(".")):
        raise ValueError("Windows 版本各段不得大於 65535")
    return value


def prepare(version):
    validate_version(version)
    out = ROOT / "target/release-meta"
    out.mkdir(parents=True, exist_ok=True)
    sha = run("git", "rev-parse", "HEAD")
    write_json(out / "build.json", {"version": version, "commit": sha, "tag": f"v{version}",
                                   "source": f"{REPOSITORY}/releases/tag/v{version}"})
    write_json(out / "tauri.conf.json", {
        "version": version,
        "app": {"windows": []},
        "bundle": {
            "license": "GPL-3.0-only",
            "licenseFile": "../LICENSE",
            "resources": {"../target/release-legal/": "licenses/"},
            "windows": {"webviewInstallMode": {"type": "downloadBootstrapper"},
                        "nsis": {"installMode": "currentUser"}}
        }
    })


def license_texts(directory):
    # Include upstream notices verbatim, including nested NOTICE files.
    files = [p for p in directory.rglob("*") if p.is_file() and
             re.match(r"(?i)^(licen[sc]e|copying|notice|copyright|unlicense)([._-]|$)", p.name)]
    return [(p.relative_to(directory).as_posix(), p.read_text(encoding="utf-8", errors="replace"))
            for p in sorted(files) if p.stat().st_size < 2_000_000]


def notices():
    out = ROOT / "target/release-legal"
    out.mkdir(parents=True, exist_ok=True)
    meta = read_json(ROOT / "target/release-meta/build.json")
    shutil.copy2(ROOT / "LICENSE", out / "LICENSE.txt")
    inventory = []
    overrides = read_json(ROOT / "packaging/licenses/overrides.json")
    sections = [f"RoudaMix {meta['version']} — Third-party notices\n"
                "RoudaMix software and build scripts: GPL-3.0-only. No warranty.\n"
                "Third-party components retain their respective licenses.\n"
                "ASIO's dual-licensed interfaces are used under GPLv3; BSD-licensed host helpers retain BSD terms.\n"
                f"Corresponding source: {meta['source']} (RoudaMix-{meta['version']}-source.tar.gz).\n"]

    def add(name, version, license_id, origin, texts):
        if not texts:
            texts = [(p, (ROOT / "packaging/licenses" / p).read_text(encoding="utf-8"))
                     for p in overrides.get(f"{name}@{version}", [])]
        if not license_id or not texts:
            raise ValueError(f"Missing license metadata/text: {name} {version}")
        inventory.append({"name": name, "version": version, "license": license_id, "source": origin})
        sections.append(f"\n{'=' * 72}\n{name} {version}\nLicense: {license_id}\nSource: {origin}\n")
        for filename, text in texts:
            sections.append(f"\n--- {filename} ---\n{text}\n")

    add("Steinberg ASIO SDK", run("git", "rev-parse", "HEAD", cwd=ROOT / "engine/third_party/asio-sdk"),
        "GPL-3.0-only AND BSD-3-Clause", "https://github.com/audiosdk/asio",
        license_texts(ROOT / "engine/third_party/asio-sdk") +
        [("host/pc/asiolist.cpp (embedded BSD notice)",
          (ROOT / "engine/third_party/asio-sdk/host/pc/asiolist.cpp").read_text().split("#include")[0])])
    add("Steinberg VST 3 SDK", "vendored", "MIT", "https://github.com/steinbergmedia/vst3sdk",
        license_texts(ROOT / "engine/third_party/vst3-sdk"))
    add("nlohmann/json", "3.12.0", "MIT", "https://github.com/nlohmann/json",
        [("LICENSE.MIT", (ROOT / "packaging/licenses/nlohmann-json-MIT.txt").read_text())])
    add("IBM Plex Sans TC", "vendored", "OFL-1.1", "https://github.com/IBM/plex",
        [("OFL", (ROOT / "packaging/licenses/IBM-Plex-OFL.txt").read_text(encoding="utf-8"))])
    metadata = json.loads(run("cargo", "metadata", "--locked", "--format-version", "1",
                              "--filter-platform", "x86_64-pc-windows-msvc"))
    included = {n["id"] for n in metadata["resolve"]["nodes"]}
    for package in sorted(metadata["packages"], key=lambda p: (p["name"], p["version"])):
        if package["id"] not in included or package["source"] is None:
            continue
        directory = Path(package["manifest_path"]).parent
        texts = license_texts(directory)
        if package.get("license_file"):
            path = directory / package["license_file"]
            texts.append((path.name, path.read_text(encoding="utf-8")))
        add("crate:" + package["name"], package["version"], package["license"],
            f"https://crates.io/crates/{package['name']}/{package['version']}", texts)
    seen = set()
    for project in (ROOT, ROOT / "ui"):
        for relative in read_json(project / "package-lock.json")["packages"]:
            directory = project / relative
            if not relative or not (directory / "package.json").is_file():
                continue  # platform-specific optional packages not installed on Windows
            package = read_json(directory / "package.json")
            key = (package["name"], package["version"])
            if key in seen:
                continue
            seen.add(key)
            add("npm:" + key[0], key[1], package.get("license"),
                "https://www.npmjs.com/package/" + key[0] + "/v/" + key[1], license_texts(directory))
    (out / "THIRD-PARTY-NOTICES.txt").write_text("\n".join(sections), encoding="utf-8")
    write_json(out / "dependencies.json", inventory)
    shutil.copy2(ROOT / "target/release-meta/build.json", out / "BUILD-INFO.json")
    for path in (ROOT / "packaging/licenses").glob("*.txt"):
        shutil.copy2(path, out / path.name)
    print(f"Attribution generated for {len(inventory)} components")


def source_allowed(relative):
    path = Path(relative)
    return (relative in SOURCE_FILES or path.parts[0] in SOURCE_DIRS) and not any(
        part in {".git", "node_modules", "__pycache__", "WebView2Runtime", "build", "dist"}
        for part in path.parts)


def download_package(item, directory):
    url, integrity = item
    if not url.startswith("https://registry.npmjs.org/"):
        raise ValueError(f"Unreviewed npm package source: {url}")
    algorithm, encoded = integrity.split("-", 1)
    if algorithm not in {"sha512", "sha256", "sha1"}:
        raise ValueError("Unsupported npm integrity algorithm")
    name = hashlib.sha256(url.encode()).hexdigest() + ".tgz"
    dest = directory / name
    if not dest.exists():
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
        if base64.b64encode(hashlib.new(algorithm, data).digest()).decode() != encoded:
            raise ValueError(f"npm integrity mismatch: {url}")
        dest.write_bytes(data)
    return url, name


def source():
    meta = read_json(ROOT / "target/release-meta/build.json")
    stage = ROOT / "target/release-source" / f"RoudaMix-{meta['version']}-source"
    if stage.exists():
        raise ValueError(f"Source staging already exists; use a clean build directory: {stage}")
    stage.mkdir(parents=True)
    tracked = run("git", "ls-files", "--recurse-submodules", "-z").split("\0")
    for relative in tracked:
        if relative and source_allowed(relative):
            path = ROOT / relative
            if path.is_file():
                dest = stage / relative
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, dest)
    shutil.copytree(ROOT / "target/release-legal", stage / "LICENSES")
    shutil.copy2(ROOT / "target/release-meta/build.json", stage / "BUILD-INFO.json")
    # Vendor Rust library sources; tools and the Windows SDK remain external prerequisites.
    vendor = stage / "vendor/rust"
    subprocess.run(["cargo", "vendor", "--locked", "--versioned-dirs", str(vendor)], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    (stage / ".cargo").mkdir(exist_ok=True)
    (stage / ".cargo/config.toml").write_text(
        '[source.crates-io]\nreplace-with = "vendored-sources"\n'
        '[source.vendored-sources]\ndirectory = "vendor/rust"\n', encoding="utf-8")
    packages = {}
    for project in (ROOT, ROOT / "ui"):
        for p in read_json(project / "package-lock.json")["packages"].values():
            if p.get("resolved"):
                packages[p["resolved"]] = p["integrity"]
    npm_vendor = stage / "vendor/npm"
    npm_vendor.mkdir(parents=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        entries = dict(pool.map(lambda item: download_package(item, npm_vendor), packages.items()))
    write_json(npm_vendor / "packages.json", entries)
    output = ROOT / "output/release"
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"{stage.name}.tar.gz"
    with tarfile.open(archive, "w:gz", compresslevel=6) as tar:
        tar.add(stage, arcname=stage.name)
    print(archive)


def restore_npm():
    packages = read_json(ROOT / "vendor/npm/packages.json")
    # Only change locations; retain exact locked versions and integrity digests.
    for project in (ROOT, ROOT / "ui"):
        lock_path = project / "package-lock.json"
        lock = read_json(lock_path)
        for p in lock["packages"].values():
            if p.get("resolved"):
                path = ROOT / "vendor/npm" / packages[p["resolved"]]
                p["resolved"] = "file:" + os.path.relpath(path, project).replace("\\", "/")
        write_json(lock_path, lock)
    print("npm lockfiles now reference vendored tarballs; run npm ci and npm --prefix ui ci")


def notes():
    meta = read_json(ROOT / "target/release-meta/build.json")
    tag = meta["tag"]
    tags = run("git", "tag", "--merged", meta["commit"], "--sort=-version:refname").splitlines()
    previous = next((t for t in tags if t != tag and VERSION.fullmatch(t.removeprefix("v")) and
                     run("git", "rev-parse", f"{t}^{{commit}}") != meta["commit"]), None)
    revision = f"{previous}..{meta['commit']}" if previous else meta["commit"]
    commits = run("git", "log", "--no-merges", "--format=%h%x09%s", revision).splitlines()
    lines = [f"# RoudaMix {meta['version']}", "", "Windows 10/11 x64。", "",
             "- `*-portable.zip`：解壓後執行 RoudaMix.exe；需要系統 WebView2 Runtime。",
             "- `*-setup.exe`：安裝版；缺少 WebView2 時會透過網路下載安裝。",
             "- `*-source.tar.gz`：GPLv3 對應原始碼、相依套件與建置說明。",
             "- `SHA256SUMS.txt`：下載檔案的 SHA-256。",
             "", "目前未使用 Authenticode 簽章，Windows 可能顯示未知發行者提示。",
             "ASIO 驅動及第三方 VST3 外掛須自行安裝。", "", "## 改動日誌", ""]
    for commit in commits:
        sha, subject = commit.split("\t", 1)
        subject = subject.replace("<", "&lt;").replace(">", "&gt;")
        lines.append(f"- {subject} ([{sha}]({REPOSITORY}/commit/{sha}))")
    if previous:
        lines += ["", f"[完整比較]({REPOSITORY}/compare/{previous}...{tag})"]
    lines += ["", f"建置提交：`{meta['commit']}`", ""]
    (ROOT / "target/release-meta/notes.md").write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["prepare", "notices", "source", "restore-npm", "notes"])
    parser.add_argument("--version")
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.version or "")
    else:
        {"notices": notices, "source": source, "restore-npm": restore_npm, "notes": notes}[args.command]()
