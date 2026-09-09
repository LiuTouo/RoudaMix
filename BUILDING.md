# 建置與發佈

## Windows x64 建置需求

- Windows 10/11 x64，Visual Studio 2022 C++ Build Tools 與 Windows SDK。
- Git、Node.js 24（含 npm）、Python 3.12、Rust 1.97.1 MSVC toolchain、CMake 3.24 以上、7-Zip（`7z` 在 PATH）。
- 首次建置需網路，下載相依套件與 NSIS 工具。

從 Git 儲存庫建置：

```powershell
git clone --recurse-submodules https://github.com/LiuTouo/RoudaMix.git
cd RoudaMix
git checkout v0.1.0
git submodule update --init --recursive
./scripts/build-release.ps1 -Version 0.1.0
```

輸出位於 `output/release`。版本由 tag 或手動輸入注入 Tauri 建置設定，並記錄於 `BUILD-INFO.json`。
新版本應使用乾淨 checkout，避免舊 NSIS 檔案或來源包混入。

## 從 Release 原始碼包建置

`*-source.tar.gz` 包含該提交的應用程式、ASIO submodule 內容、C++ 相依套件、完整 Rust vendor sources、
npm 鎖定版本 tarballs、資源、測試、建置腳本與授權文字。GitHub 自動提供的「Source code」zip 不包含
submodule 內容，請優先使用額外附上的對應原始碼包。

解壓後可直接建置應用程式，無須 Git 歷史：

```powershell
python scripts/release.py restore-npm
npm ci
npm --prefix ui ci
cmake -S engine -B engine/build -A x64
cmake --build engine/build --config Release
npm run tauri -- build --ci --bundles nsis --config '{"version":"0.1.0","app":{"windows":[]}}'
```

請將範例版本替換為 `BUILD-INFO.json` 的版本。官方發行版使用系統 Evergreen WebView2。
Windows SDK、編譯器及 Microsoft WebView2 Runtime 為外部工具／平台元件，不納入 GPL 原始碼包。
一般修改後編譯的版本可直接執行，沒有簽章金鑰或產品啟用限制。

## 授權資料

`scripts/release.py notices` 會讀取實際安裝的 npm 套件及 Windows 建置使用的 Cargo 相依套件，
收錄套件內的 LICENSE／NOTICE／COPYING 等全文。缺少授權資料時建置失敗。
少數上游套件未在發佈包包含授權文件，其補充資料依**套件名稱與版本**列於
`packaging/licenses/overrides.json`；更新版本後需要重新檢查。
上游全文下載來源列於 `packaging/licenses/upstream-sources.json`。
MIT、MPL 標準文字來自 SPDX license-list-data；上游只有 MIT 宣告的套件一併保留其作者與 package.json。

ASIO host 輔助程式的 BSD 條款與 SDK 的 GPLv3 選擇均保留；VST3、nlohmann/json 的 MIT 及
IBM Plex Sans TC 的 OFL 全文也包含在內。MPL 相依元件的來源隨 Rust vendor 提供。
未修改的第三方原始碼保留原版權聲明，RoudaMix 變更可依 tag 與提交記錄辨識。

下載包只包含正式程式、必要 runtime／資源、授權、版本資料；不包含 `.pdb`、`.map`、
本機 `data`、代理工作檔或 Git 目錄。原始碼公開範圍與 GPL 授權權利不能用額外禁止修改、
禁止再散布或僅限非商業用途的限制縮減。
