// DTO 鏡像 — 契約權威:contracts/protocol.schema.json $defs

export interface ConnectionStatus {
  connected: boolean;
  epoch: number;
  engineVersion: string;
}

export interface ParamValue {
  paramId: number;
  normalized: number;
}

export interface EngineStatus {
  running: boolean;
  deviceKey: string | null;
  sampleRate: number;
  bufferSize: number | null;
  inputLatency: number | null;
  outputLatency: number | null;
  xruns: number;
  source: "sine" | "passthrough";
  sineFreq: number;
  inputMono: boolean;
  pluginFails: number;
  rack: RackSlot[];
  error: string | null;
}

export interface RackSlot {
  instanceId: number;
  name: string;
  pluginPath: string;
  classId: string;
  bypassed: boolean;
  params: ParamValue[];
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

export interface ScanModule {
  path: string;
  classes: PluginClass[];
}

export interface Snapshot {
  epoch: number;
  engineVersion: string;
  status: EngineStatus;
  rack: RackSlot[];
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
}

// telemetry SHM(contracts/telemetry_abi.md)
export interface MeterStrip {
  instanceId: number;
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
