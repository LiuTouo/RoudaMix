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

const rack = () => document.querySelector<HTMLElement>(".vst")!;
const mounted: ReturnType<typeof mount>[] = [];
// 尚未提供 dialog methods 的 jsdom 版本也使用相同的最小替身。
for (const method of ["showModal", "close"] as const) {
  if (!HTMLDialogElement.prototype[method]) {
    Object.defineProperty(HTMLDialogElement.prototype, method, { configurable: true, writable: true, value() {} });
  }
}
beforeEach(() => {
  vi.stubEnv("VITE_CONSOLE_PREVIEW", undefined);
  localStorage.clear();
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
  localStorage.clear();
  vi.restoreAllMocks();
  vi.unstubAllEnvs();
});

function show(overrides: Partial<ComponentProps<typeof PluginPicker>> = {}) {
  const props = {
    trackName: "Stream", modules, onPick: vi.fn(async () => {}),
    onClose: vi.fn(), onStartScan: vi.fn(async () => true),
    onCancelScan: vi.fn(), ...overrides,
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
const groups = () => [...document.querySelectorAll<HTMLDetailsElement>("details.plugin-group")];
const groupNames = () => groups().map((group) => group.getAttribute("aria-label"));
const rows = () => [...document.querySelectorAll<HTMLButtonElement>('[aria-label="插件搜尋結果"] li button')];
const button = (text: string) => [...document.querySelectorAll<HTMLButtonElement>("button")].find((item) => item.textContent?.trim() === text)!;
const status = () => document.querySelector('[role="status"]')?.textContent;
const selectCategory = (value: string) => {
  const select = document.querySelector<HTMLSelectElement>(".category-field select")!;
  select.value = value;
  select.dispatchEvent(new Event("change", { bubbles: true }));
  flushSync();
};
const categoryOptions = () =>
  [...document.querySelectorAll<HTMLSelectElement>(".category-field select option")].map((o) => o.value);

describe("插件選擇器", () => {
  it("預設樹狀目錄全收合；展開廠牌即見插件，同名衝突以完整路徑區分", () => {
    show();
    expect(groupNames()).toEqual(["Acme", "Echo", "未知廠牌"]);
    expect(groups().every((group) => !group.open)).toBe(true);
    expect(status()).toBe("5 / 5 個插件");
    groups()[0]!.open = true; // Acme：兩個同名 Tone 10
    const acme = [...groups()[0]!.querySelectorAll<HTMLButtonElement>("li button")];
    expect(acme.map((row) => row.querySelector(".tree-label")?.textContent?.trim())).toEqual(["Tone 10", "Tone 10"]);
    // 同名衝突以完整路徑呈現來源；分類以標籤列出
    expect(acme.map((row) => row.querySelector(".source")?.textContent)).toEqual(["C:\\Other\\Studio.vst3", "C:\\VST\\Studio.vst3"]);
    expect([...acme[1]!.querySelectorAll(".cats span")].map((chip) => chip.textContent)).toEqual(["Dynamics", "EQ"]);
    expect([...acme[0]!.querySelectorAll(".cats span")].map((chip) => chip.textContent)).toEqual(["EQ"]);
    expect(acme.map((row) => row.getAttribute("data-tooltip")))
      .toContain("C:\\Other\\Studio.vst3\nAcme · 1.0");
    expect(acme[0]!.getAttribute("data-tooltip")).toContain("1.0");
    expect(document.querySelector("[title]")).toBeNull();
  });

  it("分類下拉自動偵測 metadata，可單獨或與搜尋一起篩選", () => {
    show();
    // 目錄實際出現的分類：Dynamics/EQ（Tone 10）、Reverb（Room）、樂器（Keys）、未分類（Mystery）
    expect(categoryOptions()).toEqual(expect.arrayContaining(["", "Dynamics", "EQ", "Reverb", "樂器", "未分類"]));
    selectCategory("EQ");
    expect(status()).toBe("2 / 5 個插件");
    expect(groupNames()).toEqual(["Acme"]);
    expect(rows()).toHaveLength(2);
    search("tone");
    expect(status()).toBe("2 / 5 個插件");
    selectCategory("Reverb");
    expect(status()).toBe("0 / 5 個插件");
    expect(document.body.textContent).toContain("找不到符合條件的插件");
    selectCategory("");
    search("");
    expect(status()).toBe("5 / 5 個插件");
    expect(groups().every((group) => !group.open)).toBe(true);
  });

  it("跨欄位、不分大小寫、多詞搜尋自動展開，清除後回到全收合", () => {
    show();
    search("  tOnE   acME  eq  ");
    expect(status()).toBe("2 / 5 個插件");
    expect(groupNames()).toEqual(["Acme"]);
    expect(groups()[0]!.open).toBe(true);
    expect(rows()).toHaveLength(2);
    expect(document.querySelector("input")?.value).toBe("  tOnE   acME  eq  ");
    button("清除搜尋").click();
    flushSync();
    expect(status()).toBe("5 / 5 個插件");
    expect(groupNames().at(-1)).toBe("未知廠牌");
    expect(groups().every((group) => !group.open)).toBe(true);
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
    expect(status()).toBe("2 / 2 個插件");
    expect(groupNames()).toEqual(["Acme"]);
    groups()[0]!.open = true;
    expect([...groups()[0]!.querySelectorAll<HTMLButtonElement>("li button")]
      .map((row) => row.querySelector(".tree-label")?.textContent?.trim())).toEqual(["EQ 2", "EQ 10"]);
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

  it("掃描按鈕在工具列：未連線停用、可掃描時開新掃描，掃描中永遠可取消", async () => {
    const idle = show({ scanReady: true });
    expect(button("掃描 VST").disabled).toBe(false);
    button("掃描 VST").click();
    expect(idle.props.onStartScan).toHaveBeenCalledOnce();
    expect(idle.props.onCancelScan).not.toHaveBeenCalled();
    await unmount(mounted.pop()!);

    const offline = show({ scanReady: false });
    expect(button("掃描 VST").disabled).toBe(true);
    await unmount(mounted.pop()!);

    const running = show({ scanReady: false, scanRunning: true, scanProgress: { done: 2, total: 9 } });
    const cancel = button("取消掃描");
    expect(cancel.disabled).toBe(false);
    cancel.click();
    expect(running.props.onCancelScan).toHaveBeenCalledOnce();
    expect(running.props.onStartScan).not.toHaveBeenCalled();
  });

  it("掃描與加入期間置中顯示進度條", async () => {
    const { component } = show({ scanRunning: true, scanProgress: { done: 3, total: 8 } });
    const veil = document.querySelector(".busy-veil")!;
    expect(veil.getAttribute("role")).toBe("status");
    expect(veil.textContent).toContain("正在掃描 VST");
    expect(veil.querySelector(".busy-fill")?.getAttribute("style")).toContain("width: 38%");
    expect(veil.textContent).toContain("3/8");
    component.update({ scanRunning: false, scanProgress: null });
    flushSync();
    expect(document.querySelector(".busy-veil")).toBeNull();
    await unmount(mounted.pop()!);

    let finish!: () => void;
    show({ onPick: () => new Promise<void>((resolve) => { finish = resolve; }) });
    rows()[0].click();
    flushSync();
    const adding = document.querySelector(".busy-veil")!;
    expect(adding.textContent).toContain("正在加入插件");
    expect(adding.querySelector(".busy-fill")?.classList.contains("indeterminate")).toBe(true);
    finish();
    await vi.waitFor(() => expect(document.querySelector(".busy-veil")).toBeNull());
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

  it("選取送出正確 class，選取列反白，等待中避免重複加入，成功才關閉", async () => {
    let finish!: () => void;
    const onPick = vi.fn(() => new Promise<void>((resolve) => { finish = resolve; }));
    const { props } = show({ onPick });
    search("room");
    const row = rows()[0];
    row.click();
    row.click();
    flushSync();
    expect(onPick).toHaveBeenCalledExactlyOnceWith("C:\\VST\\Studio.vst3", "room");
    expect(row.classList.contains("sel")).toBe(true);
    expect(row.disabled).toBe(true);
    expect(document.querySelector("dialog")?.open).toBe(true);
    finish();
    await tick();
    expect(props.onClose).toHaveBeenCalledOnce();
    expect(document.querySelector("dialog")?.open).toBe(false);
  });

  it("加入錯誤在視窗內顯示，保留搜尋及分組並可重試", async () => {
    const onPick = vi.fn().mockRejectedValueOnce({ code: "plugin_load_failed", message: "測試載入失敗" }).mockResolvedValueOnce({});
    const { props } = show({ onPick });
    search("room");
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

function showTrack(kind: Track["kind"] = "audio") {
  const track: Track = {
    trackId: 42, kind, name: "測試軌", color: 0, source: null, dests: [], output: null,
    gain: 1, mute: false, plugins: [], latencyPolicy: "fullPdc",
  };
  const component = mount(TrackStrip, { target: document.body, props: {
    track, tracks: [track], devices: [], selectedDeviceKey: "", meterView: {},
    scanModules: modules, onStartScan: async () => true, onCancelScan: vi.fn(), openMenu: vi.fn(),
  } });
  mounted.push(component);
  flushSync();
  // Header Add = 圖示鈕（無文字），以 aria-label 定位
  return document.querySelector<HTMLButtonElement>('[aria-label^="加入插件到"]')!;
}

it.each(["audio", "app", "fx", "output"] as const)("%s 軌道接線：新增成功關閉選擇器、返回焦點、重新開啟重設", async (kind) => {
  const opener = showTrack(kind);
  opener.focus();
  opener.click();
  flushSync();
  search("room");
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
  expect(groups().length).toBeGreaterThan(0);
  expect(groups().every((group) => !group.open)).toBe(true);
  document.querySelector<HTMLButtonElement>('[aria-label="關閉插件選擇器"]')!.click();
  flushSync();
  expect(document.activeElement).toBe(opener);
});

it("等待加入完成期間列保持停用，完成後才關閉選擇器", async () => {
  let finish!: () => void;
  command.mockImplementationOnce(() => new Promise((resolve) => { finish = () => resolve({}); }));
  showTrack().click(); flushSync(); search("room");
  rows()[0].click(); await tick();
  expect(rows()[0].disabled).toBe(true);
  finish(); await tick();
  await vi.waitFor(() => expect(document.querySelector(".plugin-picker")).toBeNull());
});

it("加入失敗選擇器留住錯誤，重試成功才關閉", async () => {
  command.mockRejectedValueOnce({ code: "plugin_load_failed", message: "測試載入失敗" });
  showTrack().click(); flushSync(); search("room");
  rows()[0].click(); await tick();
  await vi.waitFor(() => expect(document.querySelector('[role="alert"]')?.textContent).toContain("測試載入失敗"));
  rows()[0].click(); await tick();
  await vi.waitFor(() => expect(document.querySelector(".plugin-picker")).toBeNull());
});

it("未選取插件就取消，不送命令且選擇器關閉", () => {
  showTrack().click(); flushSync();
  document.querySelector<HTMLButtonElement>('[aria-label="關閉插件選擇器"]')!.click(); flushSync();
  expect(command).not.toHaveBeenCalled();
  expect(document.querySelector(".plugin-picker")).toBeNull();
});
