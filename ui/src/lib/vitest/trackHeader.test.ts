// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import TrackStrip from "../TrackStrip.svelte";
import type { Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (importOriginal) => ({
  ...await importOriginal<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));
let component: ReturnType<typeof mount>;
beforeEach(() => {
  command.mockReset().mockResolvedValue({});
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
  const track: Track = {
    trackId: 42, kind: "audio", name: "麥克風", color: 0,
    source: null, dests: [], output: null, gain: 1, mute: false, plugins: [],
  };
  component = mount(TrackStrip, { target: document.body, props: {
    track, tracks: [track], devices: [], selectedDeviceKey: "", metered: false,
    onStartScan: async () => true, onCancelScan: vi.fn(), openMenu: vi.fn(),
  } });
  flushSync();
});
afterEach(async () => {
  await unmount(component);
  document.body.replaceChildren();
  vi.restoreAllMocks();
});
async function editName(value: string) {
  document.querySelector<HTMLButtonElement>(".head .name")!.click();
  await tick();
  const input = document.querySelector<HTMLInputElement>(".head .nameedit")!;
  input.value = value;
  input.dispatchEvent(new Event("input", { bubbles: true }));
  await tick();
  return input;
}

it("頂部名稱保留 Enter 套用與失焦提交，色票保留換色命令", async () => {
  const input = await editName("新名稱");
  input.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
  await tick();
  expect(command).toHaveBeenCalledWith("track_set", { trackId: 42, name: "新名稱" });
  const second = await editName("另一個名稱");
  second.dispatchEvent(new FocusEvent("blur"));
  await tick();
  expect(command).toHaveBeenCalledWith("track_set", { trackId: 42, name: "另一個名稱" });
  const color = document.querySelector<HTMLInputElement>(".head .barpick")!;
  color.value = "#123456";
  color.dispatchEvent(new Event("change", { bubbles: true }));
  await tick();
  expect(command).toHaveBeenCalledWith("track_set", { trackId: 42, color: 0x123456 });
});

it("頂部名稱保留 Esc 取消與拒絕空白名稱", async () => {
  const input = await editName("取消的名稱");
  input.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
  await tick();
  expect(document.querySelector(".head .nameedit")).toBeNull();
  const blank = await editName("   ");
  blank.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
  await tick();
  expect(command).not.toHaveBeenCalled();
});
