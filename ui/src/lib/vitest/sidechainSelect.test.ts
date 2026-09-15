// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import TrackRoutingHarness from "./TrackRoutingHarness.svelte";
import { sidechainCandidates } from "../tracks";
import type { Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (importOriginal) => ({
  ...await importOriginal<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));

const track = (
  id: number,
  name: string,
  kind: Track["kind"],
  dests: number[] = [],
  sidechain: number[] = [],
): Track => ({
  trackId: id, name, kind, dests, sidechain, color: 0x789abc, source: null,
  output: null, gain: 1, mute: false, plugins: [],
});
const mounted: ReturnType<typeof mount>[] = [];

function toggleEvent(element: HTMLElement, newState: string) {
  const before = new Event("beforetoggle");
  Object.defineProperty(before, "newState", { value: newState });
  element.dispatchEvent(before);
  const event = new Event("toggle");
  Object.defineProperty(event, "newState", { value: newState });
  element.dispatchEvent(event);
}

beforeEach(() => {
  command.mockReset().mockResolvedValue({ tracks: [] });
  Object.defineProperty(HTMLElement.prototype, "showPopover", { configurable: true,
    value(this: HTMLElement) { toggleEvent(this, "open"); } });
  Object.defineProperty(HTMLElement.prototype, "hidePopover", { configurable: true,
    value(this: HTMLElement) { toggleEvent(this, "closed"); } });
});
afterEach(async () => {
  for (const component of mounted.splice(0)) await unmount(component);
  document.body.innerHTML = "";
});

function show(initialTracks: Track[]) {
  const component = mount(TrackRoutingHarness, { target: document.body, props: { initialTracks } });
  mounted.push(component);
  flushSync();
  return component;
}
const triggers = () =>
  [...document.querySelectorAll<HTMLButtonElement>(".destination-trigger")];
const popups = () => [...document.querySelectorAll<HTMLElement>(".destination-popup")];
function check(popup: HTMLElement, index: number, value: boolean) {
  const input = popup.querySelectorAll<HTMLInputElement>('input[type="checkbox"]')[index];
  input.checked = value;
  input.dispatchEvent(new Event("change", { bubbles: true }));
  flushSync();
}
async function settle() { await tick(); await Promise.resolve(); flushSync(); }

describe("側鏈來源", () => {
  it("sidechainCandidates 只列 audio/app 軌(引擎規則鏡像)", () => {
    const tracks = [
      track(1, "麥克風", "audio"),
      track(2, "音樂", "app"),
      track(3, "混響", "fx"),
      track(4, "監聽", "output"),
    ];
    expect(sidechainCandidates(tracks).map((t) => t.trackId)).toEqual([1, 2]);
  });

  it("fx 軌顯示側鏈來源列,勾選送 track_set_sidechain;非 fx 軌不顯示", async () => {
    const fixtures = [
      track(5, "壓縮", "fx"),
      track(1, "麥克風", "audio"),
      track(3, "監聽", "output"),
    ];
    show(fixtures);
    // fx 軌:輸出到 + 側鏈來源 兩列;label 辨認
    expect(triggers()).toHaveLength(2);
    const rows = [...document.querySelectorAll(".destination-row")];
    expect(rows.map((r) => r.textContent?.includes("側鏈來源"))).toEqual([false, true]);
    // popover id 去重:popovertarget 各指各的 panel(寫死同 id 會永遠開第一個)
    const [destTarget, sidechainTarget] = triggers().map((t) =>
      t.getAttribute("popovertarget"),
    );
    expect(destTarget).not.toBe(sidechainTarget);
    const panels = popups().map((p) => p.id);
    expect(panels).toContain(destTarget!);
    expect(panels).toContain(sidechainTarget!);

    const sidechainPopup = popups()[1];
    sidechainPopup.showPopover();
    flushSync();
    check(sidechainPopup, 0, true);
    await settle();
    expect(command.mock.calls.at(-1)).toEqual([
      "track_set_sidechain", { trackId: 5, sources: [1] },
    ]);

    // 非 fx 軌(audio):只有輸出到列
    document.body.innerHTML = "";
    show([fixtures[1], fixtures[2]]);
    expect(triggers()).toHaveLength(1);
  });

  it("失敗時恢復引擎回報值", async () => {
    show([track(5, "壓縮", "fx", [], [1]), track(1, "麥克風", "audio")]);
    command.mockRejectedValueOnce({ code: "cycle_detected", message: "循環路由" });
    const sidechainPopup = popups()[1];
    sidechainPopup.showPopover();
    flushSync();
    check(sidechainPopup, 0, false);  // 取消唯一來源 → sources []
    await settle();
    // 恢復:引擎回報仍是 [1],重新打開勾選狀態為已勾
    sidechainPopup.showPopover();
    flushSync();
    const input = sidechainPopup.querySelectorAll<HTMLInputElement>('input[type="checkbox"]')[0];
    expect(input.checked).toBe(true);
  });
});
