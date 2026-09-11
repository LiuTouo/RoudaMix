// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import { get } from "svelte/store";
import PluginTransferHarness from "./PluginTransferHarness.svelte";
import { copyPlugin, pastePlugin, pluginTransfer, resetPluginTransfer, PLUGIN_DRAG_TYPE } from "../pluginTransfer";
import { removeDragGhost } from "../ghost";
import type { Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (original) => ({
  ...await original<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));
let component: ReturnType<typeof mount>;
let transfer: { setData: ReturnType<typeof vi.fn>; setDragImage: ReturnType<typeof vi.fn>; effectAllowed: string; dropEffect: string };
const makeTrack = (id: number, ids: number[]): Track => ({
  trackId: id, name: `音軌 ${id}`, kind: id === 3 ? "output" : "audio", source: null, output: null,
  color: 0, gain: 1, mute: false, dests: [], plugins: ids.map((instanceId) => ({
    instanceId, name: "同款插件", pluginPath: "fixture.vst3", classId: "fixture", bypassed: false, params: [],
  })),
});
const racks = () => [...document.querySelectorAll<HTMLElement>(".vst")];
const rows = (rack = 0) => [...racks()[rack].querySelectorAll<HTMLElement>(".plug")];
function show(tracks = [makeTrack(1, [1, 2]), makeTrack(2, [3, 4]), makeTrack(3, [])], enabled = true) {
  component = mount(PluginTransferHarness, { target: document.body, props: { initialTracks: tracks, enabled } });
  flushSync();
  racks().forEach((rack) => [...rack.querySelectorAll<HTMLElement>(".plug")].forEach((row, i) => {
    vi.spyOn(row, "getBoundingClientRect").mockReturnValue({ top: 100 + 50 * i, height: 48,
      left: 0, right: 170, bottom: 148 + 50 * i, width: 170, x: 0, y: 100 + 50 * i, toJSON() {} });
  }));
}
function drag(element: HTMLElement, type: string, y = 0) {
  const event = new Event(type, { bubbles: true, cancelable: true });
  Object.defineProperties(event, { dataTransfer: { value: transfer }, clientY: { value: y } });
  element.dispatchEvent(event); flushSync(); return event;
}
function menu(element: HTMLElement) {
  element.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true })); flushSync();
}
function menuItem(label: string) {
  return [...document.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')].find((item) => item.textContent?.trim() === label)!;
}
async function choose(label: string) { menuItem(label).click(); await tick(); flushSync(); }
beforeEach(() => {
  resetPluginTransfer();
  command.mockReset().mockImplementation(async (kind: string) => kind === "copy_plugin"
    ? { clipboardId: "snapshot-1", name: "同款插件" } : { tracks: [] });
  transfer = { setData: vi.fn(), setDragImage: vi.fn(), effectAllowed: "all", dropEffect: "none" };
  for (const method of ["showModal", "close"] as const) {
    Object.defineProperty(HTMLDialogElement.prototype, method, { configurable: true, value() { this.open = method === "showModal"; } });
  }
});
afterEach(async () => {
  if (component) await unmount(component);
  resetPluginTransfer(); removeDragGhost(); document.body.replaceChildren(); vi.restoreAllMocks();
});

describe("VST 複製選單", () => {
  it("空剪貼簿禁用貼上；複製後可在插件後與空輸出機架重複貼上", async () => {
    show(); menu(racks()[2]); expect(menuItem("貼上").disabled).toBe(true);
    menu(rows()[0]); await choose("複製");
    expect(command).toHaveBeenCalledWith("copy_plugin", { instanceId: 1 });
    menu(rows(1)[0]); await choose("在此插件後貼上");
    expect(command).toHaveBeenCalledWith("paste_plugin", { clipboardId: "snapshot-1", trackId: 2, newIndex: 1 });
    menu(racks()[2]); await choose("貼上");
    expect(command).toHaveBeenCalledWith("paste_plugin", { clipboardId: "snapshot-1", trackId: 3, newIndex: 0 });
    expect(get(pluginTransfer).clipboard?.clipboardId).toBe("snapshot-1");
  });
  it("鍵盤 Shift+F10 與 ContextMenu 可開啟插件與機架選單", async () => {
    show();
    rows()[0].dispatchEvent(new KeyboardEvent("keydown", { key: "F10", shiftKey: true, bubbles: true }));
    flushSync(); await choose("複製");
    racks()[2].dispatchEvent(new KeyboardEvent("keydown", { key: "ContextMenu", bubbles: true }));
    flushSync(); expect(menuItem("貼上").disabled).toBe(false);
  });
  it("複製失败保留舊剪貼簿並在來源顯示錯誤", async () => {
    show(); await copyPlugin(1);
    command.mockRejectedValueOnce({ code: "plugin_state_failed", message: "state rejected" });
    menu(rows()[1]); await choose("複製"); await tick();
    expect(get(pluginTransfer).clipboard?.clipboardId).toBe("snapshot-1");
    expect(document.body.textContent).toContain("state rejected");
  });
  it("舊引擎不顯示複製選項，placeholder 禁用複製", () => {
    const track = makeTrack(1, [1]); track.plugins[0].availability = "missing";
    show([track]); menu(rows()[0]); expect(menuItem("複製").disabled).toBe(true);
  });
  it("無 capability 時選單維持既有項目", () => {
    show(undefined, false); menu(rows()[0]); expect(menuItem("複製")).toBeUndefined();
  });
});
describe("跨音軌拖曳", () => {
  it("空列表以可見文字標示落點，離開或放下後恢復空白狀態", async () => {
    show();
    const emptyList = racks()[2].querySelector<HTMLElement>(".vstlist")!;
    expect(emptyList.textContent).toContain("無插件");
    expect(emptyList.textContent).not.toContain("放開以複製插件");
    drag(rows()[0], "dragstart");
    drag(emptyList, "dragover", 100);
    expect(emptyList.textContent).toContain("放開以複製插件");
    expect(emptyList.querySelector(".empty-slot.drop-target")).not.toBeNull();
    drag(racks()[2], "dragleave");
    expect(emptyList.textContent).toContain("無插件");
    expect(emptyList.querySelector(".drop-target")).toBeNull();
    drag(emptyList, "dragover", 100);
    drag(emptyList, "drop", 100);
    await tick();
    expect(command).toHaveBeenCalledExactlyOnceWith("duplicate_plugin", { instanceId: 1, trackId: 3, newIndex: 0 });
    expect(emptyList.textContent).toContain("無插件");
    expect(emptyList.querySelector(".drop-target")).toBeNull();
  });
  it.each([[103, 0], [149, 1], [240, 2]])("依落點 %i 複製到位置 %i，保留剪貼簿及來源", async (y, newIndex) => {
    show(); await copyPlugin(1); command.mockClear();
    drag(rows()[0], "dragstart");
    expect(transfer.setData).toHaveBeenCalledWith(PLUGIN_DRAG_TYPE, "1");
    expect(transfer.effectAllowed).toBe("copyMove");
    const outer = vi.fn(); racks()[1].parentElement!.addEventListener("dragover", outer);
    drag(racks()[1], "dragover", y); expect(transfer.dropEffect).toBe("copy");
    drag(racks()[1], "drop", y);
    expect(command).toHaveBeenCalledExactlyOnceWith("duplicate_plugin", { instanceId: 1, trackId: 2, newIndex });
    expect(outer).not.toHaveBeenCalled(); expect(rows()).toHaveLength(2);
    expect(get(pluginTransfer).clipboard?.clipboardId).toBe("snapshot-1");
    expect(document.querySelector(".dropbefore, .dropafter, .drag-ghost")).toBeNull();
  });
  it("空機架顯示落點並接受副本，同軌仍是排序", async () => {
    show(); drag(rows()[0], "dragstart"); drag(racks()[2], "dragover", 100);
    expect(racks()[2].classList.contains("emptydrop")).toBe(true);
    drag(racks()[2], "drop", 100); await tick();
    expect(command).toHaveBeenCalledWith("duplicate_plugin", { instanceId: 1, trackId: 3, newIndex: 0 });
    drag(rows()[1], "dragstart"); drag(racks()[0], "drop", 103);
    expect(command).toHaveBeenCalledWith("move_plugin", { instanceId: 2, newIndex: 0 });
  });
  it("取消清除其他機架的指示；拒絕外部拖入", () => {
    show(); expect(drag(racks()[1], "dragover", 100).defaultPrevented).toBe(false);
    drag(rows()[0], "dragstart"); drag(racks()[2], "dragover", 100); drag(rows()[0], "dragend");
    expect(document.querySelector(".emptydrop, .dropbefore, .dragging")).toBeNull();
    expect(command).not.toHaveBeenCalled();
  });
  it("placeholder 不接受跨軌落點，也不誤觸排序", () => {
    const track = makeTrack(1, [1]); track.plugins[0].availability = "missing";
    show([track, makeTrack(2, [])]); drag(rows()[0], "dragstart"); drag(racks()[1], "dragover", 100);
    expect(transfer.dropEffect).toBe("none"); drag(racks()[1], "drop", 100);
    expect(command).not.toHaveBeenCalled();
  });
});
describe("剪貼簿生命週期", () => {
  it("重設後忽略舊複製回覆；處理中拒絕重複指令", async () => {
    let resolve!: (value: unknown) => void;
    command.mockImplementationOnce(() => new Promise((done) => { resolve = done; }));
    const pending = copyPlugin(1); await copyPlugin(2);
    expect(command).toHaveBeenCalledTimes(1); resetPluginTransfer();
    resolve({ clipboardId: "stale", name: "舊來源" }); await pending;
    expect(get(pluginTransfer).clipboard).toBeNull(); await pastePlugin(3, 0);
    expect(command).toHaveBeenCalledTimes(1);
  });
});
