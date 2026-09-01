RoudaMix Portable (Windows x64)
================================

啟動方式
--------
1. 將 ZIP 完整解壓到可寫入的資料夾。
2. 執行 RoudaMix.exe。
3. 程式設定與 WebView2 資料會寫入同一資料夾下的 data 目錄。

必要的外部條件
--------------
- Windows 10/11 x64。
- 可用的 ASIO 驅動與對應音訊硬體。ASIO 驅動屬於系統驅動，未包含在 ZIP。
- VST3 外掛由使用者自行安裝；RoudaMix 不隨附第三方外掛。

注意事項
--------
- portable.flag 不可刪除，否則程式會恢復使用 Windows AppData。
- 啟用「登入時自動啟動」會寫入 Windows 啟動登錄。移動本資料夾前先停用，移動後再重新啟用。
- 此內部測試產物未使用 Authenticode 簽章，Windows SmartScreen 可能顯示警告。
- data 目錄包含本機設定與瀏覽器資料；更新程式時請保留該目錄。
