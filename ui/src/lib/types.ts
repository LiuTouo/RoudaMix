// DTO 鏡像 — 契約權威:contracts/protocol.schema.json $defs(protocol v2 / telemetry v3)

export interface ConnectionStatus {
  connected: boolean;
  epoch: number;
  engineVersion: string;
}

export interface ParamValue {
  paramId: number;
  normalized: number;
}

export interface TrackSource {
  type: "sine" | "asioIn" | "app";
  freq?: number; // sine
  channel?: number; // asioIn(pair 基底)
  mono?: boolean; // asioIn:單聲道來源(channel 複製到 L/R)
  pid?: number; // app(M5b)
  name?: string; // app 顯示名
}

export interface TrackOutput {
  type: "asioOut" | "wasapi";
  channel?: number; // asioOut(pair 基底)
  deviceId?: string; // wasapi(M5c)
}

export interface AudioApp {
  pid: number;
  name: string;
}

export interface RenderDevice {
  id: string;
  name: string;
  default: boolean;
  sampleRate: number;
}

export interface Track {
  trackId: number;
  kind: "audio" | "app" | "fx" | "output";
  systemRole?: "monitor" | "stream" | null; // 系統輸出(不可刪;每 session 恰好各一)
  name: string;
  color: number; // 0xRRGGBB
  source: TrackSource | null;
  dests: number[];
  output: TrackOutput | null;
  gain: number; // 線性 [0,4]
  mute: boolean;
  plugins: RackSlot[];
  error?: string | null; // 軌道級錯誤(capture 失效等)
}

export interface EngineStatus {
  running: boolean;
  deviceKey: string | null;
  sampleRate: number;
  bufferSize: number | null;
  inputLatency: number | null;
  outputLatency: number | null;
  xruns: number;
  trackCount: number;
  pluginFails: number;
  revision?: number; // 權威 dirty 版號(所有成功 mutation +1,含 set_param)
  tracks: Track[];
  error: string | null;
}

export interface RackSlot {
  instanceId: number;
  name: string;
  pluginPath: string;
  classId: string;
  bypassed: boolean;
  params: ParamValue[];
  availability?: "ok" | "missing" | "loadFailed"; // != ok = placeholder(不參與 DSP)
  loadError?: string | null;
}

/** load_session reply 的 structured diagnostics(engine 端載不動的 plugin) */
export interface MissingPlugin {
  trackId: number; // 檔案內舊 id
  trackName: string;
  index: number;
  name: string;
  pluginPath: string;
  classId: string;
  code: string;
  message: string;
}

export interface ScanModule {
  path: string;
  classes: PluginClass[];
}

export interface ScanFailure {
  path: string;
  error: string;
}

export interface ParamInfo {
  paramId: number;
  name: string;
  normalized: number;
  default: number;
  bypass: boolean;
}

export interface PluginClass {
  uid: string;
  name: string;
  vendor: string;
  version: string;
  subcategories: string;
}

export interface Snapshot {
  epoch: number;
  engineVersion: string;
  status: EngineStatus;
  tracks: Track[];
  lastScan: ScanModule[] | null;
}

export interface DeviceInfo {
  deviceKey: string;
  name: string;
  maxIn: number;
  maxOut: number;
  sampleRates: number[];
  currentSampleRate: number; // driver 現行率(硬體面板才是權威)
  minBufferSize: number;
  maxBufferSize: number;
  preferredBufferSize: number;
  bufferSizes: number[]; // driver granularity 展開(空 = UI 自行過濾)
  inputNames: string[]; // per-channel 名稱(UI 下拉)
  outputNames: string[];
}

// telemetry SHM(contracts/telemetry_abi.md v3)
export interface MeterStrip {
  instanceId: number;
  kind: number; // 0 = plugin、1 = track、2 = engine 輸出
  peakL: number;
  peakR: number;
  rmsL: number;
  rmsR: number;
}

export interface MetersFrame {
  sequence: number;
  xruns: number;
  sampleRate: number;
  bufferSize: number;
  inputLatency: number;
  outputLatency: number;
  strips: MeterStrip[];
  spectrum: number[] | null; // 線性 0..Nyquist,dB(-120 floor);null = 無頻譜
}
