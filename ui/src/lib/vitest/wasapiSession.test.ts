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
vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async (name, cb) => {
  listeners.set(name, cb); return () => listeners.delete(name);
}) }));
vi.mock("@tauri-apps/api/window", () => ({ getCurrentWindow: () => ({ onCloseRequested: async () => () => {} }) }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(async () => "test.rmsession"), save: vi.fn() }));
vi.mock("@tauri-apps/plugin-autostart", () => ({ isEnabled: vi.fn(async () => false), enable: vi.fn(), disable: vi.fn() }));

let component: ReturnType<typeof mount>;
let status: EngineStatus;
const originalAnimate = Element.prototype.animate;
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
  Element.prototype.animate = vi.fn(() => ({ cancel() {}, finish() {}, currentTime: 0,
    finished: Promise.resolve(), effect: null })) as unknown as typeof Element.prototype.animate;
});
afterEach(async () => {
  if (component) await unmount(component);
  document.body.replaceChildren(); listeners.clear(); vi.restoreAllMocks(); vi.unstubAllGlobals();
  if (originalAnimate) Element.prototype.animate = originalAnimate;
  else Reflect.deleteProperty(Element.prototype, "animate");
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
  button("場景").click(); await settle(); // 儲存/載入 Session 已移至設定 → 場景分頁
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
  button("場景").click(); await settle(); // 儲存/載入 Session 已移至設定 → 場景分頁
  button("載入 Session").click(); await settle();
  expect(command).toHaveBeenCalledExactlyOnceWith("load_session", { path: "test.rmsession" });
  expect(document.querySelector(".capsules")?.textContent).toContain("Session 載入失敗");
});
