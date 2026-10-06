// 暫用原型：固定軌道／裝置讓 App 正常運作；插件目錄以 seeded PRNG 產生，
// 50/200/500 三種規模共用同一份前綴（500 → 切片），確保各版面比較時資料完全一致。
import type { DeviceInfo, EngineStatus, RackSlot, ScanModule, Track } from '../lib/types';

export const CATALOG_SIZES = [50, 200, 500] as const;

// mulberry32：小型可重現 PRNG，seed 相同 → 目錄相同。
function prng(seed: number) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) | 0;
    let t = Math.imul(state ^ (state >>> 15), 1 | state);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const VENDORS = [
  'Rouda Audio', 'Northern Sound', 'Signal Works', 'Analog Kitchen', 'Tokyo Dynamics',
  'Wavelength Audio', 'Orbital FX', 'Pixel Sound Lab', 'Deep Sea DSP', 'Vintage King Audio',
  'Camel Valley', 'Sine Language', 'Ghost Note Studios', 'Harmonic Forge', 'Mesa Modular',
  'Aurora Processing', 'Kite Audio', 'Fragment Audio', 'Nimbus Works', 'Bassline Bros',
];

// 與 pluginCatalog.ts 的 21 種功能類別對齊；`instrument` 對映「樂器」。
const CATEGORY_STRINGS = [
  'Fx|EQ', 'Fx|Dynamics', 'Fx|Reverb', 'Fx|Delay', 'Fx|Distortion',
  'Fx|Modulation', 'Fx|Spatial', 'Fx|Mastering', 'Fx|Restoration', 'Fx|Analyzer',
  'Fx|Channel Strip', 'Fx|Filter', 'Fx|Pitch Shift', 'Fx|Guitar', 'Fx|Bass',
  'Fx|Drums', 'Fx|Vocals', 'Fx|Generator', 'Fx|Microphone', 'Fx|Tools',
  'Fx|Channel Strip|EQ|Dynamics', 'Fx|Surround|EQ', 'Fx|Surround', 'Instrument|Generator|Fx',
];

const NAME_PREFIX = [
  'Pro', 'Ultra', 'Vintage', 'Studio', 'Pure', 'Deep', 'Warm', 'Classic', 'Neo', 'Hyper',
  'Smart', 'Analog', 'Crystal', 'Iron', 'Silk', 'Titan', 'Echo', 'Prime', 'Elite', 'Nu',
];
const NAME_CORE = [
  'Equalizer', 'Compressor', 'Reverb', 'Delay', 'Saturator', 'Limiter', 'Exciter',
  'Chorus', 'Flanger', 'Phaser', 'Tremolo', 'Filter', 'Gate', 'Expander', 'De-Esser',
  'Tape Machine', 'Amp Suite', 'Tuner', 'Analyzer', 'Widener', 'Pitch', 'Harmonizer',
  'Console', 'Bus', 'Room', 'Hall', 'Plate', 'Spring', 'Multiband', 'Clipper',
];
const NAME_SUFFIX = [
  '', '', '', '', ' Lite', ' Pro', ' XL', ' MK2', ' II', ' Neo', ' One', ' Zero',
  ' Elements', ' — Professional Channel Strip Edition',
];

function generateCatalog(): ScanModule[] {
  const random = prng(0x5eed);
  const pick = <T>(items: readonly T[]): T =>
    items[Math.floor(random() * items.length) % items.length]!;
  const modules: ScanModule[] = [];

  const entry = (name: string, vendor: string, subcategories: string, version?: string): ScanModule => ({
    path: `C:/Prototype/VST3/${modules.length + 1}.vst3`,
    classes: [{ uid: `prototype-${modules.length + 1}`, name, vendor, subcategories,
      version: version ?? `${1 + Math.floor(random() * 3)}.${Math.floor(random() * 9)}.${Math.floor(random() * 5)}` }],
  });

  // 邊界案例固定在最前面，任何規模都看得到：
  modules.push(
    entry('超長名稱的插件 — Studio Channel Strip Professional Edition', 'Studio', 'Fx|Channel Strip|EQ|Dynamics'),
    entry('Utility', '', ''),                                            // 空廠牌＋未分類
    entry('同名壓縮器', 'Signal Works', 'Fx|Dynamics'),                    // 同名＋同來源 → 顯示完整路徑
    entry('同名壓縮器', 'Signal Works', 'Fx|Dynamics'),
    entry('環繞寬化器', 'Orbital FX', 'Fx|Surround'),                     // SDK 類別 → Surround
    entry('厅堂等化器 7.1', 'Nimbus Works', 'Fx|Surround|EQ'),            // 末端 Surround 僅描述聲道 → EQ
    entry('No Version Reverb', 'Camel Valley', 'Fx|Reverb', ''),          // 版本未提供
    entry('Analog Kitchen Drum Engine', 'Analog Kitchen', 'Instrument|Generator|Fx'),
  );
  // 同一 module 提供多個同名 class → 來源顯示「路徑 · classId」。
  const twin = modules.length + 1;
  modules.push({
    path: `C:/Prototype/VST3/${twin}.vst3`,
    classes: [
      { uid: `prototype-${twin}-a`, name: '雙生限制器', vendor: 'Harmonic Forge', version: '2.0', subcategories: 'Fx|Dynamics' },
      { uid: `prototype-${twin}-b`, name: '雙生限制器', vendor: 'Harmonic Forge', version: '2.0', subcategories: 'Fx|Dynamics' },
    ],
  });

  const target = CATALOG_SIZES[CATALOG_SIZES.length - 1];
  while (modules.length < target) {
    const name = `${pick(NAME_PREFIX)} ${pick(NAME_CORE)}${pick(NAME_SUFFIX)}`;
    modules.push(entry(name, pick(VENDORS), pick(CATEGORY_STRINGS),
      random() < 0.06 ? '' : undefined));
  }
  return modules;
}

