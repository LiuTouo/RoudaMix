// @vitest-environment jsdom
// 響度分析 = 獨立 OS 視窗:App 負責開窗 + view 投影(emitTo),LoudnessWindow
// 負責渲染 + 釘選/重置/關閉回投。兩組各測一半,組合即原 popover 測試語意。
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import App from "../../App.svelte";
import LoudnessWindow from "../LoudnessWindow.svelte";
import type { EngineStatus, Track } from "../types";
import type { LoudnessReport } from "../loudness";

const { command, invoke, listeners, emit, emitTo, panelWindows, alwaysOnTopCalls, windowCloseCount, dragCount } =
  vi.hoisted(() => ({
    command: vi.fn(),
    invoke: vi.fn(),
    listeners: new Map<string, (e: { payload: unknown }) => void>(),
    emit: vi.fn(async () => {}),
    emitTo: vi.fn(async () => {}),
    panelWindows: new Map<string, {
      label: string; options: Record<string, unknown>;
      focused: number; closed: number; topmost: boolean[]; closeHandlers: Array<() => void>;
    }>(),
    alwaysOnTopCalls: [] as boolean[],
    windowCloseCount: { n: 0 },
    dragCount: { n: 0 },
  }));
vi.mock("../protocol-commands.generated", async (original) => ({
  ...await original<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/app", () => ({ getVersion: vi.fn(async () => "test") }));
vi.mock("@tauri-apps/api/event", () => ({
  listen: vi.fn(async (name: string, cb: (e: { payload: unknown }) => void) => {
    listeners.set(name, cb); return () => listeners.delete(name);
  }),
  emit,
  emitTo,
}));
vi.mock("@tauri-apps/api/window", () => ({
  getCurrentWindow: () => ({
    onCloseRequested: async () => () => {},
    outerPosition: async () => ({ x: 0, y: 0 }),
    innerSize: async () => ({ width: 1100, height: 820 }),
    setAlwaysOnTop: async (v: boolean) => { alwaysOnTopCalls.push(v); },
    close: async () => { windowCloseCount.n++; },
    startDragging: async () => { dragCount.n++; },
    scaleFactor: async () => 1.25,
    outerPosition: async () => ({ x: 0, y: 0 }),
    innerSize: async () => ({ width: 2400, height: 1320 }),
  }),
}));
vi.mock("@tauri-apps/api/webview", () => ({ getCurrentWebview: () => ({ setZoom: vi.fn(async () => {}) }) }));
vi.mock("@tauri-apps/api/webviewWindow", () => ({
  // 最小可測 WebviewWindow:記錄建窗選項、聚焦/關閉/置頂呼叫與 close-requested 回呼
  WebviewWindow: class {
    label: string; options: Record<string, unknown>;
    focused = 0; closed = 0; topmost: boolean[] = [];
    closeHandlers: Array<() => void> = [];
    constructor(label: string, options: Record<string, unknown>) {
      this.label = label; this.options = options;
      panelWindows.set(label, this);
    }
    async setFocus() { this.focused++; }
    async close() { this.closed++; }
    async setAlwaysOnTop(v: boolean) { this.topmost.push(v); }
    once() { return () => {}; }
    async onCloseRequested(cb: () => void) { this.closeHandlers.push(cb); return () => {}; }
    static async getByLabel(label: string) { return panelWindows.get(label) ?? null; }
  },
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(async () => "test.rmsession"), save: vi.fn() }));
vi.mock("@tauri-apps/plugin-autostart", () => ({ isEnabled: vi.fn(async () => false), enable: vi.fn(), disable: vi.fn() }));
vi.mock("@tauri-apps/plugin-global-shortcut", () => ({ register: vi.fn(), unregister: vi.fn() }));
vi.mock("@tauri-apps/plugin-updater", () => ({ check: vi.fn(async () => null) }));

let component: ReturnType<typeof mount> | undefined;
let status: EngineStatus;
let capabilities: string[];
let current: LoudnessReport | null;
let nextId: number;
const track = (trackId: number, name: string, systemRole?: "monitor" | "stream"): Track => ({
  trackId, name, kind: systemRole ? "output" : "audio", systemRole, color: 0x88bbcc,
  source: null, output: null, dests: [], plugins: [], gain: 1, mute: false,
});
function newReport(trackId: number): LoudnessReport {
  return { measurementId: `measure-${++nextId}`, trackId, state: "measuring", error: null, elapsedSeconds: 0,
    momentaryLufs: null, shortTermLufs: null, integratedLufs: null, truePeakDbtp: null, loudnessRangeLu: null };
}
beforeEach(() => {
  vi.useFakeTimers();
  capabilities = ["loudnessAnalysisV1"];
  status = { running: true, deviceKey: "wasapi", sampleRate: 48000, bufferSize: 128,
    inputLatency: 0, outputLatency: 128, xruns: 0, trackCount: 3, pluginFails: 0,
    revision: 0, telemetryStrips: [], tracks: [track(1, "麥克風"), track(2, "監聽", "monitor"), track(3, "串流", "stream")], error: null };
  current = null; nextId = 0;
  panelWindows.clear(); alwaysOnTopCalls.length = 0; windowCloseCount.n = 0; dragCount.n = 0;
  emit.mockClear(); emitTo.mockClear(); listeners.clear();
  command.mockReset().mockImplementation(async (kind, payload) => {
    if (kind === "get_snapshot") return { snapshot: { status, lastScan: [], capabilities } };
    if (kind === "list_devices") return { devices: [] };
    if (kind === "start_loudness") { current = newReport(payload.trackId); return { report: current }; }
    if (kind === "reset_loudness") { current = newReport(current!.trackId); return { report: current }; }
    if (kind === "get_loudness_report") return { report: current };
    if (kind === "stop_loudness") { if (payload.measurementId === current?.measurementId) current = null; return {}; }
    if (kind === "load_session") return { deviceKey: null, revision: 0, missing: [] };
    if (kind === "track_set") return { tracks: status.tracks };
    if (kind === "start" || kind === "stop") { status = { ...status, running: kind === "start" }; return status; }
    return { revision: 0, jobId: 1 };
  });
  const settings = { startupMode: "blank", lastWorkingDevice: "wasapi", lastWorkingBuffer: null,
    checkUpdatesOnStartup: false, startMinimizedOnAutostart: false };
  invoke.mockReset().mockImplementation(async (name) => name === "connect_status"
    ? { connected: true, epoch: 1, engineVersion: "test" } : { settings, warnings: [] });
  vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} unobserve() {} });
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
  vi.spyOn(HTMLElement.prototype, "getBoundingClientRect").mockReturnValue(new DOMRect(0, 0, 1024, 340));
  Object.defineProperty(Element.prototype, "animate", { configurable: true, value: vi.fn(() => ({
    cancel() {}, finish() {}, currentTime: 0, finished: Promise.resolve(), effect: null,
  })) });
});
afterEach(async () => {
  if (component) await unmount(component);
  component = undefined;
  await settle();
  document.body.replaceChildren(); listeners.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals(); vi.useRealTimers();
  Reflect.deleteProperty(Element.prototype, "animate");
});
async function settle() { for (let i = 0; i < 30; i++) { await tick(); flushSync(); } }
async function show() { component = mount(App, { target: document.body }); await settle(); }
const more = (index = 0) => document.querySelectorAll<HTMLButtonElement>(".meter-more")[index];
const starts = () => command.mock.calls.filter(([kind]) => kind === "start_loudness");
const panel = () => panelWindows.get("loudness");
const viewPushes = () => emitTo.mock.calls.filter(([, event]) => event === "loudness-panel:view");
/** 模擬面板視窗掛起後向主視窗發 ready,觸發主視窗補推 view */
function panelReady() { listeners.get("loudness-panel:ready")!({ payload: undefined }); }
function emitStatus(patch: Partial<EngineStatus>) {
  status = { ...status, ...patch };
  listeners.get("engine-event")!({ payload: { kind: "status", payload: status } });
}

