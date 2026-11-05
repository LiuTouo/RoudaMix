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

## 發布

暫時以本地腳本發布（帳號 Actions 額度鎖定期間）：`scripts/release-local.ps1 -Version X.Y.Z`
會在本機執行原本 release.yml 的全部步驟（完整測試、打包、SHA256 驗證、建 tag 推送、
建立 GitHub Release，簽章金鑰讀取 `~/.tauri/roudamix.key`）。`-Draft` 先留草稿檢查，
`-ValidateOnly` 只跑前置檢查。恢復 CI 發布時，把 release.yml 的 `push: tags: ['v*']` 觸發加回。

## CI 發布加速與驗證

Release 的測試與打包合併為單一 `build` job（同一台 runner 先跑完整測試再打包，即 build-release.ps1
不帶 `-SkipTests` 的原生順序），`publish` 以該 job 全綠為門檻才公開下載。這是為了降低 Actions
額度消耗：不再為測試另開一台與打包平行的 windows runner。本機不帶 `-SkipTests` 的建置同樣
先編譯、執行完整測試。安裝包、便攜版、原始碼、更新簽章、授權及 SHA256 驗證不因合併而省略。

Rust 快取分為 `windows-test-v1` 與 `windows-release-v1`。可信任的 `main` CI 在同一個 job 內
先跑完整測試，再於 Release 依賴快取未命中時，用相同 Tauri CLI 做無封裝建置預熱
`windows-release-v1`；不使用簽章金鑰，也不發布檔案。不同 tag 不能互讀 GitHub Actions 快取，
因此發布只讀取 `main` 預熱的依賴，不依賴上一個 tag 的快取；應用程式本身仍重新編譯。
快取不存在時照常完整建置，不影響正確性。

第一次冷建置或 Rust／依賴更新會較慢；若要讓新 tag 命中快取，先等該提交的 main CI 預熱完成。
預熱會增加冷快取時的 CI 用量，命中後則跳過預熱建置。未快取 C++ 建置目錄，避免跨 runner 的
絕對路徑、工具鏈與時間戳造成不可靠的重用，也未降低正式版最佳化或縮減測試。

在 Actions 各 job 的步驟耗時與 build job summary 查看實測；`[release timing]` 記錄建置命令耗時，
不以預估秒數作發布保證。`scripts/build-release.test.ps1` 以替身命令驗證完整／分離測試模式、
編譯失敗中止及發布流程必要門檻，並由完整測試套件執行。

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
WebView2 SDK loader 1.0.3650.58 的 BSD 與 NOTICE 全文取自 Microsoft 官方同版本 NuGet 套件，
這與另外安裝的 WebView2 Runtime 授權不同。
未修改的第三方原始碼保留原版權聲明，RoudaMix 變更可依 tag 與提交記錄辨識。

下載包只包含正式程式、必要 runtime／資源、授權、版本資料；不包含 `.pdb`、`.map`、
本機 `data`、代理工作檔或 Git 目錄。原始碼公開範圍與 GPL 授權權利不能用額外禁止修改、
禁止再散布或僅限非商業用途的限制縮減。
