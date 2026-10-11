// @vitest-environment jsdom
import capability from "../../../../src-tauri/capabilities/default.json";
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import App from "../../App.svelte";
import type { AppSettings } from "../ipc";

const { invoke, setZoom } = vi.hoisted(() => ({ invoke: vi.fn(), setZoom: vi.fn() }));
vi.mock("@tauri-apps/api/core", () => ({ invoke }));
vi.mock("@tauri-apps/api/app", () => ({ getVersion: vi.fn(async () => "0.1.0") }));
vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
vi.mock("@tauri-apps/api/window", () => ({
  getCurrentWindow: () => ({ onCloseRequested: async () => () => {} }),
}));
vi.mock("@tauri-apps/api/webview", () => ({ getCurrentWebview: () => ({ setZoom }) }));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(), save: vi.fn() }));
vi.mock("@tauri-apps/plugin-autostart", () => ({
  isEnabled: vi.fn(async () => false), enable: vi.fn(), disable: vi.fn(),
}));
vi.mock("@tauri-apps/plugin-global-shortcut", () => ({ register: vi.fn(), unregister: vi.fn() }));
vi.mock("@tauri-apps/plugin-updater", () => ({ check: vi.fn(async () => null) }));

let component: ReturnType<typeof mount> | undefined;
let settings: AppSettings;
let saveError: Error | undefined;
const originalAnimate = Element.prototype.animate;
const originalShowModal = HTMLDialogElement.prototype.showModal;
const originalClose = HTMLDialogElement.prototype.close;
beforeEach(() => {
  settings = { startupMode: "blank", sessionDir: null, startupFile: null,
    lastSessionPath: null, startMinimizedOnAutostart: false, checkUpdatesOnStartup: false };
  saveError = undefined;
  setZoom.mockReset().mockResolvedValue(undefined);
  invoke.mockReset().mockImplementation(async (command, args) => {
    if (command === "get_settings") return { settings: { ...settings }, warnings: [] };
    if (command === "set_settings") {
      if (saveError) throw saveError;
      Object.assign(settings, args.patch);
      return { settings: { ...settings }, warnings: [] };
    }
    // 引擎一直未完成連線也必須能載入與套用介面偏好。
    if (command === "connect_status") return new Promise(() => {});
    return undefined;
  });
  vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} unobserve() {} });
  HTMLDialogElement.prototype.showModal = function () { this.open = true; };
  HTMLDialogElement.prototype.close = function () { this.open = false; };
  Element.prototype.animate = vi.fn(() => ({ cancel() {}, finish() {}, currentTime: 0,
    finished: Promise.resolve(), effect: null })) as unknown as typeof Element.prototype.animate;
});
afterEach(async () => {
  if (component) await unmount(component);
  component = undefined;
  document.body.replaceChildren();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  if (originalAnimate) Element.prototype.animate = originalAnimate;
  else Reflect.deleteProperty(Element.prototype, "animate");
  if (originalShowModal) HTMLDialogElement.prototype.showModal = originalShowModal;
  else Reflect.deleteProperty(HTMLDialogElement.prototype, "showModal");
  if (originalClose) HTMLDialogElement.prototype.close = originalClose;
  else Reflect.deleteProperty(HTMLDialogElement.prototype, "close");
});

async function settle() { for (let i = 0; i < 20; ++i) { await tick(); flushSync(); } }
function button(text: string) {
  const found = [...document.querySelectorAll("button")].find(b => b.textContent?.trim() === text);
  expect(found).toBeDefined();
  return found!;
}
async function openGeneral() {
  component = mount(App, { target: document.body });
  await settle();
  document.querySelector<HTMLButtonElement>('[aria-label="設定"]')!.click();
  await settle();
  button("通用").click();
  await settle();
  return document.querySelector<HTMLInputElement>("#uiscale")!;
}
function change(slider: HTMLInputElement, value: number, type = "input") {
  slider.value = String(value);
  slider.dispatchEvent(new Event(type, { bubbles: true }));
}
function savedPatches() {
  return invoke.mock.calls.filter(([command]) => command === "set_settings").map(([, args]) => args.patch);
}

it("主視窗有原生 WebView 縮放權限", () => {
  expect(capability.windows).toContain("main");
  expect(capability.permissions).toContain("core:webview:allow-set-webview-zoom");
});

