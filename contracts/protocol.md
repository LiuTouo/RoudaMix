# RoudaMix Engine Protocol v2

RoudaMix engine(process `roudamix-engine.exe`)與 UI bridge(Tauri/Rust)之間的控制面契約。
本文件 + `protocol.schema.json` 為**唯一權威**;兩側實作改協議前先改這裡與 fixtures。

## 1. 傳輸

- **Named pipe**:`\\.\pipe\roudamix-engine`,byte mode,雙向。
- **Engine 是 server**(單一 instance,`Local\roudamix-engine-singleton` mutex 防 double-spawn);
  **bridge 是 client**。client 斷線後 engine 續跑並重建 pipe 等下一個 client。
- 一次只服務一個 client:新 client 連上前,server 關閉舊連線的控制流(舊 handle 讀到 EOF)。

## 2. Framing

- 每 frame = `u32 LE` length + payload(length 不含自身 4 bytes)。
- Payload = UTF-8 JSON 單一物件。
- **Frame 上限 1 MiB**(1048576 bytes payload)。超限即協議錯誤,server 斷線該 client。
- 雙方寫入必須單次 `WriteFile`(atomic on pipe)。

## 3. 版本

- 每個 client→server frame 帶 `protocolVersion`(本版 = `2`)。v1 已淘汰:多軌 track graph 取代單鏈 rack(`set_source`/`inputMono`/`status.rack` 全數移除),舊 client 會收到 `unsupported_version`。
- Server 拒絕 `protocolVersion` 高於自身的連線(回 `error` reply `kind="unsupported_version"` 後斷線)。
- 相同 major 內**只加欄位、不改語意**;未知欄位雙方必須忽略。

## 4. 訊息種類

### 4.1 Command(client→server)

```json
{ "protocolVersion": 2, "id": 42, "kind": "ping", "payload": {} }
```

- `id`:client 產生的相關 id(u64,連線內唯一即可);reply 原樣帶回。
- `kind` + `payload` 見 §6 命令表。

### 4.2 Reply(server→client,回應 Command)

```json
{ "id": 42, "ok": true, "epoch": 7, "result": {} }
{ "id": 42, "ok": false, "epoch": 7, "error": { "code": "device_busy", "message": "..." } }
```

- 每個 command 恰一個 reply。`epoch`:engine 單調遞增狀態版本(u64),每次成功 mutation +1。
- `error.code` 見 §7。

### 4.3 Event(server→client,非請求)

```json
{ "kind": "status", "payload": { "running": false } }
```

連線建立時 server 先推一次 `snapshot` event(等同 `get_snapshot` result),client 不需先請求。

## 5. 生命週期語意

- **連線即 snapshot**:client 接上後收到 `snapshot` event;之後才送 command。
- **斷線**:bridge 對進行中 command 視為失敗(`code="disconnected"`),不透明重發;
  重連後重新 `get_snapshot` 對齊。engine 狀態不受斷線影響。
- **engine 退出**:僅 `shutdown_engine` 命令或 idle timeout(預設 12h 無 client)。
- **UI 換代**:新前端只要實作本協議即可接上(本契約存在的目的)。

## 6. 命令表

Track model(§8 `Track`):多軌 DAG。輸入軌 kind `audio`(來源 = ASIO 輸入 pair 或 sine)、
`app`(來源 = 指定程序 process loopback,**M5b 實作**,本版送 app source 回 `bad_command`)、
`fx`(無來源,靠上游 dest 指入);輸出軌 kind `output`(sink = ASIO 輸出 pair 或 WASAPI
render 裝置,**M5c 實作 wasapi**,本版送 wasapi 回 `bad_command`)。每軌一條 VST 鏈
(無上限)、gain/mute(post-fader)、多選 dests(加總;control 面保證無環)。

