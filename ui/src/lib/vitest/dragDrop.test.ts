// @vitest-environment jsdom
import { beforeEach, afterEach, describe, expect, it, vi } from "vitest";
import { flushSync, mount, unmount } from "svelte";
import TrackRoutingHarness from "./TrackRoutingHarness.svelte";
import { removeDragGhost } from "../ghost";
import type { Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (importOriginal) => ({
  ...await importOriginal<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));
let component: ReturnType<typeof mount>;
let transfer: { setData: ReturnType<typeof vi.fn>; setDragImage: ReturnType<typeof vi.fn>; effectAllowed: string; dropEffect: string };
const rack = () => document.querySelector<HTMLElement>(".strip .vst")!;
const rows = () => [...rack().querySelectorAll<HTMLElement>(".vstlist > .plug")];

function drag(element: HTMLElement, type: string, clientY = 0, relatedTarget: EventTarget | null = null) {
  const event = new Event(type, { bubbles: true, cancelable: true });
  Object.defineProperties(event, { dataTransfer: { value: transfer }, clientY: { value: clientY }, relatedTarget: { value: relatedTarget } });
  element.dispatchEvent(event);
  flushSync();
  return event;
}

beforeEach(() => {
  command.mockReset().mockResolvedValue({ tracks: [] });
  transfer = { setData: vi.fn(), setDragImage: vi.fn(), effectAllowed: "all", dropEffect: "none" };
  const track: Track = { trackId: 1, name: "聲音", kind: "audio", source: null, output: null,
    color: 0, gain: 1, mute: false, dests: [], plugins: [1,2,3].map((id) => ({
      instanceId: id, name: `插件 ${id}`, pluginPath: `plugin-${id}.vst3`, classId: String(id), bypassed: false, params: [],
    })) };
  component = mount(TrackRoutingHarness, { target: document.body, props: { initialTracks: [track] } });
  flushSync();
  rows().forEach((row, index) => vi.spyOn(row, "getBoundingClientRect").mockReturnValue({
    x: 0, y: 100 + index * 50, top: 100 + index * 50, bottom: 148 + index * 50,
    left: 0, right: 150, width: 150, height: 48, toJSON() {},
  }));
});
afterEach(async () => { await unmount(component); removeDragGhost(); document.body.innerHTML = ""; });

describe("VST 拖曳落點", () => {
  it("上移顯示首列上緣且送出相同插入位置", () => {
    drag(rows()[2], "dragstart");
    drag(rows()[0], "dragover", 103);
    expect(rows()[0].classList.contains("dropbefore")).toBe(true);
    drag(rows()[0], "drop", 103);
    expect(command).toHaveBeenCalledWith("move_plugin", { instanceId: 3, newIndex: 0 });
    expect(rack().querySelector(".dropbefore, .dropafter")).toBeNull();
    expect(document.querySelector(".drag-ghost")).toBeNull();
  });
  it("下移顯示尾列下緣且先移除再計算最後位置", () => {
    drag(rows()[0], "dragstart");
    drag(rows()[2], "dragover", 245);
    expect(rows()[2].classList.contains("dropafter")).toBe(true);
    drag(rows()[2], "drop", 245);
    expect(command).toHaveBeenCalledWith("move_plugin", { instanceId: 1, newIndex: 2 });
  });
  it("插件間隙也能顯示並接受落點", () => {
    drag(rows()[2], "dragstart");
    drag(rack().querySelector<HTMLElement>(".vstlist")!, "dragover", 149);
    expect(rows()[1].classList.contains("dropbefore")).toBe(true);
    drag(rack().querySelector<HTMLElement>(".vstlist")!, "drop", 149);
    expect(command).toHaveBeenCalledWith("move_plugin", { instanceId: 3, newIndex: 1 });
  });
  it("機架尾端空白可放到鏈尾", () => {
    drag(rows()[0], "dragstart");
    drag(rack().querySelector<HTMLElement>(".vstfoot")!, "dragover", 280);
    expect(rows()[2].classList.contains("dropafter")).toBe(true);
    drag(rack().querySelector<HTMLElement>(".vstfoot")!, "drop", 280);
    expect(command).toHaveBeenCalledWith("move_plugin", { instanceId: 1, newIndex: 2 });
  });
  it("離開機架清除指示；移過子元件不清除；取消時收尾", () => {
    drag(rows()[2], "dragstart");
    drag(rows()[0], "dragover", 103);
    drag(rows()[0], "dragleave", 103, rows()[1]);
    expect(rows()[0].classList.contains("dropbefore")).toBe(true);
    drag(rack(), "dragleave", 103, document.body);
    expect(rack().querySelector(".dropbefore, .dropafter")).toBeNull();
    drag(rows()[0], "dragover", 103);
    drag(rows()[2], "dragend");
    expect(rack().querySelector(".dropbefore, .dropafter, .dragging")).toBeNull();
    expect(command).not.toHaveBeenCalled();
  });
  it("原位不顯示移動指示，也不送出排序指令", () => {
    drag(rows()[1], "dragstart");
    drag(rows()[1], "dragover", 153);
    expect(rack().querySelector(".dropbefore, .dropafter")).toBeNull();
    drag(rows()[1], "drop", 153);
    expect(command).not.toHaveBeenCalled();
  });
  it("其他軌道或外部拖入不接受；插件拖曳不觸發外層軌道排序", () => {
    expect(drag(rack(), "dragover", 150).defaultPrevented).toBe(false);
    expect(rack().querySelector(".dropbefore, .dropafter")).toBeNull();
    const outer = vi.fn();
    document.querySelector(".strip")!.addEventListener("dragover", outer);
    drag(rows()[2], "dragstart");
    drag(rows()[0], "dragover", 103);
    expect(outer).not.toHaveBeenCalled();
  });
});