// ---- App:開窗/切軌/清理(視窗生命週期與 view 投影)----

it("opens one panel window per click, focuses it when reopened on the same track", async () => {
  await show(); more().click(); await settle();
  const win = panel();
  expect(win).toBeDefined();
  expect(win!.options.url).toBe("/?window=loudness");
  expect(win!.options.decorations).toBe(false);
  expect(win!.options.skipTaskbar).toBe(true);
  // DPI 換算：125% 下 physical 中線左側換回 logical(2400/2-440×1.25-15) / 1.25
  expect(win!.options.x).toBe(Math.round((2400 / 2 - 440 * 1.25 - 12 * 1.25) / 1.25));
  expect(win!.options.y).toBe(Math.round((1320 - 580 * 1.25) / 2 / 1.25));
  expect(starts()).toHaveLength(1);
  panelReady(); await settle();
  const push = viewPushes().at(-1)!;
  expect(push[1]).toBe("loudness-panel:view");
  expect(push[0]).toBe("loudness");
  expect(push[2]).toMatchObject({ trackName: "麥克風", trackKind: "audio", running: true });
  // 同軌再點 = 聚焦既有視窗,不重開、不重量測
  more().click(); await settle();
  expect(panel()).toBe(win);
  expect(win!.focused).toBe(1);
  expect(starts()).toHaveLength(1);
});

