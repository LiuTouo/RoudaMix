import assert from "node:assert";
import { test } from "node:test";
import { stripEngineOut, stripOfPlugin, stripOfTrack } from "./tracks.ts";
import type { MeterStrip, TelemetryStripIdentity } from "./types.ts";

const meter = (peakL: number): MeterStrip => ({
  instanceId: 999,
  kind: 999,
  peakL,
  peakR: peakL,
  rmsL: peakL,
  rmsR: peakL,
});

test("meter 對應完全由 snapshot strip table 的 id 驅動", () => {
  const strips = [meter(0.1), meter(0.2), meter(0.3), meter(0.4)];
  const table: TelemetryStripIdentity[] = [
    { id: 2, kind: "engineOutput", trackId: null, instanceId: null },
    { id: 3, kind: "track", trackId: 7, instanceId: null },
    { id: 1, kind: "plugin", trackId: 7, instanceId: 11 },
  ];

  assert.equal(stripEngineOut(table, strips), strips[2]);
  assert.equal(stripOfTrack(7, table, strips), strips[3]);
  assert.equal(stripOfPlugin(11, table, strips), strips[1]);
});

test("table 指向不存在的 SHM strip 時回傳 undefined", () => {
  const table: TelemetryStripIdentity[] = [
    { id: 9, kind: "track", trackId: 7, instanceId: null },
  ];

  assert.equal(stripOfTrack(7, table, [meter(0.1)]), undefined);
  assert.equal(stripOfTrack(8, table, [meter(0.1)]), undefined);
});
