# 本機 PowerShell 環境兩個陷阱（2026-11-06 實測）

## 1. 遮罩輸入會被吃掉
本機主控台 `Read-Host -AsSecureString` 回傳空 SecureString（實測 masked length=0、plain length=3）。
後果：任何靠遮罩提示收密碼的腳本拿到空字串；`tauri signer sign` 遇空密碼 env 會跳互動 `Password:` 提示，可能讓預檢「假綠」而建置用空密碼失敗。
對策：密碼從剪貼簿讀（`Get-Clipboard | Out-String` 去結尾換行）；release-local.ps1 已改此路徑；互動驗證程序一律 `< NUL` 關 stdin。

## 2. npm 在函式內以 scriptblock 呼叫時參數被替換
`& $scriptblock` 內含未繫結變數參數呼叫原生指令時，該參數會被替換成 `-encodedCommand <語句原始碼> -inputFormat xml -outputFormat xml`（用假 cmd 檔實測重現；非原廠 PS 行為，疑似機器上的攔截層）。
後果：Test-CommandResult 型包裝的檢查被靜默繞過；npm 解析到 npm.ps1 shim 亦有相關的原始碼重建行為。
對策：原生指令一律在敘述層直接呼叫（不包 scriptblock）；npm 用 npm.cmd；帶變數的檢查用手寫 EAP 暫降 + $LASTEXITCODE。
