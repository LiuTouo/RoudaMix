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
  it("上移:前列下滑讓位,放下送出相同插入位置", () => {
    drag(rows()[2], "dragstart");
    // 幽靈偏移:跟隨層釘在游標右下 14px,完整露出內容不被游標蓋住
    const move = new Event("drag", { bubbles: true });
    Object.defineProperties(move, { clientX: { value: 40 }, clientY: { value: 60 } });
    window.dispatchEvent(move);
    flushSync();
    expect(document.querySelector(".drag-ghost")!.parentElement!.style.transform).toBe("translate(54px, 74px)");
    // 機架列幽靈:寬度隨內容撐開(完整名稱)、高度自適應(不鎖原高)
    const ghostEl = document.querySelector<HTMLElement>(".drag-ghost")!;
    expect(ghostEl.parentElement!.style.width).toBe("max-content");
    expect(ghostEl.style.height).toBe("");
    // 防夾:超出視窗右/下緣時夾回視窗內(jsdom viewport 1024×768;層尺寸 mock 300×200)
    vi.spyOn(ghostEl.parentElement!, "offsetWidth", "get").mockReturnValue(300);
    vi.spyOn(ghostEl.parentElement!, "offsetHeight", "get").mockReturnValue(200);
    const edge = new Event("drag", { bubbles: true });
    Object.defineProperties(edge, { clientX: { value: 1100 }, clientY: { value: 900 } });
    window.dispatchEvent(edge);
    flushSync();
    expect(ghostEl.parentElement!.style.transform).toBe("translate(724px, 568px)");
    drag(rows()[0], "dragover", 103);
    expect(rows()[0].style.transform).toBe("translateY(50px)");
    expect(rows()[1].style.transform).toBe("translateY(50px)");
    expect(rows()[2].style.transform).toBe("translateY(-100px)"); // 被拖曳列滑到首列
    vi.useFakeTimers();
    drag(rows()[0], "drop", 103);
    expect(command).toHaveBeenCalledWith("move_plugin", { instanceId: 3, newIndex: 0 });
    expect(rows()[0].style.transform).toBe("translateY(0px)");
    expect(document.querySelector(".drag-ghost")).not.toBeNull(); // 放手落定動畫中
    vi.advanceTimersByTime(300);
    expect(document.querySelector(".drag-ghost")).toBeNull();
    vi.useRealTimers();
  });
  it("下移:後列上滑,放下先移除再計算最後位置", () => {
    drag(rows()[0], "dragstart");
    drag(rows()[2], "dragover", 245);
    expect(rows()[0].style.transform).toBe("translateY(100px)");
    expect(rows()[1].style.transform).toBe("translateY(-50px)");
    expect(rows()[2].style.transform).toBe("translateY(-50px)");
    drag(rows()[2], "drop", 245);
    expect(command).toHaveBeenCalledWith("move_plugin", { instanceId: 1, newIndex: 2 });
  });
  it("插件間隙也能顯示並接受落點", () => {
    drag(rows()[2], "dragstart");
    drag(rack().querySelector<HTMLElement>(".vstlist")!, "dragover", 149);
    expect(rows()[1].style.transform).toBe("translateY(50px)");
    expect(rows()[2].style.transform).toBe("translateY(-50px)");
    drag(rack().querySelector<HTMLElement>(".vstlist")!, "drop", 149);
    expect(command).toHaveBeenCalledWith("move_plugin", { instanceId: 3, newIndex: 1 });
  });
  it("機架尾端空白可放到鏈尾", () => {
    drag(rows()[0], "dragstart");
    drag(rack().querySelector<HTMLElement>(".vstfoot")!, "dragover", 280);
    expect(rows()[2].style.transform).toBe("translateY(-50px)");
    drag(rack().querySelector<HTMLElement>(".vstfoot")!, "drop", 280);
    expect(command).toHaveBeenCalledWith("move_plugin", { instanceId: 1, newIndex: 2 });
  });
  it("離開機架歸零位移；移過子元件不清除；取消時收尾", () => {
    drag(rows()[2], "dragstart");
    drag(rows()[0], "dragover", 103);
    drag(rows()[0], "dragleave", 103, rows()[1]);
    expect(rows()[0].style.transform).toBe("translateY(50px)");
    drag(rack(), "dragleave", 103, document.body);
    expect(rows()[0].style.transform).toBe("translateY(0px)");
    drag(rows()[0], "dragover", 103);
    drag(rows()[2], "dragend");
    expect(rows()[0].style.transform).toBe("translateY(0px)");
    expect(rack().querySelector(".dragging")).toBeNull();
    expect(command).not.toHaveBeenCalled();
  });
  it("原位不產生預覽位移，也不送出排序指令", () => {
    drag(rows()[1], "dragstart");
    drag(rows()[1], "dragover", 153);
    expect(rows().every((row) => row.style.transform === "translateY(0px)")).toBe(true);
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