const fullCatalog = generateCatalog();
let catalogCache = fullCatalog;

/** 目前規模的模擬目錄（50/200/500 共用同一產生順序的前綴）。 */
export function currentCatalog(): ScanModule[] {
  return catalogCache;
}

/** 切換模擬目錄規模；僅影響記憶體，重新整理即回復預設 200。 */
export function setCatalogSize(size: number): ScanModule[] {
  catalogCache = fullCatalog.slice(0, size);
  return catalogCache;
}

export function slot(index: number, instanceId: number, bypassed = false): RackSlot {
  const module = catalogCache[index % catalogCache.length];
  return { instanceId, name: module.classes[0].name, pluginPath: module.path,
    classId: module.classes[0].uid, bypassed, monitorBypassed: false,
    latencySamples: 64, effectiveLatencySamples: 64, monitorLatencySamples: 64,
    runtimeState: 'active', monitorState: 'active', availability: 'ok', params: [] };
}

const common = { source: null, output: null, gain: 1, mute: false, metered: true, plugins: [] };
export const initialTracks: Track[] = [
  { ...common, trackId: 1, kind: 'audio', name: '麥克風', color: 0x65b6dc,
    source: { type: 'asioIn', channel: 0, mono: true }, dests: [4, 5], plugins: [slot(0, 101), slot(1, 102)] },
  { ...common, trackId: 2, kind: 'app', name: '音樂播放', color: 0x9e8fda,
    source: { type: 'app', pid: 100, name: 'Music Player' }, dests: [4, 5], gain: 0.65, plugins: [slot(3, 103, true)] },
  { ...common, trackId: 3, kind: 'fx', name: '空間效果', color: 0xc89874,
    dests: [4, 5], gain: 0.35, mute: true, plugins: [slot(2, 104)] },
  { ...common, trackId: 4, kind: 'output', systemRole: 'monitor', name: '監聽輸出', color: 0x73b6a0,
    dests: [], output: { type: 'asioOut', channel: 0 }, latencyPolicy: 'lowLatency' },
  { ...common, trackId: 5, kind: 'output', systemRole: 'stream', name: '串流輸出', color: 0x78a5d2,
    dests: [], output: { type: 'wasapi', deviceId: 'prototype-output' }, latencyPolicy: 'fullPdc', plugins: [slot(5, 105)] },
];

export const devices: DeviceInfo[] = [{
  deviceKey: 'prototype-asio', name: 'Studio Audio（模擬）', maxIn: 2, maxOut: 2,
  sampleRates: [44100, 48000], currentSampleRate: 48000, minBufferSize: 64,
  maxBufferSize: 1024, preferredBufferSize: 256, bufferSizes: [64, 128, 256, 512, 1024],
  inputNames: ['Input 1', 'Input 2'], outputNames: ['Monitor L', 'Monitor R'],
}];

export const initialStatus: EngineStatus = {
  running: true, deviceKey: devices[0].deviceKey, sampleRate: 48000, bufferSize: 256,
  inputLatency: 256, outputLatency: 256, xruns: 0, trackCount: 5, pluginFails: 0,
  revision: 1, latencyGeneration: 1, pluginDelay: { monitorSamples: 128, streamSamples: 192 },
  telemetryStrips: [], tracks: initialTracks, error: null,
};
