# RoudaMix Telemetry Shared Memory ABI v4

儀表/狀態高頻通道(engine → UI 單向)。控制面走 pipe(`protocol.md`);**高頻儀表不走 pipe**。

**v4 變更**:尾端新增 256-entry `pluginLoads` table；既有 v3 欄位 offset/語意不變。`abiVersion = 4`。

## 1. Mapping

- 名稱:`Local\roudamix-telemetry`(session-local namespace;engine `CreateFileMappingW` 建立,bridge `OpenFileMappingW` 開啟)。
- 大小:8192 bytes(兩 pages;固定有界)。
- Engine 為唯一 writer;bridge 唯讀。

## 2. Layout(`#pragma pack(8)`,C++ 與 Rust 兩側 `#[repr(C)]` 對照,offset 以位元組計)

| offset | size | 型別 | 欄位 | 說明 |
|---|---|---|---|---|
| 0 | 4 | u32 | `magic` | `0x524D5854`("RMXT") |
| 4 | 4 | u32 | `abiVersion` | 本版 = 4 |
| 8 | 4 | u32 | `sequence` | seqlock:寫前 odd(寫入中)、寫完 even;讀者比對前後 |
| 12 | 4 | u32 | `stripCount` | ≤ 64,有效 strip 數 |
| 16 | 8 | u64 | `xruns` | 累計 XRun |
| 24 | 4 | f32 | `callbackLoad` | audio callback CPU 佔比(兩次 publish 間 RT 忙碌 TSC / 總 TSC;P1-J 起實作,RT 端僅兩次 `__rdtsc` + relaxed add;夾 [0,2]) |
| 28 | 4 | f32 | `sampleRate` | Hz;未啟動 = 0 |
| 32 | 4 | u32 | `bufferSize` | frames;未啟動 = 0 |
| 36 | 4 | u32 | `inputLatency` | samples |
| 40 | 4 | u32 | `outputLatency` | samples |
| 44 | 4 | u32 | `_reserved0` | 對齊保留,必為 0 |
| 48 | 64×32 | 見下 | `strips[64]` | 每 strip 32 bytes |
| 2096 | 4 | u32 | `spectrumCount` | 有效頻譜 bin 數,本版 = 256;未啟動 = 0 |
| 2100 | 256×4 | f32 | `spectrumDb[256]` | engine 最終輸出的功率頻譜,**線性等間距** 0 Hz…Nyquist,dB(滿幅 sine ≈ 0,靜音/底下 = -120 floor);UI 自行做 log-freq 映射 |
| 3124 | 4 | u32 | `pluginLoadCount` | ≤ 256，有效 Process Load entries |
| 3128 | 4 | u32 | `_pluginLoadReserved` | 保留，必為 0 |
| 3132 | 256×16 | 見下 | `pluginLoads[256]` | primary 優先，再依 graph 順序配置 Monitor Shadow |

總計 3132 + 256×16 = 7228 bytes；pack(8) 補齊 sizeof = 7232 ≤ 8192。

每個 Process Load entry(16 bytes):`instanceId:u32`、`variant:u32`(0 primary、1 Monitor Shadow)、`processLoad:f32`、`reserved:f32`。`processLoad` 是量測窗口內 plugin process TSC 相對 wall-clock TSC 的比例，代表單一 logical CPU 佔比，不是整機 CPU。

每 strip(32 bytes):

| offset(相對 strip)| size | 型別 | 欄位 |
|---|---|---|---|
| 0 | 4 | u32 | `instanceId` |
| 4 | 4 | u32 | `kind`(v3 raw compatibility copy；consumer 不自行解讀) |
| 8 | 4 | f32 | `peakL` |
| 12 | 4 | f32 | `peakR` |
| 16 | 4 | f32 | `rmsL` |
| 20 | 4 | f32 | `rmsR` |
| 24 | 8 | — | `_reserved`(必為 0) |

總計 48 + 16×32 = 560 bytes ≤ 4096。其餘位元組保留,讀者忽略。

## 3. 語意

- **peak/rms 單位**:線性振幅 [0,1](非 dB);UI 端自行換 dB 與 ballistics(decay/hold)。
- **Publish 頻率**:engine 30 Hz；每個 `strips[id]` 的身分由 control-plane snapshot/status 的 `telemetryStrips` table 宣告。SHM 內既有 `instanceId`/`kind` 欄位為 ABI 相容資料，bridge 只照位元搬運；UI 不宣告數值語意，只把 frame 的 raw 值與 table 提供的 kind／owner 做相等驗證。graph mutation 前後的 table/frame 不相符時丟棄該 strip，避免跨世代錯配；`id >= stripCount` 同樣顯示無資料。未啟動時 `stripCount=0`。上限 64 strips = engine 輸出 + 軌 + plugin；超過預算時 plugin 錶先被省略(軌錶優先)。
- **Rust 對照**:

```rust
#[repr(C)]
pub struct TelemetryHeader { magic: u32, abi_version: u32, sequence: u32,
    strip_count: u32, xruns: u64, callback_load: f32, sample_rate: f32,
    buffer_size: u32, input_latency: u32, output_latency: u32, _reserved0: u32,
    strips: [TelemetryStrip; 64],
    spectrum_count: u32, spectrum_db: [f32; 256] }   // v3:offset 2096 起
#[repr(C)]
pub struct TelemetryStrip { instance_id: u32, kind: u32, peak_l: f32,
    peak_r: f32, rms_l: f32, rms_r: f32, _reserved: [u32; 2] }
```

## 4. Seqlock 讀法(reader)

```
loop {
  s1 = atomic_load(sequence, relaxed)
  if s1 is odd → continue          // 寫入中,重讀
  copy 全部欄位
  s2 = atomic_load(sequence, relaxed)
  if s1 == s2 → use copy
}
```

Writer:`sequence += 1`(變 odd)→ 寫欄位 → `sequence += 1`(變 even),全部 release/acquire。

## 5. 版本演進

- `abiVersion` 不符 → bridge 棄用 SHM(顯示「儀表不可用」),不得 crash。
- 只能在保留區/尾部加欄位並 bump `abiVersion`;不得改既有欄位 offset/語意。
