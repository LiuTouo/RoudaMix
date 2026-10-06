// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import TrackStrip from "../TrackStrip.svelte";
import type { DeviceInfo, Track } from "../types";

const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (importOriginal) => ({
  ...await importOriginal<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));

const microphone = { id: "usb-mic", name: "USB 麥克風", default: true, sampleRate: 48000 };
const mounted: ReturnType<typeof mount>[] = [];
beforeEach(() => {
  command.mockReset().mockResolvedValue({ devices: [microphone] });
  vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockReturnValue(null);
});
afterEach(async () => {
  for (const component of mounted.splice(0)) await unmount(component);
  document.body.replaceChildren();
  vi.restoreAllMocks();
});

function show(devices: DeviceInfo[] = []) {
  const track: Track = {
    trackId: 42, kind: "audio", name: "麥克風", color: 0, source: null,
    dests: [], output: null, gain: 1, mute: false, plugins: [],
  };
  mounted.push(mount(TrackStrip, { target: document.body, props: {
    track, tracks: [track], devices, selectedDeviceKey: devices[0]?.deviceKey ?? "wasapi",
    metered: false, onCancelScan: vi.fn(), openMenu: vi.fn(),
  } }));
  flushSync();
  return document.querySelector<HTMLSelectElement>("select")!;
}
async function settle() { await tick(); await Promise.resolve(); flushSync(); }
function selectSource(input: HTMLSelectElement, value: string) {
  input.value = value;
  input.dispatchEvent(new Event("change", { bubbles: true }));
}

it("無 ASIO 時可聚焦載入 WASAPI 麥克風，並選擇預設或指定裝置", async () => {
  const input = show();
  // 使用真正的 focus()，避免手動派送事件繞過 disabled 而漏掉回歸。
  input.focus();
  expect(document.activeElement).toBe(input);
  await settle();
  expect(command).toHaveBeenCalledExactlyOnceWith("list_capture_devices", {});
  expect([...input.options].map((option) => option.value)).toEqual(["", "mic:default", "mic:usb-mic"]);
  expect(input.textContent).toContain("USB 麥克風");

  for (const deviceId of ["", "usb-mic"]) {
    selectSource(input, `mic:${deviceId || "default"}`);
    await settle();
    expect(command).toHaveBeenLastCalledWith("track_set_source", {
      trackId: 42, source: { type: "wasapiIn", deviceId },
    });
  }
});

it.each(["empty", "error"])("列舉結果為 %s 後仍可重新聚焦載入裝置", async (result) => {
  if (result === "empty") command.mockResolvedValueOnce({ devices: [] });
  else command.mockRejectedValueOnce({ code: "internal", message: "capture enumeration failed" });
  const input = show();
  input.focus();
  await settle();
  expect(command).toHaveBeenCalledExactlyOnceWith("list_capture_devices", {});
  expect(input.disabled).toBe(false);
  expect(input.options).toHaveLength(1);
  if (result === "error") expect(document.querySelector('[role="alert"]')?.textContent).toContain("capture enumeration failed");

  input.blur();
  input.focus();
  await settle();
  expect(command).toHaveBeenCalledTimes(2);
  expect(command).toHaveBeenLastCalledWith("list_capture_devices", {});
  expect([...input.options].map((option) => option.value)).toContain("mic:usb-mic");
});

it("無 ASIO 驅動時 asioIn 來源顯示說明選項而非空白", () => {
  const track: Track = {
    trackId: 42, kind: "audio", name: "ASIO", color: 0,
    source: { type: "asioIn", channel: 1, mono: true },
    dests: [], output: null, gain: 1, mute: false, plugins: [],
  };
  mounted.push(mount(TrackStrip, { target: document.body, props: {
    track, tracks: [track], devices: [], selectedDeviceKey: "wasapi",
    metered: false, onCancelScan: vi.fn(), openMenu: vi.fn(),
  } }));
  flushSync();
  const input = document.querySelector<HTMLSelectElement>("select")!;
  expect(input.value).toBe("m1");
  const opt = [...input.options].find((option) => option.value === "m1");
  expect(opt?.disabled).toBe(true);
  expect(opt?.textContent).toContain("單聲");
  expect(opt?.textContent).toContain("無 ASIO 驅動");
});

it("ASIO 輸入保留立體聲與單聲道選項，並可同時載入 WASAPI 麥克風", async () => {
  const input = show([{
    deviceKey: "asio:test", name: "ASIO", maxIn: 2, maxOut: 2,
    sampleRates: [48000], currentSampleRate: 48000, minBufferSize: 128,
    maxBufferSize: 512, preferredBufferSize: 256, bufferSizes: [128, 256, 512],
    inputNames: ["Input L", "Input R"], outputNames: ["Output L", "Output R"],
  }]);
  input.focus();
  await settle();
  expect([...input.options].map((option) => option.value)).toEqual(["", "0", "m0", "m1", "mic:default", "mic:usb-mic"]);
  for (const [value, source] of [
    ["0", { type: "asioIn", channel: 0 }],
    ["m1", { type: "asioIn", channel: 1, mono: true }],
  ] as const) {
    selectSource(input, value);
    await settle();
    expect(command).toHaveBeenLastCalledWith("track_set_source", { trackId: 42, source });
  }
});
