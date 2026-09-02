# Fetch latency reports on demand

一般 `status` events 只攜帶 Monitor／Stream totals，以及每個 plugin slot 的 Plugin Latency 與 runtime state；完整 output、path、Compensation Delay 與 instance 關係由 drawer 開啟時透過 `get_latency_report` 按需取得。Plugin Process Load 維持走 SHM telemetry。這避免每次一般狀態廣播都重送完整 graph report，同時讓頂欄與 plugin rows 保有即時所需的最小資料。
