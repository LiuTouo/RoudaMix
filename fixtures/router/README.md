# Router golden frames

這些 fixtures 固定 `fa1eb53bda529dbd2371c2171bb5defe80f824e7` 重構前 pipe server 的
request／event／reply payload。測試以 `nlohmann::json::dump()` 比較；這與
`frame_io::write_frame` 寫入 length prefix 後的 JSON bytes 相同，因此數值表示與
envelope serialization 的差異也會失敗，不只比較 JSON 結構。

## Debug result assertion 手動反例

1. 暫時將 `Router::handle_ping` 的成功 result 欄位 `engineVersion` 改成
   `engine_version`。
2. 以 Debug 組態建置並執行 `router_test`。
3. 預期 stderr 指出 `result.engineVersion is required`，接著在
   `Router::complete` 的 result contract assertion 中止，且不送出該 reply。
4. 還原暫時修改。

Release 組態定義 `NDEBUG`，不會編譯或執行這段 Router result 驗證。
