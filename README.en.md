<div align="center">

<img src="src-tauri/icons/icon.png" width="120" alt="RoudaMix" />

# RoudaMix

**FOR STREAMERS, CREATORS & EVERY OPEN MIC**

### Your sound. Your routing.

Windows audio mixer & VST3 plugin host

[繁體中文](README.md) · **English**

[![Release](https://img.shields.io/github/v/release/LiuTouo/RoudaMix?style=flat-square&label=Release)](https://github.com/LiuTouo/RoudaMix/releases)
[![License](https://img.shields.io/badge/License-GPL--3.0--only-blue?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-0078D6?style=flat-square&logo=windows11&logoColor=white)](https://github.com/LiuTouo/RoudaMix/releases)
[![Website](https://img.shields.io/badge/Website-Interactive%20guide-teal?style=flat-square)](https://liutouo.github.io/RoudaMix/en/)

</div>

Bring your mic, game and voice chat into one workspace. Add your VST3 plugins. Choose what you hear, and what your audience hears.

---

## ✨ Features

| | Feature | Description |
| --- | --- | --- |
| 🔀 | **Dual monitor / stream outputs** | One source routed to your headphones and the stream at the same time — each mix serves its listener |
| 🎚️ | **System & per-app capture** | WASAPI system audio or per-app tracks for your game and voice chat; ASIO drivers supported |
| 🧩 | **VST3 plugin chains** | Load your own Windows x64 VST3 plugins and shape your sound |
| 👂 | **Monitor Bypass** | Split in one click: you hear the dry signal, the audience keeps the full effect |
| ⏱️ | **Latency policies** | Low Latency for monitoring, Full PDC path-aligned processing for the stream |

## ⬇️ Download

Get the latest build from [Releases](https://github.com/LiuTouo/RoudaMix/releases):

| File | Description |
| --- | --- |
| `RoudaMix-VERSION-windows-x64-portable.zip` | Extract the whole archive and run `RoudaMix.exe`; settings live in the adjacent `data` folder |
| `RoudaMix-VERSION-windows-x64-setup.exe` | Installer for the current user account |
| `RoudaMix-VERSION-source.tar.gz` | Matching source, dependencies and build instructions |

> - The portable build needs the WebView2 Evergreen Runtime; the installer downloads it when missing.
> - ASIO drivers and third-party VST3 plugins must be installed separately.
> - Releases are not Authenticode-signed; Windows may show an unknown-publisher warning.

## 🚀 Quick start

Follow the [interactive guide](https://liutouo.github.io/RoudaMix/en/) — six steps to your first streamed setup:

1. Extract and launch `RoudaMix.exe`
2. Settings → Audio / Session: select System audio (WASAPI), add an Audio track and pick your microphone
3. Install and scan your VST3 plugins, then add effects from the track's plugin list
4. Route the input to both monitor (headphones, Low Latency) and stream (Full PDC)
5. Install VB-CABLE and set the stream output to CABLE Input
6. In OBS, add an Audio Input Capture source on CABLE Output and check the meter

## 🧱 Tech stack

**Tauri 2** · Svelte 5 · TypeScript · C++ audio engine (CMake) · VST3 SDK

## 🔨 Build from source

```powershell
git clone --recurse-submodules https://github.com/LiuTouo/RoudaMix.git
./scripts/build-release.ps1 -Version 0.1.0
```

See [BUILDING.md](BUILDING.md) for full requirements and building from a Release source archive.

## 📦 Releasing (maintainers)

<details>
<summary>Expand release workflow</summary>

On GitHub, go to **Actions → Release → Run workflow**, pick a branch and enter a version such as `0.1.0`.
Check `draft` to keep it a draft; by default the Release is published after tests, build and all uploads succeed.
The workflow creates a `v0.1.0` tag on the selected commit; it does not commit any version bump.

You can also push a tag directly:

```powershell
git tag v0.1.0
git push origin v0.1.0
```

Version format is `X.Y.Z` or `X.Y.Z-rc.N` / `beta.N` / `alpha.N`.
Pre-releases are marked as prerelease; existing Releases are never overwritten.
If a failed upload leaves a draft, delete that draft and re-run; an existing tag must still point to the same commit.
The changelog is generated from commit messages between the last reachable version tag and the current commit.

</details>

## 🪪 License

Copyright (c) 2026 RoudaMix contributors.

RoudaMix's code, essential resources and build scripts are licensed under **GNU GPL version 3 only (GPL-3.0-only)**, without any warranty.
See [LICENSE](LICENSE) for the full text. Third-party components keep their original licenses; the GPLv3 option is chosen for the dual-licensed ASIO parts.
The GPL does not grant trademark rights to the RoudaMix name or logo, and does not affect your rights to use, modify and redistribute the program under the GPL.

Every Release ships the matching source and third-party license notices. The source archive excludes Git history, internal agent guidance,
local data, credentials and debug artifacts; the public GitHub repository itself remains browsable with its committed content and history.
Build instructions and license sources are documented in [BUILDING.md](BUILDING.md).
