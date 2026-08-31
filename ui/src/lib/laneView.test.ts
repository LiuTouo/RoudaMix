// P1-I 測試:視窗計算、spacer 幾何、虛擬化下的拖放插入位
import { test } from "node:test";
import assert from "node:assert";
import { visibleRange, spacerWidths, dropPosFromX, laneDropToMasterIndex, STRIP_PITCH, STRIP_W } from "./laneView.ts";

const P = STRIP_PITCH; // 258

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
  const mid = (end - start) * STRIP_W + (end - start - 1) * 8;
  // 全量寬 = count*w + (count-1)*gap;spacer 校正後總和應等於全量
  const total = count * STRIP_W + (count - 1) * 8;
  assert.equal(left + 8 + mid + 8 + right, total);
  // 邊界:全 render 無 spacer
  assert.deepEqual(spacerWidths(0, count, count), { left: 0, right: 0 });
});

test("dropPosFromX:中心點判插入位;指標過尾 = count", () => {
  // lane 在視窗 x=100、未捲動:內容座標 = clientX-100。第 0 條中心 = 125
  assert.equal(dropPosFromX(200, 100, 0, 5), 0); // 內容 100 < 125 → 0
  assert.equal(dropPosFromX(230, 100, 0, 5), 1); // 內容 130 > 125 → 1
  // 捲動 258:內容座標 300 的指標 = 視窗 100+300-258 = 142(第 0 條中心之後)
  assert.equal(dropPosFromX(142, 100, 258, 5), 1);
  // 指標在最後一條中心之後 → count
  assert.equal(dropPosFromX(100 + 4 * P + 200, 100, 0, 5), 5);
});

test("laneDropToMasterIndex:帶內位置 → master 絕對索引(先移除左移補回)", () => {
  // 帶內 4 條在 master 0..3(idToMasterIdx = 恆等)
  const id = (i: number) => i;
  assert.equal(laneDropToMasterIndex(0, 2, id, 4, 4), 0); // drag2 移到頭
  assert.equal(laneDropToMasterIndex(4, 2, id, 4, 4), 3); // drag2 移到尾(pos4>drag2 → 4-1=3)
  assert.equal(laneDropToMasterIndex(1, 0, id, 4, 4), 0); // drag0 插在自己後一位 = 原地(erase 左移)
  assert.equal(laneDropToMasterIndex(3, 0, id, 4, 4), 2); // drag0 移到 index2 前 → master 2
  // 輸入帶在 master 有位移:輸入 2 條在 master[5,6],輸出軌在 [7](master 共 8 條)。
  // 帶尾語意 = 插在帶內最後一條之後、輸出軌之前:drag 是帶尾 → erase 左移補回
  const id2 = (i: number) => 5 + i;
  assert.equal(laneDropToMasterIndex(2, 1, id2, 2, 8), 6); // drag=帶尾原地不動的等價位
  assert.equal(laneDropToMasterIndex(1, 0, id2, 2, 8), 5); // drag=帶頭移到帶中 → master 5
});