it.each([undefined, 75, 125, 150])("啟動時套用 %s，舊設定預設 100%，不等待音訊引擎", async (scale) => {
  settings.uiScale = scale;
  const slider = await openGeneral();
  expect(setZoom).toHaveBeenCalledWith((scale ?? 100) / 100);
  expect(slider.value).toBe(String(scale ?? 100));
  expect(slider.min).toBe("75");
  expect(slider.max).toBe("150");
  expect(slider.step).toBe("5");
  expect(slider.labels?.[0].textContent).toBe("介面縮放");
  expect(slider.getAttribute("aria-valuetext")).toBe(`${scale ?? 100}%`);
  expect(savedPatches()).toEqual([]);
});

it("拖動即時預覽，change 才寫入，重新掛載還原比例，按鈕可回到 100%", async () => {
  const slider = await openGeneral();
  change(slider, 125);
  await settle();
  expect(setZoom).toHaveBeenLastCalledWith(1.25);
  expect(document.querySelector('output[for="uiscale"]')?.textContent).toBe("125%");
  expect(savedPatches()).toEqual([]);
  change(slider, 125, "change");
  await settle();
  expect(savedPatches()).toEqual([{ uiScale: 125 }]);
  expect(settings.startupMode).toBe("blank");

  await unmount(component!);
  document.body.replaceChildren();
  const restored = await openGeneral();
  expect(restored.value).toBe("125");
  expect(setZoom).toHaveBeenLastCalledWith(1.25);
  button("還原 100%").click();
  await settle();
  expect(restored.value).toBe("100");
  expect(setZoom).toHaveBeenLastCalledWith(1);
  expect(savedPatches().at(-1)).toEqual({ uiScale: 100 });
});

it("快速拖動合併等待中的縮放，不讓較早回覆覆蓋最後比例", async () => {
  const slider = await openGeneral();
  setZoom.mockClear();
  let release!: () => void;
  setZoom.mockImplementationOnce(() => new Promise<void>(resolve => { release = resolve; }));
  change(slider, 110);
  change(slider, 125);
  change(slider, 150);
  change(slider, 150, "change");
  await settle();
  expect(setZoom).toHaveBeenCalledTimes(1);
  expect(slider.getAttribute("aria-valuetext")).toBe("150%");
  release();
  await settle();
  expect(setZoom.mock.calls).toEqual([[1.1], [1.5]]);
  expect(slider.value).toBe("150");
  expect(savedPatches()).toEqual([{ uiScale: 150 }]);
});

it("套用失敗保留上一次成功比例，不寫入設定，並在對話框內提示", async () => {
  const slider = await openGeneral();
  setZoom.mockRejectedValueOnce(new Error("zoom denied"));
  change(slider, 125, "change");
  await settle();
  expect(slider.value).toBe("100");
  expect(slider.getAttribute("aria-valuetext")).toBe("100%");
  expect(savedPatches()).toEqual([]);
  expect(document.querySelector('.settingsdlg [role="alert"]')?.textContent).toContain("介面縮放套用失敗");
});

it("同一比例的舊預覽失敗不會回滾較新的 change 請求", async () => {
  const slider = await openGeneral();
  setZoom.mockRejectedValueOnce(new Error("temporary failure"));
  change(slider, 125);
  change(slider, 125, "change");
  await settle();
  expect(slider.value).toBe("125");
  expect(setZoom).toHaveBeenLastCalledWith(1.25);
  expect(savedPatches()).toEqual([{ uiScale: 125 }]);
  expect(document.querySelector('.settingsdlg [role="alert"]')).toBeNull();
});

it("儲存失敗保留目前預覽、提示重啟沿用舊值，並能再次調整", async () => {
  settings.uiScale = 100;
  const slider = await openGeneral();
  saveError = new Error("disk full");
  change(slider, 150, "change");
  await settle();
  expect(slider.value).toBe("150");
  expect(settings.uiScale).toBe(100);
  expect(document.querySelector('.settingsdlg [role="alert"]')?.textContent).toContain("下次啟動沿用舊值");
  saveError = undefined;
  change(slider, 125, "change");
  await settle();
  expect(settings.uiScale).toBe(125);
  expect(document.querySelector('.settingsdlg [role="alert"]')).toBeNull();
});
