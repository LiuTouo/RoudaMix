// @vitest-environment jsdom
// 診斷:設定 → 快捷鍵分頁,擷取模式按下組合鍵是否寫入 monitorHotkey。
import { afterEach, beforeAll, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, unmount } from "svelte";
import App from "../../App.svelte";

// jsdom 未實作 <dialog> modal API 與 ResizeObserver
beforeAll(() => {
  HTMLDialogElement.prototype.showModal = function () { this.open = true; };
  HTMLDialogElement.prototype.close = function () { this.open = false; };
  (globalThis as Record<string, unknown>).ResizeObserver = class {
    observe() {} unobserve() {} disconnect() {}
  };
});

const mocks = vi.hoisted(() => ({
  invoke: vi.fn(),
  listen: vi.fn(),
  register: vi.fn(),
  unregister: vi.fn(),
  getCurrentWindow: vi.fn(),
}));
vi.mock("@tauri-apps/api/core", () => ({ invoke: mocks.invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen: mocks.listen }));
vi.mock("@tauri-apps/api/window", () => ({
  getCurrentWindow: () => ({
    onCloseRequested: vi.fn().mockResolvedValue(vi.fn()),
    hide: vi.fn().mockResolvedValue(undefined),
  }),
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn(), save: vi.fn() }));
vi.mock("@tauri-apps/plugin-autostart", () => ({
  enable: vi.fn(), disable: vi.fn(), isEnabled: vi.fn().mockResolvedValue(false),
}));
vi.mock("@tauri-apps/plugin-global-shortcut", () => ({
  register: mocks.register.mockResolvedValue(undefined),
  unregister: mocks.unregister.mockResolvedValue(undefined),
}));
vi.mock("@tauri-apps/plugin-updater", () => ({ check: vi.fn().mockResolvedValue(null) }));

let component: ReturnType<typeof mount>;
const settle = async () => { for (let i = 0; i < 8; i++) { await Promise.resolve(); flushSync(); } };

const settings = { schemaVersion: 3, monitorHotkey: null, closeBehavior: "exit", startupMode: "blank", checkUpdatesOnStartup: true };

beforeEach(() => {
  vi.clearAllMocks();
  mocks.listen.mockResolvedValue(vi.fn());
  mocks.invoke.mockImplementation((command: string, args?: Record<string, unknown>) => {
    if (command === "get_settings")
      return Promise.resolve({ settings: { ...settings }, warnings: [] });
    if (command === "set_settings") {
      Object.assign(settings, (args as { patch: object }).patch);
      return Promise.resolve({ settings: { ...settings }, warnings: [] });
    }
    if (command === "connect_status")
      return Promise.resolve({ connected: false, epoch: 0, engineVersion: "" });
    return Promise.resolve(undefined);
  });
});
afterEach(async () => { await unmount(component); document.body.innerHTML = ""; });

function press(key: string, code: string, mods: { ctrl?: boolean; alt?: boolean; shift?: boolean; meta?: boolean } = {}) {
  window.dispatchEvent(
    new KeyboardEvent("keydown", { key, code, bubbles: true, cancelable: true,
      ctrlKey: !!mods.ctrl, altKey: !!mods.alt, shiftKey: !!mods.shift, metaKey: !!mods.meta }),
  );
}

function enterCaptureMode() {
  return [...document.querySelectorAll("button")].find((b) => b.textContent === "未設定");
}

it("無修飾鍵組合被拒時應顯示原因並維持擷取模式(不再無聲)", async () => {
  component = mount(App, { target: document.body });
  await settle();
  // 齒輪鈕是純 icon:以 aria-label(無障礙名稱)定位
  [...document.querySelectorAll("button")].find((b) => b.getAttribute("aria-label") === "設定")!.click();
  await settle();
  [...document.querySelectorAll(".tabs button")].find((b) => b.textContent === "快捷鍵")!.click();
  await settle();
  enterCaptureMode()!.click();
  await settle();

  press("m", "KeyM"); // 純字母:安全規則拒絕
  await settle();
  expect(document.body.textContent).toContain("無法綁定");
  expect([...document.querySelectorAll("button")].some((b) => b.textContent!.includes("按下快捷鍵"))).toBe(true);

  press("Control", "ControlLeft"); // 純修飾鍵:組合中途,不加提示也不採計
  await settle();
  expect(mocks.invoke.mock.calls.some((c) => c[0] === "set_settings")).toBe(false);
});

it("擷取模式按下 Ctrl+M 應寫入 monitorHotkey", async () => {
  component = mount(App, { target: document.body });
  await settle();

  // 開設定 → 快捷鍵分頁
  const settingsBtn = [...document.querySelectorAll("button")].find((b) => b.getAttribute("aria-label")?.includes("設定") || b.textContent === "設定");
  settingsBtn!.click();
  await settle();
  const tabBtn = [...document.querySelectorAll(".tabs button")].find((b) => b.textContent === "快捷鍵");
  expect(tabBtn, "快捷鍵分頁按鈕應存在").toBeTruthy();
  tabBtn!.click();
  await settle();

  const capture = [...document.querySelectorAll("button")].find((b) => b.textContent === "未設定");
  expect(capture, "擷取按鈕(未設定)應存在").toBeTruthy();
  capture!.click();
  await settle();
  expect([...document.querySelectorAll("button")].some((b) => b.textContent!.includes("按下快捷鍵")), "應進入擷取模式").toBe(true);

  press("m", "KeyM", { ctrl: true });
  await settle();

  expect(mocks.invoke).toHaveBeenCalledWith("set_settings", expect.objectContaining({}));
  const setCall = mocks.invoke.mock.calls.find((c) => c[0] === "set_settings");
  const patch = setCall?.[1]?.patch ?? setCall?.[1];
  expect(JSON.stringify(patch)).toContain("Ctrl+M");
  const shown = [...document.querySelectorAll("button")].map((b) => b.textContent).join("|");
  expect(shown).toContain("Ctrl+M");
});
