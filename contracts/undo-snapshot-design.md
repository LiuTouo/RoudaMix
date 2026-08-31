# Undo Snapshot 設計(P2-N —— 刪除確認的後續 Undo 路線圖)

現況(P2-N 落地):`track_remove` / `remove_plugin` 前 UI 强制確認(列影響),
不做假 Undo —— plugin instance 的 component/controller state 在移除後無法從
host 端安全重建(`getState` 對已釋放的 module 不存在;「重載 module + 重放
params」會丢失 preset 之外的内部狀態,即假還原)。删除前快照已寫進
devtools console(`[undo-snapshot]` 前綴,見 `ui/src/lib/TrackStrip.svelte`),
是本文件的資料基礎。

## Command snapshot 模型

每次破壞操作前,client 端記一筆快照(serialized 自 engine 權威 status):

```jsonc
{
  "rev": 123,                    // engine revision(操作當下)
  "cmd": "track_remove",         // 或 remove_plugin
  "before": { ...TrackJSON },    // 整軌(含 plugins: path/classId/bypass/params/availability)
  "appliedAt": "2026-08-31T12:00:00Z"
}
```

還原(未來 `undo` 按鈕)= 依 `before` 重建:

1. `track_add(kind, name, color)` → 新 trackId
2. plugins 逐個 `add_plugin`(原鏈序)或 `add_placeholder_plugin`(availability != ok);
   params 逐個 `set_param`;bypass `set_bypass`
3. `track_set_source` / `track_set_output` / `track_set` (gain/mute)
4. dests:其他軌對舊 id 的引用 → `track_set_dests` 重接新 id(舊 id 映射表同 session load)
5. `track_move` 到原 master 序

## 限制(為何現在「確認」而非「Undo」)

- **plugin 內部 state**:`params` 之外,VST3 component state 只能靠 `.vstpreset`
  (preset IO)。完整還原 = 删除前先對每個 plugin `save_preset` 到暫存 ——
  該路徑與 RT 併發有 bypass+Sleep 成本(M4 已知),每刪一軌多一次同步 IO。
- **session load 已驗證的等價路徑**:`SessionFile` 的 best-effort 重建
  (placeholder 保留鏈位)就是上述步驟 1–4 —— Undo 可直接複用
  `rmx::session::load` 的重建語意,把單軌 `before` 包成單軌 session 餵入。
- **時序**:還原必須在「删除後沒有其他 mutation 插入」的前提下才不會覆蓋
  使用者的後續操作;實作時以 revision 比對(快照 rev + 删除 rev + 1 = 還原前
  rev)拒絕過期還原,與 dirty 判定同一權威來源。

## 落地順序(之後)

1. 快照從 console 改存 ring(最近 20 筆,記憶體即可;不含音訊資料)
2. 頂欄 Undo 鈕(僅對 `track_remove`/`remove_plugin` 有效)
3. 還原走 session-重建語意;`load_preset` 還原 plugin 內部 state(有 `.vstpreset` 快照時)
