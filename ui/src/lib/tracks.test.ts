import assert from "node:assert";
import { test } from "node:test";
import { stripEngineOut, stripOfPlugin, stripOfTrack } from "./tracks.ts";
import type { MeterStrip, TelemetryStripIdentity } from "./types.ts";

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

  assert.equal(stripEngineOut(table, strips), strips[2]);
  assert.equal(stripOfTrack(7, table, strips), strips[3]);
  assert.equal(stripOfPlugin(11, table, strips), strips[1]);
});

test("table 指向不存在的 SHM strip 時回傳 undefined", () => {
  const table: TelemetryStripIdentity[] = [
    { id: 9, kind: 20, trackId: 7, instanceId: null },
  ];

  assert.equal(stripOfTrack(7, table, [meter(0.1, 7, 20)]), undefined);
  assert.equal(stripOfTrack(8, table, [meter(0.1, 7, 20)]), undefined);
});

test("graph mutation 後丟棄與新 table 身分不符的舊 meter frame", () => {
  const table: TelemetryStripIdentity[] = [
    { id: 1, kind: 20, trackId: 8, instanceId: null },
    { id: 2, kind: 30, trackId: 8, instanceId: 12 },
  ];
  const stale = [meter(0.1, 0xffffffff, 10), meter(0.2, 7, 20), meter(0.3, 11, 30)];

  assert.equal(stripOfTrack(8, table, stale), undefined);
  assert.equal(stripOfPlugin(12, table, stale), undefined);
});
