# Router golden frames

這些 fixtures 固定 `fa1eb53bda529dbd2371c2171bb5defe80f824e7` 重構前 pipe server 的
request／event／reply payload。測試以 `nlohmann::json::dump()` 比較；這與
`frame_io::write_frame` 寫入 length prefix 後的 JSON bytes 相同，因此數值表示與
envelope serialization 的差異也會失敗，不只比較 JSON 結構。
