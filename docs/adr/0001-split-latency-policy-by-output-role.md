# Split latency policy by output role

RoudaMix 將提供自動 Plugin Delay Compensation，但不以單一政策套用所有輸出：Monitor Output 由使用者透過獨立的 Monitor Bypass 明確降低 Monitoring Latency，Stream Output 則保留完整 plugin 處理並採完整路徑對齊。一般 Output Track 可選擇 Output Latency Policy，預設採完整路徑對齊；同一個 Monitor Bypass 狀態套用所有 Low-Latency Outputs，避免形成 plugin × output 的狀態矩陣。Low-Latency Output 完全不加入 Compensation Delay，因此不承諾多條輸入路徑同步。Muted paths 仍保留在 PDC 計畫內，使 mute/unmute 不改變 Total Plugin Delay 或觸發 graph 延遲重排。這保留現場監聽的可控即時性，同時避免串流混音中的平行路徑因 plugin 延遲而失去同步；全域 bypass 仍同時影響所有輸出。