it("switching tracks reconfigures the measurement and pushes the new track", async () => {
  await show(); more().click(); await settle(); panelReady(); await settle();
  more(1).click(); await settle();
  expect(starts()).toHaveLength(2);
  const push = viewPushes().at(-1)!;
  expect(push[2]).toMatchObject({ trackName: "監聽", trackKind: "output" });
});

it("closing from the panel window stops the measurement", async () => {
  await show(); more().click(); await settle();
  panel()!.closeHandlers[0](); await settle();
  expect(command).toHaveBeenCalledWith("stop_loudness", { measurementId: "measure-1" });
  expect(panel()!.closed).toBeGreaterThanOrEqual(1);
});

it.each(["remove", "disconnect", "load"])("cleans up on %s", async (action) => {
  await show(); more().click(); await settle();
  if (action === "remove") emitStatus({ tracks: status.tracks.slice(1) });
  else if (action === "disconnect") listeners.get("connection")!({ payload: { connected: false, epoch: 1, engineVersion: "test" } });
  else {
    [...document.querySelectorAll("button")].find((button) => button.textContent?.trim() === "Session")!.click();
    await settle();
    [...document.querySelectorAll("button")].find((button) => button.textContent?.trim() === "載入 Session")!.click();
  }
  await settle();
  expect(panel()!.closed).toBeGreaterThanOrEqual(1);
  expect(command).toHaveBeenCalledWith("stop_loudness", { measurementId: "measure-1" });
});

it("keeps the level-bar icon decorative and the full button named and keyboard-operable", async () => {
  await show();
  for (const button of document.querySelectorAll<HTMLButtonElement>(".meter-more")) {
    expect(button.getAttribute("aria-label")).toContain("響度分析");
    expect(button.tabIndex).toBe(0);
    expect(button.querySelector("span")?.getAttribute("aria-hidden")).toBe("true");
    expect(button.querySelector("svg")?.getAttribute("focusable")).toBe("false");
    const bars = [...button.querySelectorAll("svg rect")];
    expect(bars.map((bar) => bar.getAttribute("height"))).toEqual(["4", "10", "7"]);
    expect(bars.every((bar) => Number(bar.getAttribute("y")) + Number(bar.getAttribute("height")) === 13)).toBe(true);
  }
});

it("old engines disable the extra entry without disabling existing meter right-click", async () => {
  capabilities = []; await show();
  expect(more().disabled).toBe(true);
  more().click(); await settle();
  const context = new MouseEvent("contextmenu", { bubbles: true, cancelable: true });
  document.querySelector("canvas")!.dispatchEvent(context); await settle();
  expect(context.defaultPrevented).toBe(true);
  expect(starts()).toHaveLength(0);
  expect(panel()).toBeUndefined();
});

// ---- LoudnessWindow:渲染 + 釘選/重置/關閉回投 ----

function mountPanel() {
  component = mount(LoudnessWindow, { target: document.body });
  return component;
}
function pushView(payload: {
  view: { report: LoudnessReport | null; busy?: boolean; error?: string; restarted?: boolean };
  trackName: string; trackKind: string; running: boolean;
}) {
  listeners.get("loudness-panel:view")!({ payload: {
    view: { report: null, busy: false, error: "", restarted: false, ...payload.view },
    trackName: payload.trackName, trackKind: payload.trackKind, running: payload.running,
  } });
}

it("announces itself ready on mount and renders a pushed view", async () => {
  mountPanel(); await settle();
  expect(emit).toHaveBeenCalledWith("loudness-panel:ready");
  pushView({ view: { report: newReport(1) }, trackName: "麥克風", trackKind: "audio", running: true });
  await settle();
  const text = document.body.textContent ?? "";
  for (const expected of ["麥克風", "LUFS-M", "LUFS-S", "LUFS-I", "最高 True Peak", "響度範圍（LRA）",
    "成品輸出參考", "Apple Podcasts", "−16 ±1 LUFS", "Spotify 音樂", "−14 LUFS", "≤ −1 dBTP", "M／S、LRA 不設目標"]) {
    expect(text).toContain(expected);
  }
});

