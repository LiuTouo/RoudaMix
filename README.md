<div align="center">

<img src="src-tauri/icons/icon.png" width="120" alt="RoudaMix" />

# RoudaMix

**為直播、創作與每一次開麥**

### 你的聲音，由你分流。

Windows 音訊混音與 VST3 外掛宿主

**繁體中文** · [English](README.en.md)

[![Release](https://img.shields.io/github/v/release/LiuTouo/RoudaMix?style=flat-square&label=%E7%99%BC%E8%A1%8C%E7%89%88)](https://github.com/LiuTouo/RoudaMix/releases)
[![License](https://img.shields.io/badge/License-GPL--3.0--only-blue?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-0078D6?style=flat-square&logo=windows11&logoColor=white)](https://github.com/LiuTouo/RoudaMix/releases)
[![Website](https://img.shields.io/badge/%E5%AE%98%E7%B6%B2-%E6%93%8D%E4%BD%9C%E6%95%99%E5%AD%B8-teal?style=flat-square)](https://liutouo.github.io/RoudaMix/)

</div>

把麥克風、遊戲與語音帶進同一個工作區。加上你的 VST3 外掛，分別決定自己與觀眾聽見什麼。

---

## ✨ 特色

| | 特色 | 說明 |
| --- | --- | --- |
| 🔀 | **監聽／串流雙輸出** | 同一來源同時送往耳機與串流，兩路各自合適 |
| 🎚️ | **系統音訊與 App 擷取** | WASAPI 系統音訊，或以 App 軌個別擷取遊戲與語音；支援 ASIO 驅動 |
| 🧩 | **VST3 外掛鏈** | 掛上你自行安裝的 Windows x64 VST3，打造自己的聲音 |
| 👂 | **Monitor Bypass** | 一鍵分流：自己聽乾淨的 dry 訊號，觀眾保留完整效果 |
| ⏱️ | **延遲政策** | 監聽走 Low Latency、串流保留 Full PDC 完整對齊處理 |

## ⬇️ 下載

從 [Releases](https://github.com/LiuTouo/RoudaMix/releases) 下載：

| 檔案 | 說明 |
| --- | --- |
| `RoudaMix-版本-windows-x64-portable.zip` | 完整解壓後執行 `RoudaMix.exe`；設定保存在旁邊的 `data` 資料夾 |
| `RoudaMix-版本-windows-x64-setup.exe` | 安裝版，安裝至目前使用者帳戶 |
| `RoudaMix-版本-source.tar.gz` | 該版本對應原始碼、相依套件及建置說明 |

> - 可攜版需要系統已安裝 WebView2 Evergreen Runtime；安裝版會在缺少時透過網路下載安裝。
> - ASIO 驅動與第三方 VST3 外掛須自行安裝。
> - 目前發行檔未使用 Authenticode 簽章，Windows 可能顯示未知發行者提示。

## 🚀 快速入門

跟著官網的[互動式教學](https://liutouo.github.io/RoudaMix/)走一遍，六步完成第一場直播的音訊設定：

1. 解壓並開啟 `RoudaMix.exe`
2. 設定 → 音訊 / Session：選擇「系統音訊 (WASAPI)」，新增 Audio 軌並指定麥克風
3. 安裝並掃描你的 VST3 外掛，從音軌加入效果鏈
4. 輸入軌同時接監聽（耳機、Low Latency）與串流（Full PDC）
5. 安裝 VB-CABLE，串流輸出指定 CABLE Input
6. OBS 新增「音訊輸入擷取」選 CABLE Output，確認電平

## 🧱 技術棧

**Tauri 2** · Svelte 5 · TypeScript · C++ 音訊引擎（CMake）· VST3 SDK

## 🔨 從原始碼建置

```powershell
git clone --recurse-submodules https://github.com/LiuTouo/RoudaMix.git
./scripts/build-release.ps1 -Version 0.1.0
```

完整需求與從 Release 原始碼包建置的方式見 [BUILDING.md](BUILDING.md)。

## 📦 發佈版本（維護者）

<details>
<summary>展開發佈流程</summary>

在 GitHub 選 **Actions → Release → Run workflow**，選擇分支並輸入版本，例如 `0.1.0`。
勾選 `draft` 可先保留草稿；預設在測試、建置、全部上傳成功後公開 Release。
流程會為選定提交建立 `v0.1.0` tag；不會另外提交版本號修改。

也可以推送 tag 觸發：

```powershell
git tag v0.1.0
git push origin v0.1.0
```

版本格式為 `X.Y.Z` 或 `X.Y.Z-rc.N` / `beta.N` / `alpha.N`。
預覽版會標示為 prerelease；已存在的 Release 不覆寫。
若上傳失敗留下草稿，先刪除該草稿再重跑；既有 tag 必須仍指向相同提交。
改動日誌取自上一個可達版本 tag 至目前提交的 commit 訊息。

</details>

## 🪪 授權

Copyright (c) 2026 RoudaMix contributors.

RoudaMix 的程式碼、必要資源及建置腳本依 **GNU GPL version 3 only（GPL-3.0-only）** 提供，無任何擔保。
完整條文見 [LICENSE](LICENSE)。第三方元件保留原授權，ASIO 的雙授權部分選用 GPLv3。
GPL 不授予 RoudaMix 名稱或標誌的商標權；不影響 GPL 對程式的使用、修改與再散布權利。

每個 Release 同時提供對應原始碼與第三方授權聲明。原始碼包不含 Git 歷史、內部代理指引、
本機資料、憑證或除錯產物；公開 GitHub 儲存庫本身仍可瀏覽其已提交內容與歷史。
建置方式與授權資料來源見 [BUILDING.md](BUILDING.md)。
