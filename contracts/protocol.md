# RoudaMix Engine Protocol v2

RoudaMix engine(process `roudamix-engine.exe`)與 UI bridge(Tauri/Rust)之間的控制面契約。
`command_contract.json` 是 command kind、payload/result shape、error code 與 event kind 的
**唯一手寫權威**；本文件保留語意敘述，`protocol.schema.json`、generated invalid fixtures 與
TypeScript command interface 由該表衍生，不得直接修改其 generated 區段。

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
`app`(來源 = 指定程序 process loopback,`list_audio_apps` 列正在出聲的 app)、
`fx`(無來源,靠上游 dest 指入);輸出軌 kind `output`(sink = ASIO 輸出 pair 或 WASAPI
render 裝置,`list_render_devices` 列 endpoints)。每軌一條 VST 鏈
(無上限)、gain/mute(post-fader)、多選 dests(加總;control 面保證無環)。

| command | 語意與行為備註 |
|---|---|
| `ping` | — |
| `get_snapshot` | — |
| `get_latency_report` | 按需取得 output totals、PDC edge delay、buffer bytes 與完整 track/plugin latency 狀態；drawer 依 `latencyGeneration` 變更重取 |
| `list_devices` | DeviceInfo 含 `inputNames`/`outputNames`(per-channel 名,UI 下拉用) |
| `start` | sampleRate null/缺 = driver 現行率(硬體面板才是權威,UI 一律傳 null);帶值時換率 = driver 整個重開。bufferSize null/缺 = driver preferred;**ASIO 緩衝是 host 權威**。engine 從所有軌的 source/output 收集 ASIO channel 聯集建 buffer;完全沒有 ASIO out 軌時 fallback ch 0/1。面板開啟中 start 回 `bad_command`("hardware panel is open") |
| `stop` | — |
| `open_device_panel` | 開 driver 自帶硬體控制面板(取樣率/緩衝的最終權威);須 running,否則 `not_running`。reply 立即回(非同步):engine 在 detach thread 開面板並等其關閉(driver modal 返回或 vendor 面板 exe 結束),關閉後推 `devices_changed`;面板期間 start 被拒 |
| `track_add` | name 缺 = 自動命名;color = 0xRRGGBB,缺 = 調色盤輪替 |
| `track_remove` | 其他軌 dests 指向此軌的引用一併清除 |
| `track_set` | gain = 線性乘數 [0, 4](1 = unity),缺 = 不變 |
| `track_set_source` | asioIn pair 被別軌占用 → `device_busy`;kind `fx`/`output` 送非 null source → `bad_command` |
| `track_set_dests` | 多選 = 加總;含自己 → `bad_command`;未知 id → `track_not_found`;造成環 → `cycle_detected` 且**不套用** |
| `track_set_output` | asioOut pair 被別軌占用 → `device_busy`;wasapi deviceId 不存在 → `device_busy`;非 output 軌送非 null → `bad_command` |
| `track_set_latency_policy` | Output Track 的 Session v3 延遲政策；lowLatency 不加入 Compensation Delay |
| `track_move` | master 陣列絕對索引重排(erase+insert;newIndex 超尾 = 移到尾);UI 輸入/輸出帶拖放用 |
| `start_scan` | **非同步增量掃描**:立即回 jobId,掃描在 engine 背景 job 跑;同一時間一個 job(已在跑 = `reused: true` 共用現行 job)。空 roots = 預設 `C:\Program Files\Common Files\VST3`、`C:\Program Files\VST3`、`%LOCALAPPDATA%\Programs\Common\VST3`。registry 會持久化；UI 每次啟動會要求一次增量掃描，未變更 module 依 path/size/last-write fingerprint 沿用快取，不重新載入；新增/變更的檔案式或 bundle 目錄式 `.vst3` 才由隔離 worker 載入。結果經 `scan_progress`/`scan_done`/`scan_failed`/`scan_cancelled` events 回報(帶 jobId)；只有全部 roots 掃完且快取原子寫入成功才替換 registry，失敗或取消保留舊清單。worker 不在 = job failed(fail closed,不 in-process 試爆)。壞 module 進 `scan_done` 的 `failed`(quarantine,不進 `plugins`)且未變更時不重試。*(v2 早期同步的 `scan_plugins` 已移除 —— 它會佔住 engine 主 thread 且 30s reply timeout 必炸)* |
| `cancel_scan` | 取消進行中的掃描 job;沒有進行中 = `cancelling: false` |
| `add_plugin` | classId 省 = module 內第一個 Audio Effect class;追加到該軌鏈尾(無上限);失敗 `plugin_load_failed`。載入前在隔離 worker 驗證(載入 + initialize);worker 不在 = `plugin_load_failed`(fail closed) |
| `retry_plugin` | placeholder 重試載入:與 `add_plugin` 同規 worker preflight;`path` 帶了 = 重新定位到新 module 路徑。原 instanceId/鏈位/params/bypass 保留;非 placeholder → `bad_command`;載入再失敗 = 維持 placeholder、`plugin_load_failed` |
| `remove_plugin` | — |
| `move_plugin` | 所屬軌鏈內重排 |
| `set_bypass` | — |
| `set_monitor_bypass` | 只讓所有 low-latency outputs 略過該 plugin；Stream primary 不受影響 |
| `set_param` | value normalized [0,1];高頻(旋鈕)—— 成功只 reply、不廣播 status、不動 epoch;權威值見 `status.tracks[].plugins[].params` |
| `get_params` | — |
| `open_editor` | 開 plugin 自帶 GUI(engine process 的 owned 浮動視窗,tab 標籤 = 軌名·plugin 名)。無 editor 回 `plugin_no_editor`。editor 內改參數 = `set_param` 語意(不廣播 status);UI 想同步權威值輪詢 `get_params` |
| `close_editor` | 關 editor 視窗(plugin 視窗自帶 X 關掉也同效) |
| `save_preset` | 寫 `.vstpreset`(VST3 容器:`Comp`=component state + `Cont`=controller state + `RmxP`=host 權威表私有 chunk,其他 host 會略過;class ID 為 32 hex 大寫 ASCII);非 mutation(不動 epoch/不廣播) |
| `load_preset` | 讀 `.vstpreset` 套用(component setState → controller setComponentState);成功 = mutation(廣播 status)。host 端 param 權威值重同步:檔案帶 `RmxP` chunk 時優先採用;無 `RmxP`(外部 host 存的)且 controller 同步成功時自 controller;皆無 = 保持現值。容器缺 `Comp` chunk 或 class ID 不符回 `preset_io` |
| `save_session` | path null = `%APPDATA%\RoudaMix\default.rmsession`;deviceKey/sampleRate/bufferSize 帶了就蓋寫進檔;寫 §8 SessionFile;非 mutation(不動 epoch/不廣播)。**P1-F 原子寫入**:同目錄 `.tmp` 完整寫入+落盤 → 舊檔搬 `.bak` → rename 替換;任何一步失敗 = 原檔不動、err 帶原因(成功保留一份 `.bak` = 上一版,手動恢復用)。revision 隨回(UI 以此定 clean 基準;失敗 = dirty 不清) |
| `load_session` | best-effort 全軌重建(壞軌略過不整體失敗;dests 以舊 id→新 id map 重接);消失/壞掉的 plugin = 原鏈位保留 placeholder(name/path/classId/bypass/params 全存,不參與 DSP),詳情列在 `missing`;`roudamixSession != 2` 一律 `session_io` 拒載(v1 不支援,現況不動);不自動 start;成功 = mutation(廣播 status) |
| `ensure_system_outputs` | 系統輸出補齊:monitor/stream 恰好各一條(缺 = 補,重複 = 留第一個其餘降級);新 session 或載入後缺 role 時 UI 呼;沒變動 = no-op(不動 epoch) |
| `shutdown_engine` | 回 ack 後退出 |

