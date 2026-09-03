# RoudaMix

RoudaMix 即時接收、處理並路由多個音訊來源，同時服務低延遲監聽與同步串流輸出。

## Language

**Plugin Latency**:
單一啟用中 plugin instance 對通過音訊引入的延遲，以 samples 為權威值並可換算為時間。
_Avoid_: VST delay, plugin lag

**Plugin Process Load**:
Plugin process 在量測窗口內占用單一 logical CPU 時間的比例，可與整體 audio callback load 比較，但不代表作業系統的整機 CPU 使用率。
_Avoid_: System CPU, callback share

**Chain Latency**:
音訊依序通過一條 plugin chain 所累積的 Plugin Latency。
_Avoid_: Track latency

**Path Latency**:
音訊從來源沿指定路由抵達某個輸出時所累積的 Plugin Latency。
_Avoid_: Total latency

**Total Plugin Delay**:
抵達特定輸出角色之有效路徑中，最大的 Path Latency；Monitor Output 與 Stream Output 各自計算，且不包含 Device Latency。
_Avoid_: Session latency, combined latency

**Compensation Delay**:
為使同一匯流點的多條路徑對齊，而額外加入較快路徑的等待時間。
_Avoid_: Latency removal

**Plugin Delay Compensation (PDC)**:
利用 Compensation Delay 維持匯流音訊同步的政策；它對齊路徑，但不會消除 Plugin Latency。
_Avoid_: Latency compensation

**Bypassed Plugin**:
仍保有原始 Plugin Latency 診斷資料、但不參與音訊處理且不計入 Path Latency 的 plugin instance。
_Avoid_: Disabled plugin

**Monitor Bypass**:
由使用者明確指定、只在 Monitor Output 路徑略過 plugin 的狀態；它不改變同一 plugin 在 Stream Output 路徑的處理。
_Avoid_: Low-latency mode, global bypass

**Runtime-suspended Plugin**:
因 primary 的執行期 Plugin Latency 超出 PDC 安全範圍而暫時停止處理的 plugin instance；此時所有輸出對該 plugin 改走 dry signal，且此狀態不寫入 Session。
_Avoid_: Bypassed Plugin, failed plugin

**Runtime-degraded Plugin**:
因 Shadow Plugin 的 Plugin Latency 超出 PDC 安全範圍而只讓 Low-Latency Outputs 對該 plugin 改走 dry signal 的狀態；Stream Output 路徑仍使用 primary 的完整處理，且此狀態不寫入 Session。
_Avoid_: Runtime-suspended Plugin, suspended plugin

**Shadow Plugin**:
當 Monitor Bypass 使 Monitor 與 Stream 的處理分岔時，為 monitor 分支額外建立的獨立 plugin instance；單向鏡像 primary 的參數、preset 與 bypass，不開放使用者直接編輯，並以自己的實際 Plugin Latency 計算所屬路徑。
_Avoid_: mirror instance, clone

**Placeholder Plugin**:
因無法載入而保留在原 chain 位置的 plugin instance；它不參與音訊處理，Plugin Latency 未知，且不計入 Path Latency。
_Avoid_: Zero-latency plugin, missing slot

**Device Latency**:
音訊裝置針對輸入與輸出分別回報的傳輸延遲，與 Plugin Latency 分開表達。
_Avoid_: Total latency

**Monitoring Latency**:
聲音從輸入來源經監聽路徑抵達輸出時，演奏者實際面對的端到端延遲。
_Avoid_: Input latency, output latency

**Monitor Output**:
系統指定、以降低 Monitoring Latency 為優先的輸出角色。
_Avoid_: Main output

**Stream Output**:
系統指定、以維持混音路徑同步為優先的輸出角色。
_Avoid_: Broadcast output

**Output Latency Policy**:
每條 Output Track 對延遲的處理意圖，可選擇完整路徑對齊或使用者控制的低延遲處理；一般 Output Track 預設採完整路徑對齊。
_Avoid_: Output mode, latency setting

**Low-Latency Output**:
採低延遲政策、不加入 Compensation Delay 的 Output Track；它套用共用的 Monitor Bypass，但不保證多條輸入路徑彼此同步。
_Avoid_: Compensated monitor, zero-latency output
