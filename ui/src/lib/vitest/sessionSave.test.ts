// @vitest-environment jsdom
// 儲存流程:有目前檔案 → 直接覆寫(不開系統對話框);沒有檔案 → 只開一次存檔對話框。
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
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(), save: vi.fn() }));
vi.mock("@tauri-apps/plugin-autostart", () => ({ isEnabled: vi.fn(async () => false), enable: vi.fn(), disable: vi.fn() }));

let component: ReturnType<typeof mount>;
let settings: Record<string, unknown>;
let writeApproved: boolean;
let pickedPath: string | null;
const originalAnimate = Element.prototype.animate;
const originalShowModal = HTMLDialogElement.prototype.showModal;
const originalClose = HTMLDialogElement.prototype.close;

beforeEach(() => {
  writeApproved = false;
  pickedPath = null;
  settings = { startupMode: "blank", lastSessionPath: null, lastWorkingDevice: "wasapi",
    lastWorkingBuffer: null, checkUpdatesOnStartup: false, startMinimizedOnAutostart: false };
  const status: EngineStatus = { running: true, deviceKey: "wasapi", sampleRate: 48000, bufferSize: 128,
    inputLatency: 0, outputLatency: 128, xruns: 0, trackCount: 0, pluginFails: 0,
    revision: 0, telemetryStrips: [], tracks: [], error: null };
  command.mockReset().mockImplementation(async (name) => {
    if (name === "get_snapshot") return { snapshot: { status, lastScan: [], capabilities: [] } };
    if (name === "list_devices") return { devices: [] };
    if (name === "save_session") return { revision: 7 };
    return { revision: 0, jobId: 1 };
  });
  invoke.mockReset().mockImplementation(async (name) => {
    if (name === "connect_status") return { connected: true, epoch: 1, engineVersion: "test" };
    if (name === "session_write_approved") return writeApproved;
    if (name === "pick_session_save") return pickedPath;
    return { settings, warnings: [] };
  });
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
  else Reflect.deleteProperty(Element.prototype, "showModal");
  if (originalClose) HTMLDialogElement.prototype.close = originalClose;
  else Reflect.deleteProperty(Element.prototype, "close");
});
async function settle() { for (let i = 0; i < 30; ++i) { await tick(); flushSync(); } }
function ctrlS() { window.dispatchEvent(new KeyboardEvent("keydown", { key: "s", ctrlKey: true })); }
const called = (name: string) => command.mock.calls.filter(([n]) => n === name);
const invoked = (name: string) => invoke.mock.calls.filter(([n]) => n === name);

it("尚未存過檔:第一次儲存開一次選檔對話框,並存到選定的路徑", async () => {
  pickedPath = "C:\\mix\\new.rmsession";
  component = mount(App, { target: document.body }); await settle();
  ctrlS(); await settle();
  expect(invoked("pick_session_save")).toHaveLength(1);
  expect(called("save_session")).toHaveLength(1);
  expect(called("save_session")[0]![1]).toMatchObject({ path: pickedPath });
});

it("有目前檔案且已核准寫入:再按儲存直接覆寫,不開對話框", async () => {
  pickedPath = "C:\\mix\\new.rmsession";
  component = mount(App, { target: document.body }); await settle();
  ctrlS(); await settle();
  writeApproved = true;
  command.mockClear(); invoke.mockClear();
  ctrlS(); await settle();
  expect(invoked("pick_session_save")).toHaveLength(0);
  expect(invoked("session_write_approved")).toHaveLength(1);
  expect(called("save_session")[0]![1]).toMatchObject({ path: pickedPath });
});
