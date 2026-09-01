# 建置 RoudaMix Portable

## 前置條件

- Windows 10/11 x64
- Visual Studio 2022 Build Tools（Desktop development with C++）
- CMake、Rust MSVC toolchain、Node.js 與 npm
- Microsoft 官方的 **WebView2 Fixed Version Runtime x64**，已從 `.cab` 解壓

傳給打包腳本的資料夾必須直接包含 `msedgewebview2.exe`。不要使用電腦上
`Program Files (x86)\Microsoft\EdgeWebView` 的 Evergreen 安裝目錄充當可散布套件。

## 建置

在 repository root 執行：

```powershell
npm run build:portable -- -WebView2RuntimeDir "D:\Runtime\Microsoft.WebView2.FixedVersionRuntime.x64"
```

預設流程會還原 npm 相依套件並執行 C++、Rust、Svelte 與 UI 測試。已確認本機
相依套件未變更時可傳入 `-SkipRestore`；只在診斷打包腳本時才使用 `-SkipTests`。

成功後會產生：

```text
output/portable/RoudaMix-<version>-windows-x64-portable/
output/portable/RoudaMix-<version>-windows-x64-portable.zip
output/portable/RoudaMix-<version>-windows-x64-portable.zip.sha256
```

每次成功打包都會完整覆蓋 repository root 的 `output/portable/`；建置或測試失敗時
則保留上一版輸出，不會先行刪除。

腳本會在建立 ZIP 前驗證三個 EXE 都是 x64，且沒有 `MSVCP*.dll` 或
`VCRUNTIME*.dll` 動態相依。建置期間會暫時使用
`src-tauri/WebView2Runtime/` 作為 staging cache，成功複製到輸出後自動刪除。

目前流程刻意不執行 Authenticode 簽章；公開發佈前需另行加入簽章、時間戳與正式
第三方授權盤點。