| kind | payload | result | 備註 |
|---|---|---|---|
| `ping` | `{}` | `{ "engineVersion": "0.1.0" }` | |
| `get_snapshot` | `{}` | `Snapshot`(§8) | |
| `list_devices` | `{}` | `{ "devices": [DeviceInfo] }` | DeviceInfo 含 `inputNames`/`outputNames`(per-channel 名,UI 下拉用) |
| `start` | `{ "deviceKey": str, "sampleRate": u32?, "bufferSize": u32? }` | `EngineStatus`(§8) | sampleRate null/缺 = driver 現行率(硬體面板才是權威,UI 一律傳 null);帶值時換率 = driver 整個重開。bufferSize null/缺 = driver preferred;**ASIO 緩衝是 host 權威**。engine 從所有軌的 source/output 收集 ASIO channel 聯集建 buffer;完全沒有 ASIO out 軌時 fallback ch 0/1。面板開啟中 start 回 `bad_command`("hardware panel is open") |
| `stop` | `{}` | `EngineStatus` | |
| `open_device_panel` | `{}` | `{ "panel": true }` | 開 driver 自帶硬體控制面板(取樣率/緩衝的最終權威);須 running,否則 `not_running`。reply 立即回(非同步):engine 在 detach thread 開面板並等其關閉(driver modal 返回或 vendor 面板 exe 結束),關閉後推 `devices_changed`;面板期間 start 被拒 |
| `track_add` | `{ "kind": "audio"\|"app"\|"fx"\|"output", "name": str?, "color": u32? }` | `{ "trackId": u32, "tracks": [Track] }` | name 缺 = 自動命名;color = 0xRRGGBB,缺 = 調色盤輪替 |
| `track_remove` | `{ "trackId": u32 }` | `{ "tracks": [Track] }` | 其他軌 dests 指向此軌的引用一併清除 |
| `track_set` | `{ "trackId": u32, "name": str?, "color": u32?, "gain": f32?, "mute": bool? }` | `{ "tracks": [Track] }` | gain = 線性乘數 [0, 4](1 = unity),缺 = 不變 |
| `track_set_source` | `{ "trackId": u32, "source": TrackSource? }` | `{ "tracks": [Track] }` | asioIn pair 被別軌占用 → `device_busy`;kind `fx`/`output` 送非 null source → `bad_command` |
| `track_set_dests` | `{ "trackId": u32, "dests": [u32] }` | `{ "tracks": [Track] }` | 多選 = 加總;含自己 → `bad_command`;未知 id → `track_not_found`;造成環 → `cycle_detected` 且**不套用** |
| `track_set_output` | `{ "trackId": u32, "output": TrackOutput? }` | `{ "tracks": [Track] }` | asioOut pair 被別軌占用 → `device_busy`;非 output 軌送非 null → `bad_command` |
| `track_move` | `{ "trackId": u32, "newIndex": u32 }` | `{ "tracks": [Track] }` | 同 kind 群組內重排(UI 欄內上下移) |
| `scan_plugins` | `{ "roots": [str]? }` | `{ "plugins": [ScanModule] }` | 同步掃描(數秒);空 roots = 預設 `C:\Program Files\Common Files\VST3`、`C:\Program Files\VST3`。載入失敗的 module 略過不 fail。**掃描與 `add_plugin` 的 module 載入驗證在隔離 worker process**(`roudamix-worker.exe`)執行:壞 module 崩潰只死 worker,engine 不受污染;worker 掛掉時掃描回報已完成的增量結果 |
| `add_plugin` | `{ "trackId": u32, "path": str, "classId": str? }` | `{ "instanceId": u32, "trackId": u32, "tracks": [Track] }` | classId 省 = module 內第一個 Audio Effect class;追加到該軌鏈尾(無上限);失敗 `plugin_load_failed` |
| `remove_plugin` | `{ "instanceId": u32 }` | `{ "tracks": [Track] }` | |
| `move_plugin` | `{ "instanceId": u32, "newIndex": u32 }` | `{ "tracks": [Track] }` | 所屬軌鏈內重排 |
| `set_bypass` | `{ "instanceId": u32, "bypassed": bool }` | `{ "tracks": [Track] }` | |
| `set_param` | `{ "instanceId": u32, "paramId": u32, "value": f32 }` | `{}` | value normalized [0,1];高頻(旋鈕)—— 成功只 reply、不廣播 status、不動 epoch;權威值見 `status.tracks[].plugins[].params` |
| `get_params` | `{ "instanceId": u32 }` | `{ "instanceId": u32, "params": [ParamInfo] }` | |
| `open_editor` | `{ "instanceId": u32 }` | `{ "instanceId": u32, "editor": true }` | 開 plugin 自帶 GUI(engine process 的 owned 浮動視窗,tab 標籤 = 軌名·plugin 名)。無 editor 回 `plugin_no_editor`。editor 內改參數 = `set_param` 語意(不廣播 status);UI 想同步權威值輪詢 `get_params` |
| `close_editor` | `{ "instanceId": u32 }` | `{}` | 關 editor 視窗(plugin 視窗自帶 X 關掉也同效) |
| `save_preset` | `{ "instanceId": u32, "path": str }` | `{ "savedPath": str }` | 寫 `.vstpreset`(VST3 容器:`Comp`=component state + `Cont`=controller state + `RmxP`=host 權威表私有 chunk,其他 host 會略過;class ID 為 32 hex 大寫 ASCII);非 mutation(不動 epoch/不廣播) |
| `load_preset` | `{ "instanceId": u32, "path": str }` | `{ "tracks": [Track] }` | 讀 `.vstpreset` 套用(component setState → controller setComponentState);成功 = mutation(廣播 status)。host 端 param 權威值重同步:檔案帶 `RmxP` chunk 時優先採用;無 `RmxP`(外部 host 存的)且 controller 同步成功時自 controller;皆無 = 保持現值。容器缺 `Comp` chunk 或 class ID 不符回 `preset_io` |
| `save_session` | `{ "path": str?, "deviceKey": str?, "sampleRate": u32?, "bufferSize": u32? }` | `{ "savedPath": str }` | path null = `%APPDATA%\RoudaMix\default.rmsession`;deviceKey/sampleRate/bufferSize 帶了就蓋寫進檔;寫 §8 SessionFile;非 mutation(不動 epoch/不廣播) |
| `load_session` | `{ "path": str }` | `{ "deviceKey": str?, "sampleRate": u32?, "bufferSize": u32? }` | 全軌重建(壞軌/消失 module 略過;dests 以舊 id→新 id map 重接);`roudamixSession != 2` 一律 `session_io` 拒載(v1 不支援);不自動 start;成功 = mutation(廣播 status) |
| `shutdown_engine` | `{}` | `{}` | 回 ack 後退出 |

