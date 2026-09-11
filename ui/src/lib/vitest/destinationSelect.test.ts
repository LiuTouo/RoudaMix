// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import TrackRoutingHarness from "./TrackRoutingHarness.svelte";
import type { Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (importOriginal) => ({
  ...await importOriginal<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));

const track = (id: number, name: string, kind: Track["kind"], dests: number[] = []): Track => ({
  trackId: id, name, kind, dests, color: 0x789abc, source: null, output: null,
  gain: 1, mute: false, plugins: [],
});
const fixtures = () => [track(1, "麥克風", "audio", [3]), track(2, "音樂", "app"),
  track(3, "監聽", "output"), track(4, "串流", "output"), track(5, "混響", "fx")];
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
  command.mockReset().mockResolvedValue({ tracks: fixtures() });
  Object.defineProperty(HTMLElement.prototype, "showPopover", { configurable: true,
    value(this: HTMLElement) { toggleEvent(this, "open"); } });
  Object.defineProperty(HTMLElement.prototype, "hidePopover", { configurable: true,
    value(this: HTMLElement) { toggleEvent(this, "closed"); } });
});
afterEach(async () => {
  for (const component of mounted.splice(0)) await unmount(component);
  document.body.innerHTML = "";
});

function show(initialTracks = fixtures()) {
  const component = mount(TrackRoutingHarness, { target: document.body, props: { initialTracks } });
  mounted.push(component);
  flushSync();
  return component;
}
const trigger = () => document.querySelector<HTMLButtonElement>(".destination-trigger")!;
const panel = () => document.querySelector<HTMLElement>(".destination-popup")!;
const checkboxes = () => [...panel().querySelectorAll<HTMLInputElement>('input[type="checkbox"]')];
function check(index: number, value: boolean) {
  const input = checkboxes()[index];
  input.checked = value;
  input.dispatchEvent(new Event("change", { bubbles: true }));
  flushSync();
}
async function settle() { await tick(); await Promise.resolve(); flushSync(); }
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((res, rej) => { resolve = res; reject = rej; });
  return { promise, resolve, reject };
}

describe("輸出目的地多選下拉", () => {
  it("只列出 FX／輸出軌，保留既有選取並顯示名稱", () => {
    show();
    expect(checkboxes()).toHaveLength(3);
    expect(panel().textContent).toContain("監聽");
    expect(panel().textContent).not.toContain("音樂");
    expect(checkboxes().map((input) => input.checked)).toEqual([true, false, false]);
    expect(trigger().textContent).toContain("監聽");
    expect(trigger().getAttribute("aria-haspopup")).toBe("dialog");
  });

  it("FX 軌的候選目的地排除自己；沒有候選時停用", () => {
    show([track(8, "效果", "fx"), track(2, "音樂", "app")]);
    expect(checkboxes()).toHaveLength(0);
    expect(trigger().disabled).toBe(true);
    expect(trigger().textContent).toContain("無可用目的地");
  });

  it("連續多選立即送出且不丟失最後意圖，也能取消全部", async () => {
    const harness = show();
    const first = deferred<{ tracks: Track[] }>();
    command.mockReturnValueOnce(first.promise).mockResolvedValue({ tracks: fixtures() });
    panel().showPopover(); flushSync();
    check(1, true);
    check(2, true);
    expect(checkboxes().map((input) => input.checked)).toEqual([true, true, true]);
    expect(command).toHaveBeenCalledTimes(1);
    expect(command.mock.calls[0]).toEqual(["track_set_dests", { trackId: 1, dests: [3, 4] }]);
    first.resolve({ tracks: fixtures() });
    await settle();
    expect(command.mock.calls[1]).toEqual(["track_set_dests", { trackId: 1, dests: [3, 4, 5] }]);
    const confirmed = fixtures(); confirmed[0].dests = [3, 4, 5];
    harness.setTracks(confirmed); flushSync();
    check(0, false); check(1, false); check(2, false);
    await settle();
    expect(command.mock.calls.at(-1)).toEqual(["track_set_dests", { trackId: 1, dests: [] }]);
    expect(trigger().getAttribute("aria-expanded")).toBe("true");
  });

  it("最新寫入失敗會恢復引擎狀態，顯示錯誤並結束待套用狀態", async () => {
    show();
    command.mockRejectedValueOnce({ code: "cycle_detected", message: "循環路由" });
    check(1, true);
    await settle();
    expect(checkboxes().map((input) => input.checked)).toEqual([true, false, false]);
    expect(document.querySelector(".destination-pending")).toBeNull();
    expect(document.querySelector('[role="alert"]')?.textContent).toContain("路由未套用");
  });

  it("舊請求失敗不會回滾後續勾選", async () => {
    show();
    const first = deferred<{ tracks: Track[] }>();
    const second = deferred<{ tracks: Track[] }>();
    command.mockReturnValueOnce(first.promise).mockReturnValueOnce(second.promise);
    check(1, true); check(2, true);
    first.reject({ code: "cycle_detected", message: "循環路由" });
    await settle();
    expect(checkboxes().map((input) => input.checked)).toEqual([true, true, true]);
    second.resolve({ tracks: fixtures() });
    await settle();
  });

  it("方向鍵移動焦點，Escape 收起後返回觸發按鈕", () => {
    show();
    panel().showPopover(); flushSync();
    expect(document.activeElement).toBe(checkboxes()[0]);
    checkboxes()[0].dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true }));
    expect(document.activeElement).toBe(checkboxes()[1]);
    checkboxes()[1].dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    flushSync();
    expect(trigger().getAttribute("aria-expanded")).toBe("false");
    expect(document.activeElement).toBe(trigger());
  });

  it("即時更新已改名或移除的目的地", () => {
    const harness = show();
    const next = fixtures(); next[2].name = "監聽耳機";
    harness.setTracks(next); flushSync();
    expect(trigger().textContent).toContain("監聽耳機");
    next[0] = { ...next[0], dests: [] };
    harness.setTracks(next.filter((item) => item.trackId !== 3)); flushSync();
    expect(checkboxes()).toHaveLength(2);
    expect(trigger().textContent).toContain("(無)");
  });
});
