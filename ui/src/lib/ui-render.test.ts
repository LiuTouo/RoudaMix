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

const stripProps = (latencyEnabled: boolean, plugins = track.plugins) => ({
  track: { ...track, plugins },
  tracks: [{ ...track, plugins }],
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

test("TrackStrip 依 capability 顯示 Monitor Bypass 狀態與 Output Latency Policy", () => {
  const plugins = [{ ...slot, monitorBypassed: true }];
  const disabled = render(TrackStrip, { props: stripProps(false, plugins) }).body;
  const enabled = render(TrackStrip, { props: stripProps(true, plugins) }).body;

  assert.doesNotMatch(disabled, /class="monitor-badge/);
  assert.match(enabled, /class="monitor-badge/);
  assert.match(enabled, /aria-label="輸出軌 Stream 低延遲"/);
  assert.doesNotMatch(enabled, /<option value="fullPdc">/);
});

test("VST 機架以可操作名稱呈現 GUI 入口，加入按鈕不承擔重新掃描", () => {
  const body = render(TrackStrip, { props: stripProps(true) }).body;

  assert.match(body, /<span[^>]*role="button"[^>]*tabindex="0"[^>]*>Synth<\/span>/);
  assert.match(body, /<button[^>]*>\+ 加入<\/button>/);
  assert.doesNotMatch(body, />GUI<\/button>/);
  assert.doesNotMatch(body, />重新掃描<\/button>/);
});

test("the production App renders C with real labels, one name rail and all audio controls", () => {
  const appBody = render(App).body;
  assert.match(appBody, /class="audio-console/);
  assert.doesNotMatch(appBody, /ROUDAMIX \/ CONSOLE|MASTER \/ BUS|coltitle/);
  const consoleBody = render(TrackStrip, { props: stripProps(true) }).body;
  assert.doesNotMatch(consoleBody, /console-zone|console-channel-heading|STEREO|LOW LATENCY|>DEVICE</);
  assert.match(consoleBody, />裝置</);
  assert.match(consoleBody, />送至</);
  assert.match(consoleBody, />低延遲</);
  assert.equal([...consoleBody.matchAll(/>Stream<\/button>/g)].length, 1);
  assert.match(consoleBody, /VST \(1\)/);
  assert.match(consoleBody, />\+ 加入</);
  assert.match(consoleBody, /console-gain[^>]*>100%/);
  assert.match(consoleBody, /data-console-kind="output"/);
  assert.doesNotMatch(consoleBody, /\stitle="/);
  assert.equal([...consoleBody.matchAll(/aria-label="靜音 Stream"/g)].length, 1);
  assert.equal([...consoleBody.matchAll(/aria-label="Bypass Stream 全部 plugin"/g)].length, 1);
  assert.equal([...consoleBody.matchAll(/type="range"/g)].length, 1);
  assert.equal([...consoleBody.matchAll(/<canvas\b/g)].length, 1);
});

test("渲染輸出的提示一律走 data-tooltip，不出現原生 title 屬性", () => {
  const appBody = render(App).body;
  const stripBody = render(TrackStrip, { props: stripProps(true) }).body;

  assert.match(stripBody, /data-tooltip=/);
  assert.doesNotMatch(appBody, /\stitle="/);
  assert.doesNotMatch(stripBody, /\stitle="/);
});

test("插件列只在 Monitor Bypass 已啟用時顯示非按鈕 MB 標記，不再常駐移除或 MB 按鈕", () => {
  const off = render(TrackStrip, { props: stripProps(true) }).body;
  const on = render(TrackStrip, { props: stripProps(true, [{ ...slot, monitorBypassed: true }]) }).body;
  assert.doesNotMatch(off, /class="monitor-badge/);
  assert.match(on, /<span[^>]*class="monitor-badge[^>]*role="img"[^>]*aria-label="監聽略過（Monitor Bypass）已啟用"[^>]*>MB<\/span>/);
  for (const body of [off, on]) {
    assert.doesNotMatch(body, /<button[^>]*(?:monitor-bypass|移除 plugin)/);
    assert.match(body, /data-tooltip="Synth — 單擊開啟 plugin/);
    assert.doesNotMatch(body, /\{s\.name\}/);
  }
});

test("音軌標頭依角色與來源顯示圖示，名稱色條只出現一次且在來源之前", () => {
  const cases: { patch: Partial<Track>; label: string }[] = [
    { patch: { systemRole: "monitor" }, label: "監聽輸出" },
    { patch: { systemRole: "stream" }, label: "串流輸出" },
    { patch: {}, label: "音訊輸出" },
    { patch: { kind: "fx" }, label: "效果音軌" },
    { patch: { kind: "app" }, label: "應用程式音軌" },
    { patch: { kind: "audio", source: { type: "wasapiIn" } }, label: "麥克風輸入" },
    { patch: { kind: "audio", source: { type: "sine" } }, label: "正弦波來源" },
    { patch: { kind: "audio", source: { type: "asioIn", channel: 0 } }, label: "音訊輸入" },
    { patch: { kind: "audio", source: null }, label: "音訊輸入" },
  ];
  for (const { patch, label } of cases) {
    const props = stripProps(false);
    props.track = { ...props.track, ...patch };
    props.tracks = [props.track];
    const body = render(TrackStrip, { props }).body;
    const head = body.indexOf('class="head');
    const icon = body.indexOf('class="track-icon');
    const rail = body.indexOf('class="colorbar');
    const source = body.indexOf('class="row source-row');
    assert.ok(head < icon && icon < rail && rail < source, label);
    assert.match(body, new RegExp(`aria-label="${label}"`));
    assert.match(body.slice(head, rail), /--icon-color: #[0-9a-f]{6}/); // 圖示線條用音軌自訂色
    assert.equal([...body.matchAll(/class="colorbar/g)].length, 1);
    assert.match(body.slice(rail, source), /console-gain[^>]*>100%/);
    assert.match(body.slice(rail, source), /aria-label="軌道 Stream 的顏色"/);
    assert.match(body.slice(rail, source), /aria-label="軌道名稱:Stream\(點擊改名\)"/);
    if (patch.systemRole) {
      assert.doesNotMatch(body, /aria-label="刪除軌道/);
      assert.match(body.slice(rail, source), patch.systemRole === "monitor" ? />MON<\/span>/ : />STR<\/span>/);
    } else {
      assert.match(body.slice(rail, source), /aria-label="刪除軌道/);
    }
  }
});

test("靜態 index.html 同樣不出現原生 title 屬性（<title> 元素除外）", () => {
  const html = readFileSync(new URL("../../index.html", import.meta.url), "utf8");
  assert.match(html, /<title>/);
  assert.doesNotMatch(html, /\stitle="/);
});
