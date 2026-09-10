# RoudaMix

Windows x64 音訊混音與 VST3 外掛宿主。

官網(操作教學與快速入門)：<https://liutouo.github.io/RoudaMix/>

## 下載

從 [Releases](https://github.com/LiuTouo/RoudaMix/releases) 下載：

- `RoudaMix-版本-windows-x64-portable.zip`：完整解壓後執行 `RoudaMix.exe`；設定保存在旁邊的 `data` 資料夾。
- `RoudaMix-版本-windows-x64-setup.exe`：安裝版，安裝至目前使用者帳戶。
- `RoudaMix-版本-source.tar.gz`：該版本對應原始碼、相依套件及建置說明。

portable 需要系統已安裝 WebView2 Evergreen Runtime；安裝版會在缺少時透過網路下載安裝。
ASIO 驅動與第三方 VST3 外掛須自行安裝。
目前發行檔未使用 Authenticode 簽章，Windows 可能顯示未知發行者提示。

## 發佈版本

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

## 授權

Copyright (c) 2026 RoudaMix contributors.

RoudaMix 的程式碼、必要資源及建置腳本依 **GNU GPL version 3 only（GPL-3.0-only）** 提供，無任何擔保。
完整條文見 [LICENSE](LICENSE)。第三方元件保留原授權，ASIO 的雙授權部分選用 GPLv3。
GPL 不授予 RoudaMix 名稱或標誌的商標權；不影響 GPL 對程式的使用、修改與再散布權利。

每個 Release 同時提供對應原始碼與第三方授權聲明。原始碼包不含 Git 歷史、內部代理指引、
本機資料、憑證或除錯產物；公開 GitHub 儲存庫本身仍可瀏覽其已提交內容與歷史。
建置方式與授權資料來源見 [BUILDING.md](BUILDING.md)。
