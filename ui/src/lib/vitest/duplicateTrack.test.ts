// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import PluginTransferHarness from "./PluginTransferHarness.svelte";
import type { Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (original) => ({
  ...await original<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));
let component: ReturnType<typeof mount>;

const makeTrack = (id: number, systemRole?: Track["systemRole"]): Track => ({
  trackId: id, name: `音軌 ${id}`, kind: "audio", systemRole: systemRole ?? null,
  source: null, output: null, color: 0, gain: 1, mute: false, dests: [], plugins: [],
});

function strip(index = 0) {
  return [...document.querySelectorAll<HTMLElement>(".strip")][index]!;
}
function menuItem(label: string) {
  return [...document.querySelectorAll<HTMLButtonElement>("[role]")].find(
    (item) => item.textContent?.trim() === label,
  )!;
}
async function openMenu(target: HTMLElement) {
  target.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true }));
  flushSync();
}

beforeEach(() => {
  command.mockReset().mockResolvedValue({ trackId: 99, tracks: [] });
  for (const method of ["showModal", "close"] as const) {
    Object.defineProperty(HTMLDialogElement.prototype, method, { configurable: true, value() { this.open = method === "showModal"; } });
  }
});
afterEach(async () => {
  if (component) await unmount(component);
  document.body.replaceChildren(); vi.restoreAllMocks();
});

describe("軌道右鍵選單：複製音軌", () => {
  it("點擊後送出 duplicate_track（帶原軌 trackId）", async () => {
    component = mount(PluginTransferHarness, {
      target: document.body,
      props: { initialTracks: [makeTrack(7), makeTrack(8)] },
    });
    flushSync();
    await openMenu(strip());
    menuItem("複製音軌").click();
    await tick(); flushSync();
    expect(command).toHaveBeenCalledWith("duplicate_track", { trackId: 7 });
  });

  it("系統輸出軌（monitor/stream）停用複製", async () => {
    component = mount(PluginTransferHarness, {
      target: document.body,
      props: { initialTracks: [makeTrack(1, "monitor"), makeTrack(2, "stream"), makeTrack(3)] },
    });
    flushSync();
    await openMenu(strip(0));
    expect(menuItem("複製音軌").disabled).toBe(true);
    await openMenu(strip(2));
    expect(menuItem("複製音軌").disabled).toBe(false);
  });

  it("舊引擎（無 trackDuplicateV1 capability）停用複製", async () => {
    component = mount(PluginTransferHarness, {
      target: document.body,
      props: { initialTracks: [makeTrack(1)], trackDuplicate: false },
    });
    flushSync();
    await openMenu(strip());
    expect(menuItem("複製音軌").disabled).toBe(true);
    expect(command).not.toHaveBeenCalled();
  });
});
