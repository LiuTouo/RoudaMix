# Migrate latency policy in Session v3

Session 格式升級至 v3，以持久保存每個 plugin slot 的 Monitor Bypass 與每條 Output Track 的 Output Latency Policy。載入 v2 時，系統 Monitor Output 遷移為 low-latency policy，其他 outputs 遷移為 full-PDC policy，所有 plugin 預設未啟用 Monitor Bypass；shadow instance 是由 primary runtime 衍生的處理狀態，不是獨立的 Session 資料。只有使用者意圖會推進 Session dirty revision；`kLatencyChanged`、Plugin Process Load、degraded、runtime-suspended 與自動恢復都是 runtime observation，不會使 Session dirty。