it("shows live numbers, freezes when stopped, and honours the measurement point by track kind", async () => {
  mountPanel(); await settle();
  const report = { ...newReport(1), elapsedSeconds: 12, momentaryLufs: -15.2,
    integratedLufs: -17.1, truePeakDbtp: 1.2, loudnessRangeLu: 4.2 };
  pushView({ view: { report }, trackName: "麥克風", trackKind: "audio", running: true });
  await settle();
  expect(document.querySelector('[data-metric="momentaryLufs"]')?.textContent).toBe("-15.2");
  expect(document.querySelector('[data-metric="truePeakDbtp"]')?.textContent).toContain("1.2");
  expect(document.querySelector("time")?.textContent).toBe("00:12");
  expect(document.body.textContent).toContain("插件、音量與靜音控制之後");
  pushView({ view: { report: { ...report, state: "stopped" } }, trackName: "麥克風", trackKind: "audio", running: false });
  await settle();
  expect(document.body.textContent).toContain("已停止 · 數值凍結");
  pushView({ view: { report }, trackName: "監聽", trackKind: "output", running: true });
  await settle();
  expect(document.body.textContent).toContain("實際輸出（含監聽）");
  expect(document.body.textContent).toContain("監聽");
});

it("pushes every polled measurement to the panel instead of freezing after ready", async () => {
  await show(); more().click(); await settle(); panelReady(); await settle();
  const before = viewPushes().length;
  current = { ...current!, elapsedSeconds: 5, momentaryLufs: -12.3 };
  vi.advanceTimersByTime(200); await settle();
  vi.advanceTimersByTime(200); await settle();
  expect(viewPushes().length).toBeGreaterThan(before);
  expect(viewPushes().at(-1)![2]).toMatchObject({ view: { report: { elapsedSeconds: 5, momentaryLufs: -12.3 } } });
});

it("reset button reports back to the main window and is gated on running", async () => {
  mountPanel(); await settle();
  pushView({ view: { report: newReport(1) }, trackName: "麥克風", trackKind: "audio", running: false });
  await settle();
  const reset = document.querySelector<HTMLButtonElement>('[aria-label="重置響度量測"]')!;
  expect(reset.disabled).toBe(true);
  pushView({ view: { report: newReport(1) }, trackName: "麥克風", trackKind: "audio", running: true });
  await settle();
  reset.click(); await settle();
  expect(emit).toHaveBeenCalledWith("loudness-panel:reset");
});

it("pin toggles always-on-top and detaches from the main window owner", async () => {
  mountPanel(); await settle();
  const pin = document.querySelector<HTMLButtonElement>('[aria-label="釘選視窗置頂"]')!;
  pin.click(); await settle();
  expect(alwaysOnTopCalls).toEqual([true]);
  expect(invoke).toHaveBeenCalledWith("set_loudness_owner", { owned: false });
  expect(pin.getAttribute("aria-pressed")).toBe("true");
  document.querySelector<HTMLButtonElement>('[aria-label="取消視窗置頂"]')!.click(); await settle();
  expect(alwaysOnTopCalls).toEqual([true, false]);
  expect(invoke).toHaveBeenCalledWith("set_loudness_owner", { owned: true });
});

it("Esc and the close button close the panel window", async () => {
  mountPanel(); await settle();
  window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", cancelable: true }));
  await settle();
  document.querySelector<HTMLButtonElement>('[aria-label="關閉響度分析"]')!.click();
  await settle();
  expect(windowCloseCount.n).toBe(2);
});

it("pointer-down on blank areas drags the window; buttons do not", async () => {
  mountPanel(); await settle();
  pushView({ view: { report: newReport(1) }, trackName: "麥克風", trackKind: "audio", running: true });
  await settle();
  document.querySelector<HTMLElement>(".status")!.dispatchEvent(
    new MouseEvent("pointerdown", { bubbles: true, cancelable: true, button: 0 }));
  await settle();
  expect(dragCount.n).toBe(1);
  document.querySelector<HTMLButtonElement>('[aria-label="釘選視窗置頂"]')!.dispatchEvent(
    new MouseEvent("pointerdown", { bubbles: true, cancelable: true, button: 0 }));
  await settle();
  expect(dragCount.n).toBe(1);  // 控制鈕不觸發拖曳
});
