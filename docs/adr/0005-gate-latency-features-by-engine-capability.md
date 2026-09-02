# Gate latency features by engine capability

Engine snapshot 將宣告 `pluginLatencyPdcV1` capability，UI 只有在收到該 capability 時才顯示 Plugin Latency、PDC、Monitor Bypass 與 Output Latency Policy。功能以內部切片開發並維持隱藏，直到 engine、protocol、UI 與自動化測試全部完成後才啟用。這讓各層可以分階段整合，也能避免 UI 與 engine 版本不一致時顯示無法兌現的控制；telemetry ABI 不相容只停用 Process Load，不連帶停用 PDC。