`list_render_devices` 列出 WASAPI render endpoints，供串流軌選擇裝置。
`list_audio_apps` 列出預設 render 裝置的 active audio sessions；名稱採 exe basename，
完整路徑用於辨識同名程序。app 軌 needsRebind(pid 0)時，UI 以此清單讓使用者選擇，
不得依程序名稱猜 PID。

Events:

| event | 觸發與處理語意 |
|---|---|
| `snapshot` | 連線建立時 |
| `status` | tracks/running/xrun/latency/裝置變更;`set_param` 不觸發(但 `revision` 會前進);**stream 狀態改變的失敗(start 失敗、ASIO 重建回滾等)也推** —— client 收 reply error 後以此重同步,不得顯示 stale running |
| `scan_progress` | `start_scan` 的 job 每掃完一個 root 前 |
| `scan_done` | 掃描 job 完成;`failed` = 壞 module quarantine(不進 registry);registry 同步寫進 `Snapshot.lastScan` |
| `scan_failed` | 掃描 job 失敗(worker 不在/逾時) |
| `scan_cancelled` | `cancel_scan` 後 job 結束 |
| `devices_changed` | 硬體面板關閉後(driver modal 返回或 vendor 面板 exe 結束):driver 現行設定可能已變、現有 stream 可能已失效(driver 面板動緩衝會死流),client 應重新 `list_devices` 並一律 stop→start 重建 |

