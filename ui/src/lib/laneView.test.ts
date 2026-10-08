// P1-I 測試:視窗計算、spacer 幾何、虛擬化下的拖放插入位
import { test } from "node:test";
import assert from "node:assert";
import { visibleRange, spacerWidths, dropPosFromX, dragShift, STRIP_PITCH, STRIP_W, STRIP_GAP } from "./laneView.ts";

const P = STRIP_PITCH;

test("visibleRange:窗口 + overscan,夾 [0,count]", () => {
  // 前 10 條全部可見(scroll 0、視窗 2600):start 0,end = ceil(2600/258)+1 = 11 → 夾 10
  const r = visibleRange(0, 2600, 10);
  assert.equal(r.start, 0);
  assert.equal(r.end, 10);
  // 捲到第 20 條左右(100 軌):start ≈ 20-3,end ≈ 25+3
  const r2 = visibleRange(20 * P, 1032, 100);
  assert.equal(r2.start, 17); // 20 - 3
  assert.equal(r2.end, 28);   // ceil((20*258+1032)/258)+1 = 24+3 夾 100
  // 空清單
  assert.deepEqual(visibleRange(0, 800, 0), { start: 0, end: 0 });
});

test("visibleRange:部分露出的 strip 算可見", () => {
  // scrollLeft 停在 strip 5 中段:strip 5 部分在左邊窗外
  const r = visibleRange(5 * P + STRIP_W - 10, 516, 100, 0);
  assert.equal(r.start, 5);
});

test("spacerWidths:總寬守恆(spacer + render 區 = 全量內容寬)", () => {
  const count = 10;
  const start = 3, end = 7;
  const { left, right } = spacerWidths(start, end, count);
  // render 區寬(含 gap):strip 3..6 + 其間 gap
  const mid = (end - start) * STRIP_W + (end - start - 1) * STRIP_GAP;
  // 全量寬 = count*w + (count-1)*gap;spacer 校正後總和應等於全量
  const total = count * STRIP_W + (count - 1) * STRIP_GAP;
  assert.equal(left + STRIP_GAP + mid + STRIP_GAP + right, total);
  // 邊界:全 render 無 spacer
  assert.deepEqual(spacerWidths(0, count, count), { left: 0, right: 0 });
});

test("dropPosFromX:中心點判插入位;指標過尾 = count", () => {
  // lane 在視窗 x=100、未捲動:內容座標 = clientX-100。第 0 條中心 = 125
  assert.equal(dropPosFromX(200, 100, 0, 5), 0); // 內容 100 < 125 → 0
  assert.equal(dropPosFromX(230, 100, 0, 5), 1); // 內容 130 > 125 → 1
  // 捲動一個 pitch:內容座標 300 的指標 = 視窗 100+300-P = 142(第 0 條中心之後)
  assert.equal(dropPosFromX(142, 100, P, 5), 1);
  // 指標在最後一條中心之後 → count
  assert.equal(dropPosFromX(100 + 4 * P + 200, 100, 0, 5), 5);
});

test("dragShift:被拖曳卡滑到最終槽位,中間卡讓位一個 pitch", () => {
  // [A0,B1,C2,D3] 拖 B(1) 插到 pos=3 → [A,C,B,D]:B +1,C -1,D -1,A 0
  assert.equal(dragShift(1, 1, 3), P);
  assert.equal(dragShift(2, 1, 3), -P);
  assert.equal(dragShift(3, 1, 3), 0); // D 是插入點,不動
  assert.equal(dragShift(0, 1, 3), 0);
  // 插到最前 pos=0:拖 B 到 0,前面卡右移
  assert.equal(dragShift(0, 1, 0), P);
  assert.equal(dragShift(1, 1, 0), -P);
  // 插到尾端 pos=count:拖曳卡到最後,中間卡左移
  assert.equal(dragShift(1, 1, 4), 2 * P);
  assert.equal(dragShift(2, 1, 4), -P);
  assert.equal(dragShift(3, 1, 4), -P);
});

test("dragShift:原位相鄰全 0;外部插入讓位", () => {
  // 原位(pos=from / from+1)不改變排序 → 全 0
  assert.equal(dragShift(1, 1, 1), 0);
  assert.equal(dragShift(1, 1, 2), 0);
  assert.equal(dragShift(0, 1, 2), 0);
  assert.equal(dragShift(2, 1, 2), 0);
  // 外部插入(from=-1,跨軌複製):pos 以下整批右移讓位
  assert.equal(dragShift(0, -1, 1), 0);
  assert.equal(dragShift(1, -1, 1), P);
  assert.equal(dragShift(2, -1, 1), P);
  // pos=0 全讓位;pos 超過尾端無卡讓位
  assert.equal(dragShift(0, -1, 0), P);
  assert.equal(dragShift(2, -1, 3), 0);
});
