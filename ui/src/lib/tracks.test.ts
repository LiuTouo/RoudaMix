import assert from "node:assert";
import { test } from "node:test";
import { destCandidates, stripEngineOut, stripOfPlugin, stripOfTrack } from "./tracks.ts";
import type { MeterStrip, TelemetryStripIdentity, Track } from "./types.ts";

const meter = (peakL: number, instanceId: number, kind: number): MeterStrip => ({
  instanceId,
  kind,
  peakL,
  peakR: peakL,
  rmsL: peakL,
  rmsR: peakL,
});

test("meter 對應完全由 snapshot strip table 的 id 驅動", () => {
  const strips = [
    meter(0.1, 404, 40),
    meter(0.2, 11, 30),
    meter(0.3, 0xffffffff, 10),
    meter(0.4, 7, 20),
  ];
  const table: TelemetryStripIdentity[] = [
    { id: 2, kind: 10, trackId: null, instanceId: null },
    { id: 3, kind: 20, trackId: 7, instanceId: null },
    { id: 1, kind: 30, trackId: 7, instanceId: 11 },
  ];
  const view = { table, strips };

  assert.equal(stripEngineOut(view), strips[2]);
  assert.equal(stripOfTrack(7, view), strips[3]);
  assert.equal(stripOfPlugin(11, view), strips[1]);
});

test("table 指向不存在的 SHM strip 時回傳 undefined", () => {
  const table: TelemetryStripIdentity[] = [
    { id: 9, kind: 20, trackId: 7, instanceId: null },
  ];

  const view = { table, strips: [meter(0.1, 7, 20)] };
  assert.equal(stripOfTrack(7, view), undefined);
  assert.equal(stripOfTrack(8, view), undefined);
});

test("graph mutation 後丟棄與新 table 身分不符的舊 meter frame", () => {
  const table: TelemetryStripIdentity[] = [
    { id: 1, kind: 20, trackId: 8, instanceId: null },
    { id: 2, kind: 30, trackId: 8, instanceId: 12 },
  ];
  const stale = [meter(0.1, 0xffffffff, 10), meter(0.2, 7, 20), meter(0.3, 11, 30)];
  const view = { table, strips: stale };

  assert.equal(stripOfTrack(8, view), undefined);
  assert.equal(stripOfPlugin(12, view), undefined);
});

// #11 路由目的地限縮:清單只列可接收路由的軌道(fx / output)
const mkTrack = (trackId: number, kind: Track["kind"]): Track => ({
  trackId,
  kind,
  name: "T" + trackId,
  color: 0,
  source: null,
  dests: [],
  output: null,
  gain: 1,
  mute: false,
  plugins: [],
});

test("#11 路由目的地清單排除來源軌(audio/app)與自身,保留 fx/output 與 master 順序", () => {
  const tracks: Track[] = [
    mkTrack(1, "audio"),
    mkTrack(2, "app"),
    mkTrack(3, "fx"),
    mkTrack(4, "output"),
    mkTrack(5, "output"),
  ];
  const candidates = destCandidates(tracks, 4);
  assert.deepEqual(candidates.map((t) => t.trackId), [3, 5]);
});

test("#11 全為來源軌時清單為空(驅動空狀態說明)", () => {
  const tracks: Track[] = [mkTrack(1, "audio"), mkTrack(2, "app")];
  assert.deepEqual(destCandidates(tracks, 1), []);
});