## 7. 錯誤碼

完整 catalog、意義與逐命令可能回碼以 `command_contract.json#errorCodes` 與各 command 的
`errors` 為權威；本文件不重列，以免新增 code 時形成第二個手寫定義。
錯誤碼只增不改語意；client 對未知錯誤碼當 `internal` 顯示。

## 8. 共用結構

下列內容是領域語意摘要；精確欄位 shape 仍以 `protocol.schema.json` 為準，control-plane
引用的共用型別則由 `command_contract.json` 衍生／覆寫 schema 對應定義。

```
DeviceInfo   { deviceKey: str, name: str, maxIn: u16, maxOut: u16, sampleRates: [u32], currentSampleRate: u32, minBufferSize: u32, maxBufferSize: u32, preferredBufferSize: u32, bufferSizes: [u32], inputNames: [str], outputNames: [str] }
EngineStatus { running: bool, deviceKey: str?, sampleRate: f32, bufferSize: u32?, inputLatency: u32?, outputLatency: u32?, xruns: u64, trackCount: u32, pluginFails: u32, revision: u64, latencyGeneration: u64, pluginDelay: { monitorSamples: u64?, streamSamples: u64? }, tracks: [Track], error: str? }
               (revision = 權威狀態版號,所有成功 mutation +1 含 set_param;client dirty 判定用)
Track        { trackId: u32, kind: "audio"|"app"|"fx"|"output", systemRole: "monitor"|"stream"|null, latencyPolicy: "fullPdc"|"lowLatency", name: str, color: u32(0xRRGGBB), source: TrackSource, dests: [u32], output: TrackOutput, gain: f32, mute: bool, plugins: [RackSlot], metered: bool, error: str? }
               (error 非 null = 該軌 capture/render 失效等軌道級錯誤;恢復時清空。
                systemRole = 系統輸出角色:每 session 恰好一條 monitor + 一條 stream,
                可改名/改 sink/routing 但不可刪除(engine track_remove 拒絕);
                載入缺 role = engine 確定性補齊。
                metered = telemetry strip 預算內有錶(false = 錶不可用,UI 顯示
                「無錶」狀態而非靜音;預算 64,track 先領、剩餘才輪 plugin —— 見
                track_graph.cpp plan_telemetry_strips,音訊不受影響))
TrackSource  = null | { type: "sine", freq: f32 } | { type: "asioIn", channel: u32 } | { type: "app", pid: u32, name: str? }
               (null = 無來源/FX 軌;asioIn channel = pair 基底,取 ch 與 ch+1;
                app = process loopback 抓該程序樹的音訊。**pid 0 = needsRebind**:
                session 載入只還原 name,engine 不依 exe 名猜 PID(同名多程序會綁
                錯)—— UI 以程序選擇器讓使用者選(list_audio_apps),選定後
                track_set_source 帶 pid 綁定;未綁定/程序已結束 = 軌 error)
TrackOutput  = null | { type: "asioOut", channel: u32 } | { type: "wasapi", deviceId: str }
               (null = 不落地;asioOut channel = pair 基底;wasapi = WASAPI render endpoint,
                裝置失效 = 軌 error「render device lost」)
RackSlot     { instanceId: u32, name: str, pluginPath: str, classId: str, bypassed: bool, monitorBypassed: bool, latencySamples: u64?, effectiveLatencySamples: u64?, monitorLatencySamples: u64?, runtimeState: "active"|"preparing"|"degraded"|"suspended", monitorState: "active"|"preparing"|"degraded"|"suspended", params: [{ paramId: u32, normalized: f32 }], availability: "ok"|"missing"|"loadFailed", loadError: str? }
               (availability != "ok" = placeholder:module 消失或載入失敗,原鏈位保留、
                不參與 DSP,UI 黯淡顯示;loadError = 失敗原因)
ParamInfo    { paramId: u32, name: str, normalized: f32, default: f32, bypass: bool }
ScanModule   { path: str, classes: [PluginClass] }
PluginClass  { uid: str, name: str, vendor: str, version: str, subcategories: str }
Snapshot     { epoch: u64, engineVersion: str, capabilities: [str], status: EngineStatus, tracks: [Track], lastScan: [ScanModule]? }
SessionFile  { roudamixSession: 3, deviceKey: str?, sampleRate: u32?, bufferSize: u32?, tracks: [SessionTrack] }
SessionFile.deviceKey/sampleRate/bufferSize = 最近一次成功 start 的設定;save_session payload 帶覆寫值時優先。
SessionTrack { trackId: u32, kind: str, systemRole: "monitor"|"stream"|null, latencyPolicy: "fullPdc"|"lowLatency", name: str, color: u32, source: TrackSource, dests: [u32(舊 id)], output: TrackOutput, gain: f32, mute: bool, plugins: [SessionSlot] }
               (載入時 trackId 全部重發;dests 以舊→新 map 重接;app 來源存程序名,載入對不到 = 該軌靜音不失敗;
                舊 v2 檔無 systemRole = 載入後 engine 確定性指派/補建,不可只靠名稱)
SessionSlot  { pluginPath: str, classId: str, name: str, bypassed: bool, monitorBypassed: bool, params: [{ paramId: u32, normalized: f32 }], availability: "ok"|"missing"|"loadFailed", loadError: str? }
               (availability 缺 = "ok";!= "ok" 載入時原樣重建 placeholder、不重新試爆;
                載入詳細清單回在 load_session result.missing)
MissingPlugin{ trackId: u32(檔案內舊 id), trackName: str, index: u32(鏈位), name: str, pluginPath: str, classId: str, code: str, message: str }
               (code: plugin_missing / plugin_load_failed / sandbox_unavailable)
```

`bufferSizes` = driver granularity 展開的合法清單(engine 計算;空 = 用 min/max 過濾常見值)。
`currentSampleRate` = probe 時 driver 回報的現行率;硬體面板改率後重新 list_devices 可見。

JSON 欄位一律 camelCase;`f32` 序列化為 JSON number;缺項 = null(可 null 欄位見上)。

## 9. Conformance

`fixtures/protocol/valid/*.json` — 雙方實作必須成功解析並符合 schema 語意。
`fixtures/protocol/invalid/*.json` — 雙方實作必須拒絕；`generated_*.json` 由 table 逐欄位衍生。
改 control-plane 契約時先改 `command_contract.json` 與本文件，再執行
`node scripts/generate-protocol-artifacts.mjs`；`--check` 用於 CI 防止衍生物漂移。
`node scripts/protocol-parity.mjs` 會把全部 fixtures 同時餵給 C++ 與 Rust adapter，
比較 accept/reject 與 error code。
