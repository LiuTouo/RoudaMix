import assert from "node:assert";
import { readFileSync } from "node:fs";
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
  meterView: {},
  latencyEnabled,
  onStartScan: async () => true,
  onCancelScan: () => {},
  openMenu: () => {},
});

test("App 頂欄不再有掃描入口", () => {
  const body = render(App).body;
  const headerEnd = body.indexOf("</header>");

  assert.ok(headerEnd > 0, "App 應渲染頂欄");
  assert.doesNotMatch(body.slice(0, headerEnd), />掃描 VST<\/button>/);
  assert.doesNotMatch(body.slice(0, headerEnd), />取消掃描/);
});

test("膠囊通知層獨立於頂欄之外，警示不再擠在頂欄", () => {
  const body = render(App).body;
  const headerEnd = body.indexOf("</header>");

  assert.ok(headerEnd > 0, "App 應渲染頂欄");
  const capsuleAt = body.indexOf("capsules");
  assert.ok(capsuleAt > headerEnd, "膠囊容器應位於頂欄之後");
  assert.match(body, /<div[^>]*class="capsules[^"]*"[^>]*>/);
  assert.doesNotMatch(body.slice(0, headerEnd), /class="notice/);
});

test("TrackStrip 依 capability 顯示 Monitor Bypass 與 Output Latency Policy", () => {
  const disabled = render(TrackStrip, { props: stripProps(false) }).body;
  const enabled = render(TrackStrip, { props: stripProps(true) }).body;

  assert.doesNotMatch(disabled, /aria-label="啟用 Synth 的 Monitor Bypass"/);
  assert.match(enabled, /aria-label="啟用 Synth 的 Monitor Bypass"/);
  assert.match(enabled, /aria-label="輸出軌 Stream 低延遲"/);
  assert.doesNotMatch(enabled, /<option value="fullPdc">/);
});

test("VST 機架以可操作名稱呈現 GUI 入口，加入按鈕不承擔重新掃描", () => {
  const body = render(TrackStrip, { props: stripProps(true) }).body;

  assert.match(body, /<span[^>]*role="button"[^>]*tabindex="0"[^>]*>Synth<\/span>/);
  assert.match(body, /<button[^>]*>＋ 加入<\/button>/);
  assert.doesNotMatch(body, />GUI<\/button>/);
  assert.doesNotMatch(body, />重新掃描<\/button>/);
});

test("渲染輸出的提示一律走 data-tooltip，不出現原生 title 屬性", () => {
  const appBody = render(App).body;
  const stripBody = render(TrackStrip, { props: stripProps(true) }).body;

  assert.match(stripBody, /data-tooltip=/);
  assert.doesNotMatch(appBody, /\stitle="/);
  assert.doesNotMatch(stripBody, /\stitle="/);
});

test("靜態 index.html 同樣不出現原生 title 屬性（<title> 元素除外）", () => {
  const html = readFileSync(new URL("../../index.html", import.meta.url), "utf8");
  assert.match(html, /<title>/);
  assert.doesNotMatch(html, /\stitle="/);
});
