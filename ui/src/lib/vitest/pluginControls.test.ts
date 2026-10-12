// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import PluginTransferHarness from "./PluginTransferHarness.svelte";
import type { RackSlot, Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (original) => ({
  ...await original<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));
const originalShowModal = HTMLDialogElement.prototype.showModal;
const originalClose = HTMLDialogElement.prototype.close;
let component: ReturnType<typeof mount> | undefined;
const makeTrack = (overrides: Partial<RackSlot> = {}): Track => ({
  trackId: 1, name: "麥克風", kind: "audio", source: null, output: null,
  color: 0, gain: 1, mute: false, dests: [], plugins: [{
    instanceId: 44, name: "FabFilter Pro-Q 3", pluginPath: "eq.vst3", classId: "eq", params: [],
    bypassed: false, monitorBypassed: false, availability: "ok", ...overrides,
  }],
});
function show(overrides: Partial<RackSlot> = {}, latencyEnabled = true) {
  component = mount(PluginTransferHarness, { target: document.body, props: {
    initialTracks: [makeTrack(overrides)], latencyEnabled,
  } });
  flushSync();
}
const row = () => document.querySelector<HTMLElement>(".plug")!;
const badge = () => document.querySelector<HTMLElement>(".monitor-badge");
const checkbox = () => document.querySelector<HTMLButtonElement>('[role="menuitemcheckbox"]')!;
function openMenu() {
  row().focus();
  row().dispatchEvent(new KeyboardEvent("keydown", { key: "F10", shiftKey: true, bubbles: true, cancelable: true }));
  flushSync();
}
function menuItem(label: string) {
  return [...document.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')].find(item => item.textContent?.trim() === label)!;
}
async function settle() { await tick(); await Promise.resolve(); flushSync(); }
function closeMenu() {
  document.querySelector(".ctxdlg")!.dispatchEvent(new Event("cancel", { cancelable: true }));
  flushSync();
}

beforeEach(() => {
  command.mockReset().mockResolvedValue({});
  HTMLDialogElement.prototype.showModal = function () { this.open = true; };
  HTMLDialogElement.prototype.close = function () { this.open = false; };
  vi.spyOn(console, "info").mockImplementation(() => {});
});
afterEach(async () => {
  if (component) await unmount(component);
  component = undefined;
  document.body.replaceChildren();
  vi.restoreAllMocks();
  if (originalShowModal) HTMLDialogElement.prototype.showModal = originalShowModal;
  else Reflect.deleteProperty(HTMLDialogElement.prototype, "showModal");
  if (originalClose) HTMLDialogElement.prototype.close = originalClose;
  else Reflect.deleteProperty(HTMLDialogElement.prototype, "close");
});

it("插件列保留旁通和編輯入口，MB 與移除在鍵盤可達的右鍵選單", async () => {
  show();
  expect(row().querySelectorAll("button")).toHaveLength(1);
  expect(row().querySelector(".del, .monitor-bypass")).toBeNull();
  expect(badge()).toBeNull();
  row().querySelector<HTMLButtonElement>(".power")!.click();
  await settle();
  expect(command).toHaveBeenCalledWith("set_bypass", { instanceId: 44, bypassed: true });
  openMenu();
  expect(checkbox().getAttribute("aria-checked")).toBe("false");
  expect(menuItem("移除插件…")).toBeDefined();
});

it.each([".plugname", ".power"])("焦點在 %s 也能用 Shift+F10 開啟選單並返回原控制項", (selector) => {
  show();
  const target = row().querySelector<HTMLElement>(selector)!;
  target.focus();
  target.dispatchEvent(new KeyboardEvent("keydown", { key: "F10", shiftKey: true, bubbles: true, cancelable: true }));
  flushSync();
  expect(checkbox()).not.toBeNull();
  closeMenu();
  expect(document.activeElement).toBe(target);
});

it("舊 dialog 延遲送出的 close 事件不會關閉剛重開的插件選單", () => {
  show(); openMenu();
  const oldDialog = document.querySelector(".ctxdlg")!;
  closeMenu(); openMenu();
  expect(document.querySelector(".ctxdlg")).not.toBe(oldDialog);
  oldDialog.dispatchEvent(new Event("close")); flushSync();
  expect(checkbox()).not.toBeNull();
});

it("MB 僅啟用時顯示非互動標記，engine snapshot 關閉後移除", () => {
  show({ monitorBypassed: true });
  expect(badge()?.textContent).toBe("MB");
  expect(badge()?.getAttribute("role")).toBe("img");
  expect(badge()?.getAttribute("aria-label")).toContain("已啟用");
  expect(badge()?.hasAttribute("tabindex")).toBe(false);
  component!.setTracks([makeTrack()]); flushSync();
  expect(badge()).toBeNull();
});

it("MB 等待 engine 確認前不顯示假狀態，處理中會提示且禁止重複切換", async () => {
  let resolve!: (result: unknown) => void;
  command.mockImplementationOnce(() => new Promise(done => { resolve = done; }));
  show(); openMenu(); checkbox().click(); await settle();
  expect(command).toHaveBeenCalledExactlyOnceWith("set_monitor_bypass", { instanceId: 44, bypassed: true });
  expect(badge()).toBeNull();
  expect(document.querySelector(".vst")?.getAttribute("aria-busy")).toBe("true");
  expect(document.querySelector('.vsthead [role="status"]')?.textContent).toContain("處理中");
  openMenu();
  expect(checkbox().disabled).toBe(true);
  expect(checkbox().getAttribute("aria-checked")).toBe("false");
  checkbox().click(); expect(command).toHaveBeenCalledTimes(1);
  resolve({}); await settle();
  component!.setTracks([makeTrack({ monitorBypassed: true })]); flushSync();
  expect(badge()?.textContent).toBe("MB");
  expect(document.querySelector(".vst")?.getAttribute("aria-busy")).toBe("false");
  expect(row().querySelector(".power")?.getAttribute("aria-pressed")).toBe("true");
  expect(checkbox().getAttribute("aria-checked")).toBe("true");
  expect(checkbox().disabled).toBe(false);
  command.mockImplementationOnce(() => new Promise(done => { resolve = done; }));
  checkbox().click(); await settle();
  expect(command).toHaveBeenLastCalledWith("set_monitor_bypass", { instanceId: 44, bypassed: false });
  expect(badge()?.textContent).toBe("MB");
  openMenu();
  expect(checkbox().disabled).toBe(true);
  expect(checkbox().getAttribute("aria-checked")).toBe("true");
  resolve({}); await settle();
  expect(badge()).toBeNull();
  expect(checkbox().disabled).toBe(false);
  expect(checkbox().getAttribute("aria-checked")).toBe("false");
});

it.each([false, true])("MB 切換失敗保留原狀態 %s 並顯示錯誤", async (monitorBypassed) => {
  show({ monitorBypassed });
  command.mockRejectedValueOnce({ code: "internal", message: "MB command rejected" });
  openMenu(); checkbox().click(); await settle();
  expect(Boolean(badge())).toBe(monitorBypassed);
  expect(document.querySelector('[role="alert"]')?.textContent).toContain("內部錯誤");
  openMenu();
  expect(checkbox().getAttribute("aria-checked")).toBe(String(monitorBypassed));
  expect(checkbox().disabled).toBe(false);
});

it.each(["ok", "missing", "loadFailed"] as const)("%s 插件從選單移除，仍可取消且確認前不送命令", async (availability) => {
  show({ availability });
  expect(row().querySelector(".del")).toBeNull();
  openMenu(); menuItem("移除插件…").click(); await settle();
  expect(document.querySelector(".confirmdlg")?.getAttribute("aria-label")).toBe("移除 plugin?");
  expect(document.querySelector(".confirmdlg")?.textContent).toContain("FabFilter Pro-Q 3");
  expect(command).not.toHaveBeenCalled();
  document.querySelector<HTMLButtonElement>(".confirmdlg button")!.click(); await settle();
  expect(command).not.toHaveBeenCalled();
  expect(document.querySelector(".confirmdlg")).toBeNull();
  openMenu(); menuItem("移除插件…").click(); await settle();
  document.querySelector<HTMLButtonElement>(".confirmdlg button.danger")!.click(); await settle();
  expect(command).toHaveBeenCalledExactlyOnceWith("remove_plugin", { instanceId: 44 });
});

it("舊確認視窗的延遲 close 不會取消下一次移除確認", async () => {
  show(); openMenu(); menuItem("移除插件…").click(); await settle();
  const oldDialog = document.querySelector(".confirmdlg")!;
  oldDialog.querySelector<HTMLButtonElement>("button")!.click(); await settle();
  openMenu(); menuItem("移除插件…").click(); await settle();
  const current = document.querySelector(".confirmdlg");
  expect(current).not.toBeNull();
  oldDialog.dispatchEvent(new Event("close")); flushSync();
  expect(document.querySelector(".confirmdlg")).toBe(current);
  expect(command).not.toHaveBeenCalled();
});

it("不支援 latency capability 時沒有 MB 選項或標記，移除仍可用", () => {
  show({ monitorBypassed: true }, false); openMenu();
  expect(badge()).toBeNull();
  expect(checkbox()).toBeNull();
  expect(menuItem("移除插件…")).toBeDefined();
});

it("隱藏機架序號不改 editor 身分；重排後仍以目前列的 instanceId 開啟", async () => {
  show();
  const track = makeTrack();
  const first = track.plugins[0];
  const second = { ...first, instanceId: 45, name: "第二顆插件" };
  component!.setTracks([{ ...track, plugins: [first, second] }]); flushSync();
  row().querySelector<HTMLElement>('.plugname[role="button"]')!.click(); await settle();
  expect(command).toHaveBeenLastCalledWith("open_editor", { instanceId: 44 });
  component!.setTracks([{ ...track, plugins: [second, first] }]); flushSync();
  row().querySelector<HTMLElement>('.plugname[role="button"]')!.click(); await settle();
  expect(command).toHaveBeenLastCalledWith("open_editor", { instanceId: 45 });
});
