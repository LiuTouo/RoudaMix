# RoudaMix Telemetry Shared Memory ABI v1

儀表/狀態高頻通道(engine → UI 單向)。控制面走 pipe(`protocol.md`);**高頻儀表不走 pipe**。

## 1. Mapping

- 名稱:`Local\roudamix-telemetry`(session-local namespace;engine `CreateFileMappingW` 建立,bridge `OpenFileMappingW` 開啟)。
- 大小:4096 bytes(單 page;夠用且強制有界)。
- Engine 為唯一 writer;bridge 唯讀。

## 2. Layout(`#pragma pack(8)`,C++ 與 Rust 兩側 `#[repr(C)]` 對照,offset 以位元組計)

| offset | size | 型別 | 欄位 | 說明 |
|---|---|---|---|---|
| 0 | 4 | u32 | `magic` | `0x524D5854`("RMXT") |
| 4 | 4 | u32 | `abiVersion` | 本版 = 1 |
| 8 | 4 | u32 | `sequence` | seqlock:寫前 odd(寫入中)、寫完 even;讀者比對前後 |
| 12 | 4 | u32 | `stripCount` | ≤ 16,有效 strip 數 |
| 16 | 8 | u64 | `xruns` | 累計 XRun |
| 24 | 4 | f32 | `callbackLoad` | callback CPU 使用率估計 [0,1] |
| 28 | 4 | f32 | `sampleRate` | Hz;未啟動 = 0 |
| 32 | 4 | u32 | `bufferSize` | frames;未啟動 = 0 |
| 36 | 4 | u32 | `inputLatency` | samples |
| 40 | 4 | u32 | `outputLatency` | samples |
| 44 | 4 | u32 | `_reserved0` | 對齊保留,必為 0 |
| 48 | 16×32 | 見下 | `strips[16]` | 每 strip 32 bytes |

每 strip(32 bytes):

| offset(相對 strip)| size | 型別 | 欄位 |
|---|---|---|---|
| 0 | 4 | u32 | `instanceId` |
| 4 | 4 | u32 | `_pad0` |
| 8 | 4 | f32 | `peakL` |
| 12 | 4 | f32 | `peakR` |
| 16 | 4 | f32 | `rmsL` |
| 20 | 4 | f32 | `rmsR` |
| 24 | 8 | — | `_reserved`(必為 0) |

總計 48 + 16×32 = 560 bytes ≤ 4096。其餘位元組保留,讀者忽略。

## 3. 語意

- **peak/rms 單位**:線性振幅 [0,1](非 dB);UI 端自行換 dB 與 ballistics(decay/hold)。
- **Publish 頻率**:engine 30 Hz;`strips[0]` = engine 最終輸出(`instanceId=0xFFFFFFFF`),`strips[1..stripCount)` = rack 順序、`instanceId` 對映 pipe 協議的 `RackSlot.instanceId`(bypass slot 也量流過訊號)。未啟動時 `stripCount=0`。上限 16 strips = engine 輸出 + 15 rack slots。
- **Rust 對照**:

```rust
#[repr(C)]
pub struct TelemetryHeader { magic: u32, abi_version: u32, sequence: u32,
    strip_count: u32, xruns: u64, callback_load: f32, sample_rate: f32,
    buffer_size: u32, input_latency: u32, output_latency: u32, _reserved0: u32,
    strips: [TelemetryStrip; 16] }
#[repr(C)]
pub struct TelemetryStrip { instance_id: u32, _pad0: u32, peak_l: f32,
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
