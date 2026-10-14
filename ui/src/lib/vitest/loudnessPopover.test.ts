// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import App from "../../App.svelte";
import type { EngineStatus, Track } from "../types";
import type { LoudnessReport } from "../loudness";

const { command, invoke, listeners } = vi.hoisted(() => ({
  command: vi.fn(), invoke: vi.fn(), listeners: new Map<string, (e: { payload: unknown }) => void>(),
}));
vi.mock("../protocol-commands.generated", async (original) => ({
  ...await original<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/app", () => ({ getVersion: vi.fn(async () => "test") }));
vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async (name, cb) => {
  listeners.set(name, cb); return () => listeners.delete(name);
}) }));
vi.mock("@tauri-apps/api/window", () => ({ getCurrentWindow: () => ({ onCloseRequested: async () => () => {} }) }));
vi.mock("@tauri-apps/api/webview", () => ({ getCurrentWebview: () => ({ setZoom: vi.fn(async () => {}) }) }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(async () => "test.rmsession"), save: vi.fn() }));
vi.mock("@tauri-apps/plugin-autostart", () => ({ isEnabled: vi.fn(async () => false), enable: vi.fn(), disable: vi.fn() }));
vi.mock("@tauri-apps/plugin-global-shortcut", () => ({ register: vi.fn(), unregister: vi.fn() }));
vi.mock("@tauri-apps/plugin-updater", () => ({ check: vi.fn(async () => null) }));

let component: ReturnType<typeof mount> | undefined;
let status: EngineStatus;
let capabilities: string[];
let current: LoudnessReport | null;
let nextId: number;
let anchorRect: DOMRect;
const frames = new Map<number, FrameRequestCallback>();
const observers: Array<{ target?: Element; disconnected: boolean; callback: (entries: Array<{ isIntersecting: boolean }>) => void }> = [];
const originalShow = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "showPopover");
const originalHide = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "hidePopover");
const originalAnimate = Object.getOwnPropertyDescriptor(Element.prototype, "animate");
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
  current = null; nextId = 0; frames.clear(); observers.length = 0;
  anchorRect = new DOMRect(200, 100, 46, 330);
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
  vi.stubGlobal("IntersectionObserver", class {
    item: (typeof observers)[number];
    constructor(callback: (entries: Array<{ isIntersecting: boolean }>) => void) {
      this.item = { disconnected: false, callback }; observers.push(this.item);
    }
    observe(target: Element) { this.item.target = target; }
    disconnect() { this.item.disconnected = true; }
  });
  let frameId = 0;
  vi.stubGlobal("requestAnimationFrame", (callback: FrameRequestCallback) => { frames.set(++frameId, callback); return frameId; });
  vi.stubGlobal("cancelAnimationFrame", (id: number) => frames.delete(id));
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
  vi.spyOn(HTMLElement.prototype, "getBoundingClientRect").mockImplementation(function () {
    if (this.classList.contains("meterwrap")) return anchorRect;
    return new DOMRect(0, 0, this.classList.contains("loudness-popup") ? 440 : 1024, 340);
  });
  Object.defineProperty(HTMLElement.prototype, "showPopover", { configurable: true, value() { this.dataset.open = "true"; } });
  Object.defineProperty(HTMLElement.prototype, "hidePopover", { configurable: true, value() { delete this.dataset.open; } });
  Object.defineProperty(Element.prototype, "animate", { configurable: true, value: vi.fn(() => ({
    cancel() {}, finish() {}, currentTime: 0, finished: Promise.resolve(), effect: null,
  })) });
});
afterEach(async () => {
  if (component) await unmount(component);
  component = undefined;
  await settle();
  document.body.replaceChildren(); listeners.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals(); vi.useRealTimers();
  for (const [prototype, name, descriptor] of [
    [HTMLElement.prototype, "showPopover", originalShow], [HTMLElement.prototype, "hidePopover", originalHide],
    [Element.prototype, "animate", originalAnimate],
  ] as const) {
    if (descriptor) Object.defineProperty(prototype, name, descriptor);
    else Reflect.deleteProperty(prototype, name);
  }
});
async function settle() { for (let i = 0; i < 30; i++) { await tick(); flushSync(); } }
async function show() { component = mount(App, { target: document.body }); await settle(); }
const popup = () => document.querySelector<HTMLElement>("#loudness-analysis");
const more = (index = 0) => document.querySelectorAll<HTMLButtonElement>(".meter-more")[index];
const starts = () => command.mock.calls.filter(([kind]) => kind === "start_loudness");
function emitStatus(patch: Partial<EngineStatus>) {
  status = { ...status, ...patch };
  listeners.get("engine-event")!({ payload: { kind: "status", payload: status } });
}
function frame() {
  const pending = [...frames.values()]; frames.clear();
  for (const callback of pending) callback(performance.now());
}

