import assert from "node:assert";
import { test } from "node:test";
import { reorderLane } from "./laneOrder.ts";

test("lane drop 以來源 id 與插入位置輸出新的 master 順序", () => {
  assert.deepEqual(reorderLane([10, 11, 12, 13], [10, 11, 12, 13], 12, 0), [
    12, 10, 11, 13,
  ]);
  assert.deepEqual(reorderLane([10, 11, 12, 13], [10, 11, 12, 13], 11, 4), [
    10, 12, 13, 11,
  ]);
});

test("先移除來源再修正向右插入位置，拖到自己相鄰位置維持原順序", () => {
  const order = [10, 11, 12, 13];

  assert.deepEqual(reorderLane(order, order, 10, 1), order);
  assert.deepEqual(reorderLane(order, order, 10, 3), [11, 12, 10, 13]);
  assert.deepEqual(order, [10, 11, 12, 13], "不得修改輸入陣列");
});

test("只在指定 lane 內排序並保留其他群組位置", () => {
  const master = [90, 10, 11, 91];
  const lane = [10, 11];

  assert.deepEqual(reorderLane(master, lane, 11, 0), [90, 11, 10, 91]);
  assert.deepEqual(reorderLane(master, lane, 10, 2), [90, 11, 10, 91]);
});

test("空 lane、未知來源或不完整 lane 都安全地維持原順序", () => {
  const master = [10, 11, 12];

  assert.deepEqual(reorderLane(master, [], 10, 0), master);
  assert.deepEqual(reorderLane(master, [10, 11], 12, 0), master);
  assert.deepEqual(reorderLane(master, [10, 99], 10, 1), master);
});

test("超出範圍的目標位置會夾到 lane 頭尾", () => {
  const order = [10, 11, 12];

  assert.deepEqual(reorderLane(order, order, 12, -5), [12, 10, 11]);
  assert.deepEqual(reorderLane(order, order, 10, 99), [11, 12, 10]);
});