M5b/M5c 保留(M5a 送了回 `internal` not implemented):`list_audio_apps` `{}` →
`{ "apps": [{ "pid": u32, "name": str }] }`;`list_render_devices` `{}` →
`{ "devices": [{ "id": str, "name": str, "default": bool, "sampleRate": u32 }] }`。

Events:

| kind | payload | 觸發 |
|---|---|---|
| `snapshot` | `Snapshot` | 連線建立時 |
| `status` | `EngineStatus`(含 `tracks`) | tracks/running/xrun/latency/裝置變更;`set_param` 不觸發 |
| `devices_changed` | `{}` | 硬體面板關閉後(driver modal 返回或 vendor 面板 exe 結束):driver 現行設定可能已變、現有 stream 可能已失效(driver 面板動緩衝會死流),client 應重新 `list_devices` 並一律 stop→start 重建 |

## 7. 錯誤碼

`unsupported_version`、`bad_frame`、`bad_command`、`not_running`、`already_running`、
`device_open_failed`、`device_lost`、`track_not_found`、`cycle_detected`、`device_busy`、
`plugin_not_found`、`plugin_load_failed`、`plugin_no_editor`、`param_not_found`、
`session_io`、`preset_io`、`plugin_state_failed`、`internal`。
(M5b 追加 `app_not_found`、`unsupported_windows`。)

錯誤碼只增不改語意;client 對未知錯誤碼當 `internal` 顯示。

## 8. 共用結構

```
DeviceInfo   { deviceKey: str, name: str, maxIn: u16, maxOut: u16, sampleRates: [u32], currentSampleRate: u32, minBufferSize: u32, maxBufferSize: u32, preferredBufferSize: u32, bufferSizes: [u32], inputNames: [str], outputNames: [str] }
EngineStatus { running: bool, deviceKey: str?, sampleRate: f32, bufferSize: u32?, inputLatency: u32?, outputLatency: u32?, xruns: u64, trackCount: u32, pluginFails: u32, tracks: [Track], error: str? }
Track        { trackId: u32, kind: "audio"|"app"|"fx"|"output", name: str, color: u32(0xRRGGBB), source: TrackSource, dests: [u32], output: TrackOutput, gain: f32, mute: bool, plugins: [RackSlot] }
TrackSource  = null | { type: "sine", freq: f32 } | { type: "asioIn", channel: u32 } | { type: "app", pid: u32, name: str? }
               (null = 無來源/FX 軌;asioIn channel = pair 基底,取 ch 與 ch+1;app = M5b)
TrackOutput  = null | { type: "asioOut", channel: u32 } | { type: "wasapi", deviceId: str }
               (null = 不落地;asioOut channel = pair 基底;wasapi = M5c)
RackSlot     { instanceId: u32, name: str, pluginPath: str, classId: str, bypassed: bool, params: [{ paramId: u32, normalized: f32 }] }
ParamInfo    { paramId: u32, name: str, normalized: f32, default: f32, bypass: bool }
ScanModule   { path: str, classes: [PluginClass] }
PluginClass  { uid: str, name: str, vendor: str, version: str, subcategories: str }
Snapshot     { epoch: u64, engineVersion: str, status: EngineStatus, tracks: [Track], lastScan: [ScanModule]? }
SessionFile  { roudamixSession: 2, deviceKey: str?, sampleRate: u32?, bufferSize: u32?, tracks: [SessionTrack] }
SessionFile.deviceKey/sampleRate/bufferSize = 最近一次成功 start 的設定;save_session payload 帶覆寫值時優先。
SessionTrack { trackId: u32, kind: str, name: str, color: u32, source: TrackSource, dests: [u32(舊 id)], output: TrackOutput, gain: f32, mute: bool, plugins: [SessionSlot] }
               (載入時 trackId 全部重發;dests 以舊→新 map 重接;app 來源存程序名,載入對不到 = 該軌靜音不失敗)
SessionSlot  { pluginPath: str, classId: str, name: str, bypassed: bool, params: [{ paramId: u32, normalized: f32 }] }
```

`bufferSizes` = driver granularity 展開的合法清單(engine 計算;空 = 用 min/max 過濾常見值)。
`currentSampleRate` = probe 時 driver 回報的現行率;硬體面板改率後重新 list_devices 可見。

JSON 欄位一律 camelCase;`f32` 序列化為 JSON number;缺項 = null(可 null 欄位見上)。

## 9. Conformance

`fixtures/protocol/valid/*.json` — 雙方實作必須成功解析並符合 schema 語意。
`fixtures/protocol/invalid/*.json` — 雙方實作必須拒絕(解析失敗或 schema 不符)。
改協議 = 改本文件 + schema + fixtures,雙方測試同步更新後才可改實作。