it("ellipsis and meter open the same unique persistent panel without resetting it", async () => {
  await show();
  more().click(); await settle();
  expect(popup()?.getAttribute("popover")).toBe("manual");
  expect(popup()?.textContent).toContain("麥克風");
  expect(popup()?.textContent).toContain("LUFS-M");
  expect(popup()?.textContent).toContain("LUFS-S");
  expect(popup()?.textContent).toContain("LUFS-I");
  expect(popup()?.textContent).toContain("最高 True Peak");
  expect(popup()?.textContent).toContain("響度範圍（LRA）");
  expect(document.activeElement).toBe(popup());
  document.querySelector<HTMLButtonElement>(".meter-trigger")!.click(); await settle();
  expect(starts()).toHaveLength(1);
  document.body.click();
  document.querySelector<HTMLInputElement>('.slotwrap input')!.dispatchEvent(new Event("input", { bubbles: true }));
  await settle();
  expect(popup()).not.toBeNull();
  more(1).click(); await settle();
  expect(document.querySelectorAll("#loudness-analysis")).toHaveLength(1);
  expect(popup()?.textContent).toContain("監聽");
  expect(popup()?.textContent).toContain("實際輸出（含監聽）");
  expect(starts()).toHaveLength(2);
  expect(observers[0].disconnected).toBe(true);
  expect(observers.at(-1)?.target).toBe(more(1).parentElement);
});

it("preserves right-click clipping reset, Esc closes and restores focus", async () => {
  await show();
  const context = new MouseEvent("contextmenu", { bubbles: true, cancelable: true });
  document.querySelector("canvas")!.dispatchEvent(context);
  await settle();
  expect(context.defaultPrevented).toBe(true);
  expect(starts()).toHaveLength(0);
  more().focus(); more().click(); await settle();
  const esc = new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true });
  window.dispatchEvent(esc); await settle();
  expect(popup()).toBeNull();
  expect(document.activeElement).toBe(more());
  expect(command).toHaveBeenCalledWith("stop_loudness", { measurementId: "measure-1" });
});

it("shows live numbers and reset returns to warmup without changing the Session", async () => {
  await show(); more().click(); await settle();
  current = { ...current!, elapsedSeconds: 12, momentaryLufs: -15.2, shortTermLufs: -16.4,
    integratedLufs: -17.1, truePeakDbtp: 1.2, loudnessRangeLu: 4.2 };
  await vi.advanceTimersByTimeAsync(200); await settle();
  expect(popup()?.querySelector('[data-metric="momentaryLufs"]')?.textContent).toBe("-15.2");
  expect(popup()?.querySelector('[data-metric="truePeakDbtp"]')?.textContent).toContain("1.2");
  expect(popup()?.querySelector("time")?.textContent).toBe("00:12");
  popup()!.querySelector<HTMLButtonElement>('[aria-label="重置響度量測"]')!.click(); await settle();
  expect(popup()?.querySelector('[data-metric="integratedLufs"]')?.textContent).toBe("—");
  expect(popup()?.querySelector("time")?.textContent).toBe("00:00");
  expect(command).toHaveBeenCalledWith("reset_loudness", { measurementId: "measure-1" });
  expect(status.revision).toBe(0);
});

