import assert from "node:assert";
import { after, test } from "node:test";
import { createServer } from "vite";
import type { RackSlot, Track } from "./types.ts";

const vite = await createServer({ server: { middlewareMode: true }, appType: "custom" });
const [{ default: App }, { default: TrackStrip }, { render }] = await Promise.all([
  vite.ssrLoadModule("/src/App.svelte"),
  vite.ssrLoadModule("/src/lib/TrackStrip.svelte"),
  vite.ssrLoadModule("svelte/server"),
]);

after(() => vite.close());

const slot: RackSlot = {
  instanceId: 7,
  name: "Synth",
  pluginPath: "synth.vst3",
  classId: "synth",
  bypassed: false,
  monitorBypassed: false,
  params: [],
  availability: "ok",
  runtimeState: "active",
  monitorState: "active",
};

const track: Track = {
  trackId: 1,
  kind: "output",
  name: "Stream",
  color: 0,
  source: null,
  dests: [],
  output: null,
  gain: 1,
  mute: false,
  plugins: [slot],
  latencyPolicy: "fullPdc",
};

const stripProps = (latencyEnabled: boolean) => ({
  track,
  tracks: [track],
  devices: [],
  selectedDeviceKey: "",
  strips: undefined,
  latencyEnabled,
  onCancelScan: () => {},
  openMenu: () => {},
});

test("App 對外渲染的掃描入口位於頂欄", () => {
  const body = render(App).body;

  assert.match(body, /<header[\s\S]*?<button[^>]*>掃描 VST<\/button>/);
});

test("TrackStrip 依 capability 顯示 Monitor Bypass 與 Output Latency Policy", () => {
  const disabled = render(TrackStrip, { props: stripProps(false) }).body;
  const enabled = render(TrackStrip, { props: stripProps(true) }).body;

  assert.doesNotMatch(disabled, /aria-label="啟用 Synth 的 Monitor Bypass"/);
  assert.match(enabled, /aria-label="啟用 Synth 的 Monitor Bypass"/);
  assert.match(enabled, /aria-label="輸出軌 Stream 的延遲政策"/);
});

test("VST 機架以可操作名稱呈現 GUI 入口，加入按鈕不承擔重新掃描", () => {
  const body = render(TrackStrip, { props: stripProps(true) }).body;

  assert.match(body, /<span[^>]*role="button"[^>]*tabindex="0"[^>]*>Synth<\/span>/);
  assert.match(body, /<button[^>]*>＋ 加入<\/button>/);
  assert.doesNotMatch(body, />GUI<\/button>/);
  assert.doesNotMatch(body, />重新掃描<\/button>/);
});
