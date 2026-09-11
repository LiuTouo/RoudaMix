// 暫用原型的 Tauri 邊界：所有回覆來自記憶體，不呼叫桌面、設備或檔案系統。
import type { MetersFrame, Snapshot, Track } from '../lib/types';
import type { AppSettings } from '../lib/ipc';
import { catalog, devices, initialStatus, slot } from './fixtures';

const handlers = new Map<string, Set<(event: { payload: any }) => void>>();
const status = structuredClone(initialStatus);
let settings: AppSettings = { startupMode: 'blank', sessionDir: null, startupFile: null,
  lastSessionPath: null, closeBehavior: 'tray', startMinimizedOnAutostart: false };
let autostart = false;
let jobId = 0;
let scanTimer: ReturnType<typeof setTimeout> | undefined;
let nextTrack = 6;
let nextPlugin = 106;
let lastAction = '固定範例已載入；所有操作僅供預覽';

function emit(name: string, payload: unknown) {
  for (const handler of handlers.get(name) ?? []) handler({ payload: structuredClone(payload) });
}

export function previewNotice(message: string) {
  lastAction = message;
  window.dispatchEvent(new CustomEvent('prototype-state', { detail: inspectPreview() }));
}

function snapshot(): Snapshot {
  status.trackCount = status.tracks.length;
  status.telemetryStrips = status.tracks.flatMap((track) => [
    { id: 0, kind: 1, trackId: track.trackId, instanceId: null },
    ...track.plugins.map((plugin) => ({ id: 0, kind: 2, trackId: track.trackId, instanceId: plugin.instanceId })),
  ]).map((identity, id) => ({ ...identity, id }));
  return structuredClone({ epoch: 1, engineVersion: 'prototype', status, tracks: status.tracks,
    telemetryStrips: status.telemetryStrips, lastScan: catalog, capabilities: ['pluginLatencyPdcV1'] });
}

export function inspectPreview() {
  return { mode: '記憶體模擬', lastAction, settings: structuredClone(settings), autostart,
    snapshot: snapshot() };
}

function changed(action: string) {
  status.revision = (status.revision ?? 0) + 1;
  status.latencyGeneration = (status.latencyGeneration ?? 0) + 1;
  emit('engine-snapshot', snapshot());
  previewNotice(action);
}

export async function listen<T>(name: string, handler: (event: { payload: T }) => void) {
  if (!handlers.has(name)) handlers.set(name, new Set());
  handlers.get(name)!.add(handler);
  return () => { handlers.get(name)?.delete(handler); };
}

export async function invoke<T>(command: string, args: Record<string, any> = {}): Promise<T> {
  let result: unknown;
  if (command === 'connect_status') result = { connected: true, epoch: 1, engineVersion: 'prototype', phase: 'connected' };
  else if (command === 'get_settings') result = { settings, warnings: [] };
  else if (command === 'set_settings') {
    settings = { ...settings, ...args.patch };
    result = { settings, warnings: [] };
    previewNotice('設定已更新（僅記憶體）');
  } else if (command === 'list_sessions') result = [];
  else if (command === 'engine_command') result = engine(args.kind, args.payload ?? {});
  else if (command === 'quit_app' || command === 'respawn_engine') previewNotice('預覽不會退出程式或重新啟動真實引擎');
  else throw { code: 'prototype_unsupported', message: `預覽尚未模擬：${command}` };
  return structuredClone(result) as T;
}

