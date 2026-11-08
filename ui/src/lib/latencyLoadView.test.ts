// Drawer 負載觀察測試:EWMA 種子/收斂、history 上限、peak 視窗、驅逐已移除 instance
import { test } from "node:test";
import assert from "node:assert";
import { observePluginLoads, pluginLoadKey } from "./latencyLoadView.ts";
import type { MetersFrame } from "./types.ts";

type Load = NonNullable<MetersFrame["pluginLoads"]>[number];
const loads = (items: Array<Partial<Load>>): Load[] =>
  items.map((item) => ({ instanceId: 7, variant: 0, processLoad: 0.5, ...item }));

test("首輪以樣本種子 EWMA(非從 0 拉起),peak 同值", () => {
  const history = new Map<string, number[]>();
  const view = observePluginLoads(history, {}, loads([{ processLoad: 0.4 }]));
  assert.equal(view["7:0"].ewma, 0.4);
  assert.equal(view["7:0"].peak, 0.4);
  assert.deepEqual(history.get("7:0"), [0.4]);
});

test("次輪 EWMA 循序收斂,peak 取視窗最大", () => {
  const history = new Map<string, number[]>();
  const first = observePluginLoads(history, {}, loads([{ processLoad: 0.2 }]));
  const second = observePluginLoads(history, first, loads([{ processLoad: 1.0 }]));
  const expected = 0.2 + (1.0 - 0.2) / 30;
  assert.ok(Math.abs(second["7:0"].ewma - expected) < 1e-12);
  assert.equal(second["7:0"].peak, 1.0);
});

test("history 每鍵上限 150 筆(shift 最舊)", () => {
  const history = new Map<string, number[]>();
  let view: Record<string, { ewma: number; peak: number }> = {};
  for (let i = 0; i < 160; i++) view = observePluginLoads(history, view, loads([{ processLoad: 0.1 }]));
  assert.equal(history.get("7:0").length, 150);
});

test("primary 與 monitor variant 分鍵互不干擾", () => {
  const history = new Map<string, number[]>();
  const view = observePluginLoads(history, {}, [
    { instanceId: 7, variant: 0, processLoad: 0.1 },
    { instanceId: 7, variant: 1, processLoad: 0.9 },
  ]);
  assert.deepEqual(Object.keys(view).sort(), ["7:0", "7:1"]);
  assert.equal(view[pluginLoadKey(7, 1)].ewma, 0.9);
});

test("消失的 instance 同時從 view 與 history 驅逐(防無限成長)", () => {
  const history = new Map<string, number[]>();
  const view = observePluginLoads(history, {}, loads([{ instanceId: 1 }, { instanceId: 2 }]));
  assert.equal(Object.keys(view).length, 2);
  const after = observePluginLoads(history, view, loads([{ instanceId: 2 }]));
  assert.deepEqual(Object.keys(after), ["2:0"]);
  assert.deepEqual([...history.keys()], ["2:0"]);
  // 空輪全清:Drawer 收到空圖不得殘留 stale 統計
  const emptied = observePluginLoads(history, after, []);
  assert.deepEqual(emptied, {});
  assert.equal(history.size, 0);
});
