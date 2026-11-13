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

/** 回來源選單錨點按鈕(popover 內容在 jsdom 一樣有 render,可直接斷言/點擊) */
function show(devices: DeviceInfo[] = [], track?: Track) {
  const t: Track = track ?? {
    trackId: 42, kind: "audio", name: "麥克風", color: 0, source: null,
    dests: [], output: null, gain: 1, mute: false, plugins: [],
  };
  mounted.push(mount(TrackStrip, { target: document.body, props: {
    track: t, tracks: [t], devices, selectedDeviceKey: devices[0]?.deviceKey ?? "wasapi",
    metered: false, onCancelScan: vi.fn(), openMenu: vi.fn(),
  } }));
  flushSync();
  return document.querySelector<HTMLButtonElement>(".source-trigger")!;
}
async function settle() { await tick(); await Promise.resolve(); flushSync(); }
/** 點開選單(載入裝置清單)後回全部可選列的 data-value */
async function openMenu(trigger: HTMLButtonElement) {
  trigger.click();
  await settle();
  return [...document.querySelectorAll<HTMLButtonElement>(".source-popup .src-row")]
    .map((row) => row.dataset.value ?? "");
}
function pick(value: string) {
  const row = document.querySelector<HTMLButtonElement>(`.source-popup .src-row[data-value="${value}"]`)!;
  row.click();
}

it("無 ASIO 時點開可載入 WASAPI 麥克風，並選擇預設或指定裝置", async () => {
  const trigger = show();
  const values = await openMenu(trigger);
  expect(command).toHaveBeenCalledExactlyOnceWith("list_capture_devices", {});
  expect(values).toEqual(["", "mic:default", "mic:usb-mic"]);
  expect(document.querySelector(".source-popup")!.textContent).toContain("USB 麥克風");
  expect(document.querySelector(".source-popup")!.textContent).toContain("WASAPI");

  for (const deviceId of ["", "usb-mic"]) {
    pick(`mic:${deviceId || "default"}`);
    await settle();
    expect(command).toHaveBeenLastCalledWith("track_set_source", {
      trackId: 42, source: { type: "wasapiIn", deviceId },
    });
  }
});

it.each(["empty", "error"])("列舉結果為 %s 後仍可重新點開載入裝置", async (result) => {
  if (result === "empty") command.mockResolvedValueOnce({ devices: [] });
  else command.mockRejectedValueOnce({ code: "internal", message: "capture enumeration failed" });
  const trigger = show();
  await openMenu(trigger);
  expect(command).toHaveBeenCalledExactlyOnceWith("list_capture_devices", {});
  if (result === "error") {
    await settle();
    expect(document.querySelector('[role="alert"]')?.textContent).toContain("capture enumeration failed");
  }

  const values = await openMenu(trigger);
  expect(command).toHaveBeenCalledTimes(2);
  expect(command).toHaveBeenLastCalledWith("list_capture_devices", {});
  expect(values).toContain("mic:usb-mic");
});

it("無 ASIO 驅動時 asioIn 來源顯示說明摘要而非空白", () => {
  show([], {
    trackId: 42, kind: "audio", name: "ASIO", color: 0,
    source: { type: "asioIn", channel: 1, mono: true },
    dests: [], output: null, gain: 1, mute: false, plugins: [],
  });
  const trigger = document.querySelector<HTMLButtonElement>(".source-trigger")!;
  expect(trigger.textContent).toContain("單聲");
  expect(trigger.textContent).toContain("無 ASIO 驅動");
  expect(document.querySelector(".noasio")?.textContent).toContain("來源保留但靜音");
});

it("ASIO 輸入保留立體聲與單聲道選項，並可同時載入 WASAPI 麥克風", async () => {
  const trigger = show([{
    deviceKey: "asio:test", name: "ASIO", maxIn: 2, maxOut: 2,
    sampleRates: [48000], currentSampleRate: 48000, minBufferSize: 128,
    maxBufferSize: 512, preferredBufferSize: 256, bufferSizes: [128, 256, 512],
    inputNames: ["Input L", "Input R"], outputNames: ["Output L", "Output R"],
  }]);
  const values = await openMenu(trigger);
  expect(values).toEqual(["", "0", "m0", "m1", "mic:default", "mic:usb-mic"]);
  expect(document.querySelector(".source-popup")!.textContent).toContain("ASIO");
  for (const [value, source] of [
    ["0", { type: "asioIn", channel: 0 }],
    ["m1", { type: "asioIn", channel: 1, mono: true }],
  ] as const) {
    pick(value);
    await settle();
    expect(command).toHaveBeenLastCalledWith("track_set_source", { trackId: 42, source });
  }
});