function engine(kind: string, payload: Record<string, any>): unknown {
  const track = status.tracks.find((item) => item.trackId === payload.trackId);
  const owner = status.tracks.find((item) => item.plugins.some((plugin) => plugin.instanceId === payload.instanceId));
  const plugin = owner?.plugins.find((item) => item.instanceId === payload.instanceId);
  switch (kind) {
    case 'ping': return { engineVersion: 'prototype' };
    case 'get_snapshot': return { snapshot: snapshot() };
    case 'list_devices': return { devices };
    case 'list_audio_apps': return { apps: [{ pid: 100, name: 'Music Player', path: 'C:/Prototype/MusicPlayer.exe' }] };
    case 'list_capture_devices': return { devices: [{ id: 'prototype-input', name: '麥克風（模擬）', default: true, sampleRate: 48000 }] };
    case 'list_render_devices': return { devices: [{ id: 'prototype-output', name: '串流裝置（模擬）', default: true, sampleRate: 48000 }] };
    case 'ensure_system_outputs': return { tracks: status.tracks, revision: status.revision };
    case 'get_latency_report': return { report: {
      generation: status.latencyGeneration, ok: true, error: '', bufferBytes: 4096,
      outputs: status.tracks.filter((t) => t.kind === 'output').map((t) => ({ trackId: t.trackId,
        totalPluginDelaySamples: 128, compensationDelaySamples: 64, synchronized: true })),
      edges: [], tracks: status.tracks,
    } };
    case 'start_scan': {
      clearTimeout(scanTimer);
      const currentJob = ++jobId;
      scanTimer = setTimeout(() => emit('engine-event', { kind: 'scan_done',
        payload: { jobId: currentJob, plugins: catalog, failed: [] } }), 180);
      return { jobId: currentJob, reused: false };
    }
    case 'cancel_scan':
      clearTimeout(scanTimer);
      setTimeout(() => emit('engine-event', { kind: 'scan_cancelled', payload: { jobId } }), 0);
      return { jobId, cancelling: true };
    case 'start':
      status.running = true;
      status.deviceKey = payload.deviceKey;
      status.bufferSize = payload.bufferSize ?? 256;
      changed('音訊啟動狀態已模擬');
      return status;
    case 'stop':
      status.running = false;
      changed('音訊停止狀態已模擬');
      return status;
    case 'open_device_panel':
      previewNotice('硬體面板屬於桌面設備功能，瀏覽器預覽不會開啟');
      setTimeout(() => emit('engine-event', { kind: 'devices_changed', payload: {} }), 0);
      return { panel: false };
    case 'track_set':
      if (track) for (const key of ['name', 'color', 'gain', 'mute'] as const) {
        if (payload[key] !== undefined) Object.assign(track, { [key]: payload[key] });
      }
      break;
    case 'track_set_source': if (track) track.source = payload.source; break;
    case 'track_set_dests': if (track) track.dests = payload.dests; break;
    case 'track_set_output': if (track) track.output = payload.output; break;
    case 'track_set_output_latency_policy': if (track) track.latencyPolicy = payload.policy; break;
    case 'track_add': {
      const added: Track = { trackId: nextTrack++, kind: payload.kind, name: payload.name ?? '新軌道',
        color: payload.color ?? 0x78a5d2, source: null, dests: [4, 5], output: null,
        gain: 1, mute: false, metered: true, plugins: [] };
      status.tracks.push(added);
      changed('已加入模擬軌道');
      return { trackId: added.trackId, tracks: status.tracks };
    }
    case 'track_remove':
      status.tracks = status.tracks.filter((item) => item.trackId !== payload.trackId || item.systemRole);
      for (const item of status.tracks) item.dests = item.dests.filter((id) => status.tracks.some((t) => t.trackId === id));
      break;
    case 'track_move': {
      const index = status.tracks.findIndex((item) => item.trackId === payload.trackId);
      if (index >= 0) status.tracks.splice(payload.newIndex, 0, status.tracks.splice(index, 1)[0]);
      break;
    }
    case 'add_plugin': {
      if (!track) throw { code: 'track_not_found', message: '找不到模擬軌道' };
      const index = catalog.findIndex((item) => item.path === payload.path);
      if (index < 0) throw { code: 'plugin_not_found', message: '找不到模擬插件' };
      const added = slot(index, nextPlugin++);
      track.plugins.push(added);
      changed('已加入模擬插件');
      return { instanceId: added.instanceId, trackId: track.trackId, tracks: status.tracks };
    }
    case 'remove_plugin': if (owner) owner.plugins = owner.plugins.filter((p) => p.instanceId !== payload.instanceId); break;
    case 'move_plugin':
      if (owner && plugin) {
        owner.plugins.splice(owner.plugins.indexOf(plugin), 1);
        owner.plugins.splice(payload.newIndex, 0, plugin);
      }
      break;
    case 'set_bypass': if (plugin) plugin.bypassed = payload.bypassed; break;
    case 'set_monitor_bypass': if (plugin) plugin.monitorBypassed = payload.bypassed; break;
    case 'get_params': return { instanceId: payload.instanceId, params: [] };
    case 'open_editor':
      previewNotice('VST 原生編輯器僅能在桌面版開啟；目前為風格預覽');
      return { instanceId: payload.instanceId, editor: false };
    case 'close_editor': case 'set_editor_owner': return {};
    default:
      previewNotice(`此操作不在風格預覽範圍：${kind}`);
      throw { code: 'prototype_unsupported', message: `僅供外觀比較，未執行 ${kind}` };
  }
  changed(`已模擬 ${kind}`);
  return { tracks: status.tracks };
}

export async function open() { previewNotice('檔案／資料夾選擇僅供桌面版使用，預覽不讀取本機檔案'); return null; }
export async function save() { previewNotice('預覽不會儲存 Session 或建立檔案'); return null; }
export async function enable() { autostart = true; previewNotice('自動啟動已勾選（僅模擬）'); }
export async function disable() { autostart = false; previewNotice('自動啟動已取消（僅模擬）'); }
export async function isEnabled() { return autostart; }
export function getCurrentWindow() {
  return { onCloseRequested: async () => () => {}, hide: async () => previewNotice('預覽不會操作桌面視窗') };
}

export function startPreviewMeters() {
  let sequence = 0;
  const timer = setInterval(() => {
    const table = snapshot().telemetryStrips;
    const frame: MetersFrame = { sequence: sequence++, xruns: 0, callbackLoad: 0.12,
      sampleRate: status.sampleRate, bufferSize: status.bufferSize ?? 256,
      inputLatency: 256, outputLatency: 256,
      strips: table.map((identity) => {
        const track = status.tracks.find((t) => t.trackId === identity.trackId)!;
        const level = status.running && !track.mute ? 0.34 * track.gain : 0;
        return { instanceId: identity.instanceId ?? identity.trackId!, kind: identity.kind,
          peakL: level, peakR: level * 0.88, rmsL: level * 0.65, rmsR: level * 0.59 };
      }),
      pluginLoads: status.tracks.flatMap((t) => t.plugins.map((p) => ({ instanceId: p.instanceId, variant: 0 as const, processLoad: 0.032 }))),
      spectrum: Array.from({ length: 128 }, (_, i) => status.running ? -35 - i * 0.32 + 9 * Math.sin(i * 0.35) : -120),
    };
    emit('meters', frame);
  }, 100);
  return () => { clearInterval(timer); clearTimeout(scanTimer); handlers.clear(); };
}
