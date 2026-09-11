// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount, type ComponentProps } from "svelte";
import PluginPicker from "../PluginPicker.svelte";
import PluginPickerHarness from "./PluginPickerHarness.svelte";
import TrackStrip from "../TrackStrip.svelte";
import type { PluginClass, ScanModule, Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn(async () => ({})) }));
vi.mock("../protocol-commands.generated", async (importOriginal) => ({
  ...await importOriginal<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));

const plugin = (uid: string, name: string, vendor: string, subcategories: string): PluginClass =>
  ({ uid, name, vendor, subcategories, version: "1.0" });
const modules: ScanModule[] = [
  { path: "C:\\VST\\Studio.vst3", classes: [
    plugin("eq", "Tone 10", "Acme", "Fx|EQ|Dynamics|EQ|Stereo"),
    plugin("room", "Room", "Echo", "Fx|Reverb"),
  ] },
  { path: "C:\\Other\\Studio.vst3", classes: [plugin("eq2", "Tone 10", "Acme", "Fx|EQ")] },
  { path: "C:\\VST\\Unknown.vst3", classes: [plugin("unknown", "Mystery", "  ", "Fx|OnlyRT|Mono|Unrecognized")] },
  { path: "C:\\VST\\Synth.vst3", classes: [plugin("synth", "Keys", "Echo", "Instrument|Synth|Stereo")] },
];

const mounted: ReturnType<typeof mount>[] = [];
// 尚未提供 dialog methods 的 jsdom 版本也使用相同的最小替身。
for (const method of ["showModal", "close"] as const) {
  if (!HTMLDialogElement.prototype[method]) {
    Object.defineProperty(HTMLDialogElement.prototype, method, { configurable: true, writable: true, value() {} });
  }
}
beforeEach(() => {
  command.mockReset().mockResolvedValue({});
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
  // jsdom 不模擬原生 modal 的 top layer、焦點圈限或 Escape 預設操作。
  // 此處僅補 open/close 事件；原生鍵盤與版面另由瀏覽器驗收。
  vi.spyOn(HTMLDialogElement.prototype, "showModal").mockImplementation(function () { this.open = true; });
  vi.spyOn(HTMLDialogElement.prototype, "close").mockImplementation(function () {
    if (!this.open) return;
    this.open = false;
    this.dispatchEvent(new Event("close"));
  });
});
afterEach(async () => {
  for (const component of mounted.splice(0)) await unmount(component);
  document.body.replaceChildren();
  vi.restoreAllMocks();
});

function show(overrides: Partial<ComponentProps<typeof PluginPicker>> = {}) {
  const props = {
    trackName: "Stream", modules, onPick: vi.fn(async () => {}),
    onClose: vi.fn(), onCancelScan: vi.fn(), ...overrides,
  };
  const component = mount(PluginPickerHarness, { target: document.body, props: { initial: props } });
  mounted.push(component);
  flushSync();
  return { component, props };
}
function search(text: string) {
  const input = document.querySelector<HTMLInputElement>('input[type="search"]')!;
  input.value = text;
  input.dispatchEvent(new Event("input", { bubbles: true }));
  flushSync();
}
function grouping(value: string) {
  const select = document.querySelector<HTMLSelectElement>('[aria-label^="VST 插件列表"] select')!;
  select.value = value;
  select.dispatchEvent(new Event("change", { bubbles: true }));
  flushSync();
}
const rows = () => [...document.querySelectorAll<HTMLButtonElement>('[aria-label="插件搜尋結果"] li button')];
const groupNames = () => [...document.querySelectorAll("section")].map((group) => group.getAttribute("aria-label"));
const button = (text: string) => [...document.querySelectorAll<HTMLButtonElement>("button")].find((item) => item.textContent?.trim() === text)!;
const status = () => document.querySelector('[role="status"]')?.textContent;

describe("插件選擇器", () => {
  it("開啟預設廠牌並聚焦搜尋，可切換功能分類且跨組只計一次", () => {
    show();
    expect(document.querySelector("select")?.value).toBe("vendor");
    expect(groupNames()).toEqual(["Acme", "Echo", "未知廠牌"]);
    expect(rows()).toHaveLength(5);
    grouping("type");
    expect(document.activeElement).toBe(document.querySelector('input[type="search"]'));
    expect(document.querySelector("dialog")?.getAttribute("aria-label")).toContain("Stream");
    expect(groupNames()).toEqual(expect.arrayContaining(["EQ", "Dynamics", "Reverb", "樂器", "未分類"]));
    expect(groupNames().at(-1)).toBe("未分類");
    expect(groupNames()).not.toEqual(expect.arrayContaining(["Fx", "Stereo", "OnlyRT"]));
    expect(status()).toBe("5 / 5 個插件");
    expect(rows()).toHaveLength(6);
    expect(document.querySelector('section[aria-label="EQ"]')?.textContent).toContain("C:\\Other\\Studio.vst3");
    expect(document.querySelector('section[aria-label="EQ"]')?.textContent).toContain("C:\\VST\\Studio.vst3");
    expect(document.querySelector("[title]")).toBeNull();
    expect(rows()[0].getAttribute("data-tooltip")).toContain("1.0");
  });

  it("跨欄位、不分大小寫、多詞搜尋，切換廠牌保留查詢並清除", () => {
    show();
    grouping("type");
    search("  tOnE   acME  eq  ");
    expect(status()).toBe("2 / 5 個插件");
    expect(groupNames()).toEqual(["Dynamics", "EQ"]);
    grouping("vendor");
    expect(groupNames()).toEqual(["Acme"]);
    expect(rows()).toHaveLength(2);
    expect(document.querySelector("input")?.value).toBe("  tOnE   acME  eq  ");
    button("清除搜尋").click();
    flushSync();
    expect(status()).toBe("5 / 5 個插件");
    expect(groupNames().at(-1)).toBe("未知廠牌");
    expect(document.activeElement).toBe(document.querySelector("input"));
    search("   ");
    expect(status()).toBe("5 / 5 個插件");
    search("樂器");
    expect(rows()).toHaveLength(1);
    expect(rows()[0].textContent).toContain("Keys");
    search("不存在");
    expect(rows()).toHaveLength(0);
    expect(document.body.textContent).toContain("找不到符合條件的插件");
  });

  it("正常名稱含 EQ 不猜測分類，空 metadata、重複來源與自然排序皆穩定", () => {
    const list = [{ path: "C:\\VST\\Empty.vst3", classes: [
      plugin("b", "EQ 10", " acme ", ""),
      plugin("a", "EQ 2", "Acme", "Fx"),
    ] }];
    show({ modules: [...list, ...list] });
    grouping("type");
    expect(status()).toBe("2 / 2 個插件");
    expect(groupNames()).toEqual(["未分類"]);
    grouping("vendor");
    expect(groupNames()).toEqual(["Acme"]);
    expect(rows().map((row) => row.querySelector("strong")?.textContent)).toEqual(["EQ 2", "EQ 10"]);
  });

  it("背景掃描可取消，更新資料重新篩選，取消或失敗通知保留既有結果", () => {
    const { component, props } = show({ scanRunning: true, scanProgress: { done: 1, total: 4 } });
    search("room");
    button("取消掃描").click();
    expect(props.onCancelScan).toHaveBeenCalledOnce();
    expect(status()).toBe("1 / 5 個插件");
    component.update({ modules: [...modules, { path: "New.vst3", classes: [plugin("r", "Room 2", "Echo", "Fx|Reverb")] }], scanRunning: false });
    flushSync();
    expect(status()).toBe("2 / 6 個插件");
    component.update({ scanNotice: "掃描失敗，已保留原清單" });
    flushSync();
    expect(status()).toBe("2 / 6 個插件");
    component.update({ scanNotice: "掃描已取消，已保留原清單" });
    flushSync();
    expect(rows()).toHaveLength(2);
    expect(document.querySelector("input")?.value).toBe("room");
  });

  it("空清單與掃描中有不同提示，僅失敗項目也可查看隔離原因", () => {
    const { component } = show({ modules: [], failures: [{ path: "Broken.vst3", error: "bad module" }] });
    expect(document.body.textContent).toContain("尚無 VST 清單");
    expect(document.querySelector("details")?.textContent).toContain("bad module");
    expect(rows()).toHaveLength(0);
    component.update({ scanRunning: true });
    flushSync();
    expect(document.body.textContent).toContain("正在掃描 VST");
  });

  it("選取送出正確 class，等待中避免重複加入，成功才關閉", async () => {
    let finish!: () => void;
    const onPick = vi.fn(() => new Promise<void>((resolve) => { finish = resolve; }));
    const { props } = show({ onPick });
    search("room");
    const row = rows()[0];
    row.click();
    row.click();
    flushSync();
    expect(onPick).toHaveBeenCalledExactlyOnceWith("C:\\VST\\Studio.vst3", "room");
    expect(row.disabled).toBe(true);
    expect(document.querySelector("dialog")?.open).toBe(true);
    finish();
    await tick();
    expect(props.onClose).toHaveBeenCalledOnce();
    expect(document.querySelector("dialog")?.open).toBe(false);
  });

  it("加入錯誤在視窗內顯示，保留搜尋及分類並可重試", async () => {
    const onPick = vi.fn().mockRejectedValueOnce({ code: "plugin_load_failed", message: "測試載入失敗" }).mockResolvedValueOnce({});
    const { props } = show({ onPick });
    search("room");
    grouping("vendor");
    rows()[0].click();
    await tick();
    await vi.waitFor(() => expect(document.querySelector('[role="alert"]')?.textContent).toContain("測試載入失敗"));
    expect(document.querySelector('[role="alert"]')?.textContent).toContain("載入或初始化失敗");
    expect(document.querySelector("dialog")?.open).toBe(true);
    expect(document.querySelector("input")?.value).toBe("room");
    expect(groupNames()).toEqual(["Echo"]);
    expect(rows()[0].disabled).toBe(false);
    rows()[0].click();
    await tick();
    expect(onPick).toHaveBeenCalledTimes(2);
    await vi.waitFor(() => expect(props.onClose).toHaveBeenCalledOnce());
  });

  it("待處理加入完成時，不再次關閉已卸載的選擇器", async () => {
    let finish!: () => void;
    const { props, component } = show({ onPick: () => new Promise<void>((resolve) => { finish = resolve; }) });
    rows()[0].click();
    document.querySelector<HTMLButtonElement>('[aria-label="關閉插件選擇器"]')!.click();
    await unmount(component);
    mounted.splice(mounted.indexOf(component), 1);
    finish();
    await tick();
    expect(props.onClose).toHaveBeenCalledOnce();
  });

  it("一千個插件可搜尋並正確計數，搜尋不發送引擎命令", () => {
    show({ modules: [{ path: "Large.vst3", classes: Array.from({ length: 1000 }, (_, i) => plugin(`id${i}`, `Plugin ${i}`, "Vendor", "Fx|EQ")) }] });
    expect(status()).toBe("1000 / 1000 個插件");
    search("plugin 999");
    expect(rows()).toHaveLength(1);
    expect(rows()[0].textContent).toContain("Plugin 999");
    expect(command).not.toHaveBeenCalled();
  });
});

it.each(["audio", "app", "fx", "output"] as const)("%s 軌道接線：正確目標、關閉返回焦點、重新開啟重設", async (kind) => {
  const track: Track = {
    trackId: 42, kind, name: "測試軌", color: 0, source: null, dests: [], output: null,
    gain: 1, mute: false, plugins: [], latencyPolicy: "fullPdc",
  };
  const component = mount(TrackStrip, { target: document.body, props: {
    track, tracks: [track], devices: [], selectedDeviceKey: "", meterView: {},
    scanModules: modules, onCancelScan: vi.fn(), openMenu: vi.fn(),
  } });
  mounted.push(component);
  flushSync();
  const opener = button("＋ 加入");
  opener.focus();
  opener.click();
  flushSync();
  search("room");
  grouping("type");
  expect(command).not.toHaveBeenCalled();
  rows()[0].click();
  await tick();
  expect(command).toHaveBeenCalledExactlyOnceWith("add_plugin", {
    trackId: 42, path: "C:\\VST\\Studio.vst3", classId: "room",
  });
  await vi.waitFor(() => expect(document.activeElement).toBe(opener));
  expect(document.querySelector('[aria-label="插件搜尋結果"]')).toBeNull();
  opener.click();
  flushSync();
  expect(document.querySelector('input[type="search"]')?.getAttribute("placeholder")).toContain("名稱");
  expect((document.querySelector('input[type="search"]') as HTMLInputElement).value).toBe("");
  expect((document.querySelector("dialog.plugin-picker select") as HTMLSelectElement).value).toBe("vendor");
  document.querySelector<HTMLButtonElement>('[aria-label="關閉插件選擇器"]')!.click();
  flushSync();
  expect(document.activeElement).toBe(opener);
});
