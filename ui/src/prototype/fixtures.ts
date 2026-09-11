// 暫用原型：固定資料讓四個版本能在相同內容下比較。
import type { DeviceInfo, EngineStatus, RackSlot, ScanModule, Track } from '../lib/types';

export const catalog: ScanModule[] = [
  ['Studio EQ', 'Rouda Audio', 'Fx|EQ'],
  ['Voice Compressor', 'Rouda Audio', 'Fx|Dynamics'],
  ['Room Reverb', 'Northern Sound', 'Fx|Reverb'],
  ['Tape Saturation', 'Northern Sound', 'Fx|Distortion'],
  ['Stereo Delay', 'Signal Works', 'Fx|Delay'],
  ['Output Limiter', 'Signal Works', 'Fx|Dynamics'],
  ['超長名稱的插件 — Studio Channel Strip Professional Edition', 'Studio', 'Fx|Channel Strip|EQ|Dynamics'],
  ['Utility', '', ''],
].map(([name, vendor, subcategories], i) => ({
  path: `C:/Prototype/VST3/${i + 1}.vst3`,
  classes: [{ uid: `prototype-${i + 1}`, name, vendor, subcategories, version: '1.0' }],
}));

export function slot(index: number, instanceId: number, bypassed = false): RackSlot {
  const module = catalog[index];
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
