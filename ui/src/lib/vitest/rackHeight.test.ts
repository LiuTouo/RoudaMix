// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, unmount } from "svelte";
import TrackRoutingHarness from "./TrackRoutingHarness.svelte";
import type { Track } from "../types";

vi.mock("../protocol-commands.generated", async (importOriginal) => ({
  ...await importOriginal<typeof import("../protocol-commands.generated")>(), engineCommand: vi.fn(),
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));
let component: ReturnType<typeof mount> | undefined;
const key = "rmix.vsth.1";
const rack = () => document.querySelector<HTMLElement>(".vst")!;
const grip = () => document.querySelector<HTMLButtonElement>(".vstgrip")!;
const track = (count = 2): Track => ({
  trackId: 1, name: "聲音", kind: "audio", source: null, output: null,
  color: 0, gain: 1, mute: false, dests: [],
  plugins: Array.from({ length: count }, (_, i) => ({
    instanceId: i + 1, name: `插件 ${i + 1}`, pluginPath: `${i}.vst3`,
    classId: String(i), bypassed: false, params: [],
  })),
});
function show() {
  component = mount(TrackRoutingHarness, { target: document.body, props: { initialTracks: [track()] } });
  flushSync();
}
function dragBy(delta: number) {
  grip().dispatchEvent(new MouseEvent("pointerdown", { clientY: 100, bubbles: true, cancelable: true }));
  window.dispatchEvent(new MouseEvent("pointermove", { clientY: 100 + delta }));
  window.dispatchEvent(new MouseEvent("pointerup"));
  flushSync();
}

beforeEach(() => {
  // Vitest stringifies custom env defines (including false); disable preview-only persistence suppression.
  vi.stubEnv("VITE_CONSOLE_PREVIEW", undefined);
  localStorage.clear();
  // jsdom has no layout; real content sizing and overflow are checked in Chromium.
  vi.spyOn(HTMLElement.prototype, "clientHeight", "get").mockReturnValue(500);
  vi.spyOn(HTMLElement.prototype, "offsetHeight", "get").mockImplementation(function () {
    if (this.classList.contains("vst")) return parseFloat(this.style.height) || 72;
    if (this.classList.contains("vsthead")) return 18;
    if (this.classList.contains("vstgrip")) return 6;
    return 0;
  });
});
afterEach(async () => {
  if (component) await unmount(component);
  component = undefined;
  document.body.replaceChildren();
  localStorage.clear();
  vi.restoreAllMocks();
  vi.unstubAllEnvs();
});

it("預設交由內容決定高度，不寫入手動偏好", () => {
  show();
  for (const count of [0, 8, 1]) {
    component!.setTracks([track(count)]);
    flushSync();
    expect(rack().style.height).toBe("");
    expect(grip().dataset.tooltip).toContain("自動依插件內容");
    expect(localStorage.getItem(key)).toBeNull();
  }
});

it("拖拉固定高度並保存，單純套用 snapshot 不會重設手動偏好", () => {
  show();
  dragBy(120);
  expect(rack().style.height).toBe("192px");
  expect(rack().style.flexShrink).not.toBe("0");
  expect(localStorage.getItem(key)).toBe("192");
  expect(grip().dataset.tooltip).toContain("手動高度");
  for (const count of [20, 0]) {
    component!.setTracks([track(count)]);
    flushSync();
    expect(rack().style.height).toBe("192px");
  }
});

it("既有每軌手動高度仍會載入", () => {
  localStorage.setItem(key, "240");
  show();
  expect(rack().style.height).toBe("240px");
  expect(grip().dataset.tooltip).toContain("手動高度");
});

it("視窗空間較小時不覆寫已儲存的手動偏好，由 CSS 限制可見高度", () => {
  localStorage.setItem(key, "800");
  show();
  expect(rack().style.height).toBe("800px");
  expect(localStorage.getItem(key)).toBe("800");
});

it.each(["dblclick", "Enter", " "])("%s 恢復自動並清除該軌手動偏好", (action) => {
  localStorage.setItem(key, "240");
  localStorage.setItem("rmix.vsth.2", "300");
  show();
  grip().dispatchEvent(action === "dblclick"
    ? new MouseEvent("dblclick", { bubbles: true })
    : new KeyboardEvent("keydown", { key: action, bubbles: true, cancelable: true }));
  flushSync();
  expect(rack().style.height).toBe("");
  expect(grip().dataset.tooltip).toContain("自動依插件內容");
  expect(localStorage.getItem(key)).toBeNull();
  expect(localStorage.getItem("rmix.vsth.2")).toBe("300");
});