it("shows purpose-specific finished-mix references without changing measured values or gain", async () => {
  await show(); more().click(); await settle();
  const reference = popup()!.querySelector(".reference")!;
  expect(reference.textContent).toContain("成品輸出參考");
  expect(reference.textContent).toContain("LUFS-I");
  expect(reference.textContent).toContain("Apple Podcasts");
  expect(reference.textContent).toContain("−16 ±1 LUFS");
  expect(reference.textContent).toContain("Spotify 音樂");
  expect(reference.textContent).toContain("−14 LUFS");
  expect(reference.textContent).toContain("≤ −1 dBTP");
  expect(reference.textContent).toContain("低於 −2 dBTP");
  expect(reference.textContent).toContain("非單軌目標");
  expect(reference.textContent).toContain("M／S、LRA 不設固定值");
  current = { ...current!, elapsedSeconds: 12, integratedLufs: -9, truePeakDbtp: 2 };
  await vi.advanceTimersByTimeAsync(200); await settle();
  expect(popup()?.querySelector('[data-metric="integratedLufs"]')?.textContent).toBe("-9.0");
  expect(popup()?.querySelector('[data-metric="truePeakDbtp"]')?.textContent).toContain("2.0");
  expect(command.mock.calls.some(([kind]) => kind === "track_set")).toBe(false);
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
  more().click(); document.querySelector<HTMLButtonElement>(".meter-trigger")!.click();
  const context = new MouseEvent("contextmenu", { bubbles: true, cancelable: true });
  document.querySelector("canvas")!.dispatchEvent(context); await settle();
  expect(context.defaultPrevented).toBe(true);
  expect(starts()).toHaveLength(0);
  expect(popup()).toBeNull();
});

it("can open while stopped, freezes a stopped report, and restarts while open", async () => {
  await show(); emitStatus({ running: false }); await settle(); more().click(); await settle();
  expect(popup()?.textContent).toContain("引擎未啟動");
  expect(starts()).toHaveLength(0);
  emitStatus({ running: true }); await settle();
  current = { ...current!, elapsedSeconds: 6, integratedLufs: -19 };
  emitStatus({ running: false }); await settle();
  expect(popup()?.textContent).toContain("已停止 · 數值凍結");
  expect(popup()?.querySelector('[data-metric="integratedLufs"]')?.textContent).toBe("-19.0");
  emitStatus({ running: true, sampleRate: 96000 }); await settle();
  expect(starts()).toHaveLength(2);
  expect(popup()?.textContent).toContain("量測已重置");
});

it.each(["remove", "disconnect", "load"])("cleans up on %s", async (action) => {
  await show(); more().click(); await settle();
  if (action === "remove") emitStatus({ tracks: status.tracks.slice(1) });
  else if (action === "disconnect") listeners.get("connection")!({ payload: { connected: false, epoch: 1, engineVersion: "test" } });
  else {
    [...document.querySelectorAll("button")].find((button) => button.textContent?.trim() === "場景 / Session")!.click();
    await settle();
    [...document.querySelectorAll("button")].find((button) => button.textContent?.trim() === "載入 Session")!.click();
  }
  await settle();
  expect(popup()).toBeNull();
  expect(command).toHaveBeenCalledWith("stop_loudness", { measurementId: "measure-1" });
});

it("flips at viewport edge, follows anchor geometry, and stops when clipped out", async () => {
  anchorRect = new DOMRect(940, 600, 46, 330);
  await show(); more().click(); await settle();
  expect(Number.parseFloat(popup()!.style.left)).toBeLessThan(anchorRect.left);
  expect(Number.parseFloat(popup()!.style.top) + 340).toBeLessThanOrEqual(window.innerHeight - 8);
  anchorRect = new DOMRect(100, 40, 46, 330);
  frame(); await settle();
  expect(popup()!.style.left).toBe("154px");
  expect(popup()!.style.top).toBe("40px");
  observers.at(-1)!.callback([{ isIntersecting: false }]); await settle();
  expect(popup()).toBeNull();
  expect(current).toBeNull();
});

it("does not consume Esc already handled by another dialog", async () => {
  await show(); more().click(); await settle();
  const dialog = document.createElement("dialog"); dialog.open = true; document.body.append(dialog);
  window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true }));
  await settle(); expect(popup()).not.toBeNull();
  dialog.remove();
  const handled = new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true });
  handled.preventDefault(); window.dispatchEvent(handled);
  await settle(); expect(popup()).not.toBeNull();
});
