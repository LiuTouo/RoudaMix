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
const bBtn = () => document.querySelector<HTMLButtonElement>(".strip .console-switches .rack-b")!;
const bypassCalls = () => command.mock.calls.filter(([kind]) => kind === "set_bypass");

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
beforeEach(() => command.mockReset().mockResolvedValue({ tracks: [] }));
afterEach(async () => { await unmount(component); document.body.innerHTML = ""; });

describe("全軌 Bypass B 鈕", () => {
  it("未全 bypass 時點擊 = 全部送 set_bypass(true);再點 = 全部恢復", async () => {
    await show(makeTrack([{ instanceId: 1, bypassed: false }, { instanceId: 2, bypassed: true }]));
    const b = bBtn();
    expect(b.getAttribute("aria-pressed")).toBe("false");
    b.click(); await flushSync();
    expect(bypassCalls()).toEqual([
      ["set_bypass", { instanceId: 1, bypassed: true }],
    ]); // 已 bypass 的不重複送
    command.mockClear();

    // 模擬 engine 回報後的新狀態(全部 bypassed)
    await show(makeTrack([{ instanceId: 1, bypassed: true }, { instanceId: 2, bypassed: true }]));
    const b2 = bBtn();
    expect(b2.getAttribute("aria-pressed")).toBe("true");
    b2.click(); await flushSync();
    expect(bypassCalls()).toEqual([
      ["set_bypass", { instanceId: 1, bypassed: false }],
      ["set_bypass", { instanceId: 2, bypassed: false }],
    ]);
  });
  it("placeholder 不發命令;全 placeholder 時 B 鈕停用", async () => {
    await show(makeTrack([{ instanceId: 1, availability: "missing" }, { instanceId: 2, bypassed: false }]));
    bBtn().click(); await flushSync();
    expect(bypassCalls()).toEqual([["set_bypass", { instanceId: 2, bypassed: true }]]);

    await show(makeTrack([{ instanceId: 1, availability: "loadFailed" }]));
    expect(bBtn().disabled).toBe(true);
    expect(bBtn().getAttribute("aria-pressed")).toBe("false");
  });
  it("無插件時 B 鈕停用", async () => {
    await show(makeTrack([]));
    expect(bBtn().disabled).toBe(true);
  });
  it("M/B 控制區與標題在機架盒子外且保留右鍵機架選單", async () => {
    await show(makeTrack([{ instanceId: 1 }]));
    const rack = document.querySelector<HTMLElement>(".strip .vst")!;
    const head = document.querySelector<HTMLElement>(".strip .vsthead")!;
    const switches = document.querySelector<HTMLElement>(".strip .console-switches")!;
    expect(switches.querySelector(".rack-b")).not.toBeNull();
    expect(switches.querySelector(".mute:not(.rack-b)")).not.toBeNull();
    expect(rack.contains(switches)).toBe(false);
    expect(rack.contains(head)).toBe(false); // 盒子內不含標題列
  });
});
