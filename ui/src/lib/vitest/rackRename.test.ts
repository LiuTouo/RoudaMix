// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { flushSync, mount, unmount } from "svelte";
import TrackRoutingHarness from "./TrackRoutingHarness.svelte";
import type { Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (importOriginal) => ({
  ...await importOriginal<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));
let component: ReturnType<typeof mount>;
const nameCalls = () => command.mock.calls.filter(([kind]) => kind === "set_plugin_name");
const plug = () => document.querySelector<HTMLElement>(".strip .vstlist .plug")!;

function makeTrack(plugins: Partial<Track["plugins"][number]>[]): Track {
  return { trackId: 1, name: "聲音", kind: "audio", source: null, output: null,
    color: 0, gain: 1, mute: false, dests: [],
    plugins: plugins.map((p, i) => ({
      instanceId: p.instanceId ?? i + 1, name: `插件 ${i + 1}`, pluginPath: `p${i}.vst3`,
      classId: String(i), bypassed: false, params: [], ...p,
    })) };
}
async function show(track: Track) {
  if (component) await unmount(component);
  document.body.replaceChildren();
  component = mount(TrackRoutingHarness, { target: document.body, props: { initialTracks: [track] } });
  flushSync();
}
// F2 進入改名(同 pluginMenuKey 的 target === currentTarget 條件)
async function startRename() {
  plug().dispatchEvent(new KeyboardEvent("keydown", { key: "F2", bubbles: true }));
  await flushSync();
}
const renameInput = () => document.querySelector<HTMLInputElement>(".strip .plugrename")!;

beforeEach(() => command.mockReset().mockResolvedValue({ tracks: [] }));
afterEach(async () => { await unmount(component); document.body.innerHTML = ""; });

describe("插件自訂名稱(右鍵選單/F2 → 行內編輯)", () => {
  it("F2 進入改名;Enter 送 set_plugin_name(trim 後)", async () => {
    await show(makeTrack([{ instanceId: 1, name: "Pro-Q 3" }]));
    plug().focus();
    await startRename();
    const input = renameInput();
    expect(input).not.toBeNull();
    expect(input.value).toBe("Pro-Q 3");
    input.value = "  主唱 EQ  ";
    input.dispatchEvent(new Event("input", { bubbles: true }));
    input.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    await flushSync();
    expect(nameCalls()).toEqual([["set_plugin_name", { instanceId: 1, name: "主唱 EQ" }]]);
    expect(renameInput()).toBeNull(); // 送出後離開編輯
  });

  it("Esc 取消不送命令;blur 也送出(同 Enter)", async () => {
    await show(makeTrack([{ instanceId: 1 }]));
    plug().focus();
    await startRename();
    renameInput().dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
    await flushSync();
    expect(nameCalls()).toEqual([]);
    expect(renameInput()).toBeNull();

    plug().focus();
    await startRename();
    renameInput().value = "改名";
    renameInput().dispatchEvent(new Event("input", { bubbles: true }));
    renameInput().dispatchEvent(new FocusEvent("blur"));
    await flushSync();
    expect(nameCalls()).toEqual([["set_plugin_name", { instanceId: 1, name: "改名" }]]);
  });

  it("空白名稱 = 恢復原名(送空字串);沒改 = 不送", async () => {
    await show(makeTrack([{ instanceId: 1, name: "Vocal EQ", displayName: "Vocal EQ" }]));
    plug().focus();
    await startRename();
    renameInput().value = "   ";
    renameInput().dispatchEvent(new Event("input", { bubbles: true }));
    renameInput().dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    await flushSync();
    expect(nameCalls()).toEqual([["set_plugin_name", { instanceId: 1, name: "" }]]);

    plug().focus();
    await startRename();
    renameInput().dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    await flushSync();
    expect(nameCalls().length).toBe(1); // 同名不重複送
  });

  it("placeholder 也可改名;有自訂名時 tooltip 標示自訂名稱與檔案", async () => {
    await show(makeTrack([{ instanceId: 1, availability: "missing", name: "Ghost" }]));
    plug().focus();
    await startRename();
    expect(renameInput()).not.toBeNull();
    renameInput().value = "自訂 ghost";
    renameInput().dispatchEvent(new Event("input", { bubbles: true }));
    renameInput().dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    await flushSync();
    expect(nameCalls()).toEqual([["set_plugin_name", { instanceId: 1, name: "自訂 ghost" }]]);

    await show(makeTrack([{ instanceId: 1, name: "自訂", displayName: "自訂" }]));
    expect(document.querySelector<HTMLElement>(".strip .plugname")!.dataset.tooltip)
      .toContain("自訂名稱：自訂");
  });
});
