// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import App from "../../App.svelte";
import type { EngineStatus } from "../types";

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
const originalAnimate = Element.prototype.animate;
const originalShowModal = HTMLDialogElement.prototype.showModal;
const originalClose = HTMLDialogElement.prototype.close;
beforeEach(() => {
  status = { running: true, deviceKey: "wasapi", sampleRate: 48000, bufferSize: 128,
    inputLatency: 0, outputLatency: 128, xruns: 0, trackCount: 0, pluginFails: 0,
    revision: 0, telemetryStrips: [], tracks: [], error: null };
  command.mockReset().mockImplementation(async (name) => {
    if (name === "get_snapshot") return { snapshot: { status, lastScan: [], capabilities: [] } };
    if (name === "list_devices") return { devices: [] };
    if (name === "load_session") return { deviceKey: "wasapi", bufferSize: 999, sampleRate: 44100, revision: 0, missing: [] };
    if (name === "stop" || name === "start") {
      status = { ...status, running: name === "start", error: null }; return status;
    }
    return { revision: 0, jobId: 1 };
  });
  const settings = { startupMode: "blank", lastWorkingDevice: "wasapi", lastWorkingBuffer: null,
    checkUpdatesOnStartup: false, startMinimizedOnAutostart: false };
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

it.each([false, true])("ASIO 清單空白，running=%s 載入 WASAPI Session 會透過佇列啟動", async (running) => {
  component = mount(App, { target: document.body }); await settle();
  listeners.get("engine-event")!({ payload: { kind: "status", payload: { ...status, running } } });
  await settle(); command.mockClear();
  button("場景 / Session").click(); await settle();
  button("載入 Session").click(); await settle();
  expect(command.mock.calls.filter(([name]) => name === "start" || name === "stop").map(([name]) => name))
    .toEqual(running ? ["stop", "start"] : ["start"]);
  expect(command).toHaveBeenCalledWith("start", { deviceKey: "wasapi", sampleRate: null, bufferSize: null });
});

it("主裝置失效會顯示通知，手動 Start 使用 WASAPI 格式並可再次停止", async () => {
  component = mount(App, { target: document.body }); await settle(); command.mockClear();
  status = { ...status, running: false, deviceKey: null, bufferSize: null,
    error: "WASAPI output device lost; reconnect the device and press Start" };
  listeners.get("engine-event")!({ payload: { kind: "status", payload: status } });
  await settle();
  expect(command.mock.calls.some(([name]) => name === "start")).toBe(false);
  expect(document.querySelector(".capsules")?.textContent).toContain("音訊已停止");
  status = { ...status, deviceKey: "wasapi", bufferSize: 128 };
  button("Start").click(); await settle();
  expect(command).toHaveBeenCalledWith("start", { deviceKey: "wasapi", sampleRate: null, bufferSize: null });
  expect(button("Stop").disabled).toBe(false);
});

it("Session 載入失敗不啟動或停止目前串流", async () => {
  component = mount(App, { target: document.body }); await settle(); command.mockClear();
  command.mockRejectedValueOnce({ code: "session_io", message: "invalid session" });
  button("場景 / Session").click(); await settle();
  button("載入 Session").click(); await settle();
  expect(command).toHaveBeenCalledExactlyOnceWith("load_session", { path: "test.rmsession" });
  expect(document.querySelector('.capsules [role="alert"]')?.textContent).toContain("Session 載入失敗");
});

it("設定分頁支援方向鍵、首尾跳轉與單一 Tab 入口，面板標題隨選取同步", async () => {
  component = mount(App, { target: document.body }); await settle();
  document.querySelector<HTMLButtonElement>('[aria-label="設定"]')!.click(); await settle();
  const dialog = document.querySelector<HTMLDialogElement>(".settingsdlg")!;
  expect(dialog.open).toBe(true);
  expect(document.getElementById(dialog.getAttribute("aria-labelledby")!)?.textContent).toBe("設定");
  button("音訊").focus();
  for (const [key, id] of [["ArrowRight", "general"], ["ArrowRight", "scenes"], ["End", "about"],
    ["ArrowRight", "audio"], ["ArrowLeft", "about"], ["Home", "audio"]]) {
    document.activeElement!.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }));
    await settle();
    const selected = document.querySelector<HTMLButtonElement>('[role="tab"][aria-selected="true"]')!;
    expect(selected.id).toBe(`settings-tab-${id}`);
    expect(document.activeElement).toBe(selected);
    expect(selected.tabIndex).toBe(0);
    expect(document.querySelectorAll('[role="tab"][tabindex="0"]')).toHaveLength(1);
    expect(document.querySelector('[role="tabpanel"]')?.getAttribute("aria-labelledby")).toBe(selected.id);
    expect(selected.getAttribute("aria-controls")).toBe("settings-panel");
  }
  button("場景 / Session").click(); await settle();
  expect(button("儲存 Session").closest('[role="tabpanel"]')).not.toBeNull();
});

it.each([false, true])("空工作區正確說明 WASAPI 與引擎狀態 running=%s，不宣稱整條路徑就緒", async (running) => {
  component = mount(App, { target: document.body }); await settle();
  listeners.get("engine-event")!({ payload: { kind: "status", payload: { ...status, running } } });
  await settle();
  const card = document.querySelector(".startcard")!;
  expect(card.textContent).toContain("系統音訊 (WASAPI)");
  expect(card.textContent).toContain(running ? "引擎執行中" : "引擎未啟動");
  expect(card.textContent).toContain("輸出軌尚未建立");
  expect(card.textContent).not.toMatch(/無 ASIO 裝置|已就緒/);
  button("場景 / Session").click(); await settle();
  button("設定音訊…").click(); await settle();
  expect(document.querySelector<HTMLDialogElement>(".settingsdlg")!.open).toBe(true);
  expect(button("音訊").getAttribute("aria-selected")).toBe("true");
});

it("已建立系統輸出仍提示確認裝置與接收端電平", async () => {
  component = mount(App, { target: document.body }); await settle();
  const tracks: EngineStatus["tracks"] = ["monitor", "stream"].map((role, i) => ({
    trackId: i + 1, kind: "output", name: role, systemRole: role as "monitor" | "stream",
    dests: [], plugins: [], source: null, output: null, gain: 1, mute: false, color: 0,
  }));
  listeners.get("engine-event")!({ payload: { kind: "status", payload: { ...status, tracks, trackCount: 2 } } });
  await settle();
  expect(document.querySelector(".startcard")?.textContent).toContain("輸出軌已建立");
  expect(document.querySelector(".startcard")?.textContent).toContain("接收程式檢查電平");
  expect(document.querySelector(".startcard")?.textContent).not.toContain("已就緒");
});
