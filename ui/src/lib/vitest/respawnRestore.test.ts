// @vitest-environment jsdom
// 引擎換代(崩潰重生)救援:換代救走本代 session 路徑,新代首個權威 status 為 0 軌
// 時升為還原膠囊,點擊以該路徑 load_session;engine 存活的瞬斷重連不出膠囊。
// 注意:膠囊 out:fly 在 jsdom 的 outro 不會完成(節點殘留),因此不斷言「消失」,
// 只斷言「出現」與「點擊後的命令」。
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import App from "../../App.svelte";
import { capsuleItems } from "../capsules";
import type { EngineStatus, Track } from "../types";

const { command, invoke, listeners } = vi.hoisted(() => ({
  command: vi.fn(), invoke: vi.fn(), listeners: new Map<string, (e: { payload: unknown }) => void>(),
}));
vi.mock("../protocol-commands.generated", async (original) => ({
  ...await original<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/webview", () => ({
  getCurrentWebview: () => ({ setZoom: vi.fn().mockResolvedValue(undefined) }),
}));
vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async (name, cb) => {
  listeners.set(name, cb); return () => listeners.delete(name);
}) }));
vi.mock("@tauri-apps/api/window", () => ({ getCurrentWindow: () => ({ onCloseRequested: async () => () => {} }) }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(async () => "test.rmsession"), save: vi.fn() }));
vi.mock("@tauri-apps/plugin-autostart", () => ({ isEnabled: vi.fn(async () => false), enable: vi.fn(), disable: vi.fn() }));

let component: ReturnType<typeof mount>;
let status: EngineStatus;
let settings: Record<string, unknown>;
const originalAnimate = Element.prototype.animate;
const originalShowModal = HTMLDialogElement.prototype.showModal;
const originalClose = HTMLDialogElement.prototype.close;
beforeEach(() => {
  status = { running: true, deviceKey: "wasapi", sampleRate: 48000, bufferSize: 128,
    inputLatency: 0, outputLatency: 128, xruns: 0, trackCount: 0, pluginFails: 0,
    revision: 0, telemetryStrips: [], tracks: [], error: null };
  settings = { startupMode: "blank", lastSessionPath: "C:\\mix\\show.rmsession",
    lastWorkingDevice: "wasapi", lastWorkingBuffer: null,
    checkUpdatesOnStartup: false, startMinimizedOnAutostart: false };
  command.mockReset().mockImplementation(async (name) => {
    if (name === "get_snapshot") return { snapshot: { status, lastScan: [], capabilities: [] } };
    if (name === "list_devices") return { devices: [] };
    if (name === "load_session") return { deviceKey: "wasapi", bufferSize: 999, sampleRate: 44100, revision: 0, missing: [] };
    if (name === "stop" || name === "start") {
      status = { ...status, running: name === "start" }; return status;
    }
    return { revision: 0, jobId: 1 };
  });
  invoke.mockReset().mockImplementation(async (name) => name === "connect_status"
    ? { connected: true, epoch: 1, engineVersion: "test" } : { settings, warnings: [] });
  vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} unobserve() {} });
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
  HTMLDialogElement.prototype.showModal = function () { this.open = true; };
  HTMLDialogElement.prototype.close = function () {
    this.open = false;
    this.dispatchEvent(new Event("close"));
  };
  Element.prototype.animate = vi.fn(() => ({ cancel() {}, finish() {}, currentTime: 0,
    finished: Promise.resolve(), effect: null })) as unknown as typeof Element.prototype.animate;
});
afterEach(async () => {
  if (component) await unmount(component);
  document.body.replaceChildren(); listeners.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals();
  if (originalAnimate) Element.prototype.animate = originalAnimate;
  else Reflect.deleteProperty(Element.prototype, "animate");
  if (originalShowModal) HTMLDialogElement.prototype.showModal = originalShowModal;
  else Reflect.deleteProperty(HTMLDialogElement.prototype, "showModal");
  if (originalClose) HTMLDialogElement.prototype.close = originalClose;
  else Reflect.deleteProperty(HTMLDialogElement.prototype, "close");
});
async function settle() { for (let i = 0; i < 30; ++i) { await tick(); flushSync(); } }
function button(text: string) {
  const found = [...document.querySelectorAll("button")].find(b => b.textContent?.trim() === text);
  expect(found).toBeDefined(); return found!;
}
const track = (trackId: number, kind: Track["kind"]): Track => ({
  trackId, kind, name: `t${trackId}`, dests: [], plugins: [], source: null, output: null,
  gain: 1, mute: false, color: 0,
});
/** 模擬崩潰換代:兩次 connection 事件(spawn 計數 +1)+ 新 engine 快照 */
async function crashAndRespawn(snapshotStatus: EngineStatus) {
  listeners.get("connection")!({ payload: { connected: true, epoch: 1, bootGen: 1, engineVersion: "test" } });
  await settle();
  listeners.get("connection")!({ payload: { connected: true, epoch: 1, bootGen: 2, engineVersion: "test" } });
  await settle();
  listeners.get("engine-snapshot")!({ payload: { status: snapshotStatus, lastScan: [], capabilities: [] } });
  await settle();
}

it("capsuleItems:只有帶救回路徑才產生還原膠囊(名稱取自路徑尾段)", () => {
  const base = { notices: [], audioStale: false, audioStaleDismissed: false,
    audioStaleDetail: "", restoreError: "", missing: [] };
  expect(capsuleItems({ ...base, respawnRestorePath: null })).toHaveLength(0);
  const c = capsuleItems({ ...base, respawnRestorePath: "C:\\mix\\show.rmsession" })[0]!;
  expect(c.key).toBe("respawnRestore");
  expect(c.action).toBe("respawnRestore");
  expect(c.closable).toBe(true);
  expect(c.msg).toContain("show.rmsession");
});

it("引擎換代救走本代 session,新代空快照升為膠囊,點擊以該路徑還原", async () => {
  component = mount(App, { target: document.body }); await settle();
  // 建立本代 context:起始區「最近」載入 → currentSessionPath 指向該檔
  button("最近:show.rmsession").click(); await settle();
  expect(command).toHaveBeenCalledWith("load_session", { path: "C:\\mix\\show.rmsession", deferBypassed: false });
  command.mockClear();
  await crashAndRespawn(status);
  const capsule = document.querySelector(".capsule-body");
  expect(capsule?.textContent).toContain("引擎已重啟");
  capsule!.click(); await settle();
  expect(command).toHaveBeenCalledWith("load_session", { path: "C:\\mix\\show.rmsession", deferBypassed: false });
});

it("engine 存活的瞬斷重連(快照與自動 restart 回覆都帶活軌道)不出還原膠囊", async () => {
  component = mount(App, { target: document.body }); await settle();
  button("最近:show.rmsession").click(); await settle();
  // engine 存活:重連後自動 restart 的回覆與快照都帶著原本的軌道
  status = { ...status, tracks: [track(1, "audio"), track(2, "output")] };
  await crashAndRespawn(status);
  expect(document.querySelector(".capsules")?.textContent ?? "").not.toContain("引擎已重啟");
});
