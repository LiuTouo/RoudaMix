// 水平帶幾何測試:可變槽寬(sidecar)layout、視窗計算、spacer 寬度、
// 虛擬化下的拖放插入位與搬移預覽;垂直清單(dropPosFromY/dragShift)同舊。
import { test } from "node:test";
import assert from "node:assert";
import {
  laneLayout,
  visibleRange,
  spacerWidths,
  dropPosFromX,
  dropPosFromY,
  dragShift,
  moveShifts,
  slotW,
  STRIP_W,
  STRIP_GAP,
  SIDE_W,
  SIDE_GAP,
} from "./laneView.ts";

// 全收 = 固定 pitch(舊模型相容):[關,關,關,關]
const closed4 = laneLayout([false, false, false, false]);
// 混合:第 1、3 軌開 sidecar → 槽寬 [W, W+S, W, W+S]
const mixed = laneLayout([false, true, false, true]);

test("laneLayout:prefix-sum 位置;開 sidecar 槽 = W + SIDE_GAP + SIDE_W", () => {
  assert.equal(slotW(false), STRIP_W);
  assert.equal(slotW(true), STRIP_W + SIDE_GAP + SIDE_W);
  const open = [false, true, false];
  const l = laneLayout(open);
  assert.deepEqual(l.x, [0, STRIP_W + STRIP_GAP, STRIP_W + STRIP_GAP + slotW(true) + STRIP_GAP]);
  assert.deepEqual(l.w, [STRIP_W, STRIP_W + SIDE_GAP + SIDE_W, STRIP_W]);
  assert.equal(l.total, l.x[2] + l.w[2]);
  // 空清單
  assert.deepEqual(laneLayout([]), { x: [], w: [], total: 0 });
});

test("visibleRange:窗口 + overscan,夾 [0,count]", () => {
  // 固定 pitch 全收:scroll 0 視窗 2600 → 全部可見
  const r = visibleRange(0, 2600, laneLayout(new Array(10).fill(false)));
  assert.equal(r.start, 0);
  assert.equal(r.end, 10);
  // 捲到第 20 條(100 軌):start ≈ 20-3
  const r2 = visibleRange(20 * (STRIP_W + STRIP_GAP), 1032, laneLayout(new Array(100).fill(false)));
  assert.equal(r2.start, 17);
  // 空清單
  assert.deepEqual(visibleRange(0, 800, laneLayout([])), { start: 0, end: 0 });
});

test("visibleRange:部分露出的 strip 算可見;開 sidecar 的寬槽照樣吃到", () => {
  const opens = new Array(100).fill(false);
  opens[6] = true; // 第 6 槽較寬,確認窗口跨寬槽
  const l = laneLayout(opens);
  // scrollLeft 停在 strip 5 中段:strip 5 部分在左邊窗外
  const r = visibleRange(5 * (STRIP_W + STRIP_GAP) + STRIP_W - 10, 516, l, 0);
  assert.equal(r.start, 5);
  assert.ok(r.end >= 7); // 視窗蓋過寬槽 6
  // scrollLeft 恰在前一槽右緣 = 寬槽 6 左緣:寬槽是第一個可見
  const r2 = visibleRange(l.x[6], 100, l, 0);
  assert.equal(r2.start, 6);
});

test("spacerWidths:總寬守恆(spacer + render 區 + flex gap = 全量內容寬)", () => {
  const opens = [false, true, false, true, false, false, true, false, false, false];
  const l = laneLayout(opens);
  const start = 3, end = 7;
  const { left, right } = spacerWidths(start, end, l);
  // render 區寬(含 gap):槽 3..6 + 其間 gap
  const mid = l.x[end - 1] + l.w[end - 1] - l.x[start];
  // spacer 與首/末 render 槽各隔一個 gap → 總和 = total
  assert.equal(left + STRIP_GAP + mid + STRIP_GAP + right, l.total);
  // 邊界:全 render 無 spacer
  assert.deepEqual(spacerWidths(0, 10, l), { left: 0, right: 0 });
});

test("dropPosFromX:槽中心判插入位;指標過尾 = count", () => {
  // lane 在視窗 x=100、未捲動:內容座標 = clientX-100。第 0 槽中心 = W/2 = 88
  assert.equal(dropPosFromX(150, 100, 0, closed4), 0); // 內容 50 < 中心 → 0
  assert.equal(dropPosFromX(200, 100, 0, closed4), 1); // 內容 100 > 中心 → 1
  // 開 sidecar 的寬槽:中心 = x + w/2,mixed 槽 1 中心 = 156+144 = 300
  const c1 = mixed.x[1] + mixed.w[1] / 2;
  assert.equal(dropPosFromX(100 + c1 - 1, 100, 0, mixed), 1);
  assert.equal(dropPosFromX(100 + c1 + 1, 100, 0, mixed), 2);
  // 捲動一個 pitch(全收):內容座標 300 的指標 = 視窗 100+300-pitch
  const P = STRIP_W + STRIP_GAP;
  assert.equal(dropPosFromX(142, 100, P, closed4), 1);
  // 指標在最後一槽中心之後 → count
  assert.equal(dropPosFromX(100 + 3 * P + 300, 100, 0, closed4), 4);
});

test("moveShifts:搬移後各卡位移;槽寬(sidecar)跟著卡片走", () => {
  // [關0,開1,關2,開3] 搬 1 → pos=3(先移除左移,插在 2 之後)
  const s = moveShifts(mixed, 1, 3);
  // 新序 [0,2,1,3]:0 不動;2 左移「槽1寬+gap」;1 右移到 2 讓出的位置;3 不動
  assert.equal(s[0], 0);
  assert.equal(s[2], -(mixed.w[1] + STRIP_GAP));
  assert.equal(s[1], mixed.w[2] + STRIP_GAP);
  assert.equal(s[3], 0);
  // 插到最前 pos=0:前面卡右移自己槽寬+gap
  const s2 = moveShifts(mixed, 1, 0);
  assert.equal(s2[0], mixed.w[1] + STRIP_GAP);
  assert.equal(s2[1], -mixed.x[1]);
  // 原位相鄰(pos = from / from+1)= 不動
  assert.deepEqual(moveShifts(mixed, 1, 1), [0, 0, 0, 0]);
  assert.deepEqual(moveShifts(mixed, 1, 2), [0, 0, 0, 0]);
  // 拖著開 sidecar 的軌(3)插到 0:大家右移(寬槽)
  const s3 = moveShifts(mixed, 3, 0);
  assert.equal(s3[0], mixed.w[3] + STRIP_GAP);
  assert.equal(s3[2], mixed.w[3] + STRIP_GAP);
  assert.equal(s3[3], -mixed.x[3]);
});

test("dropPosFromY:中線規則,夾 [0,count];空清單插在開頭", () => {
  // 3 列、列距 50、列高 48、首列頂 100:中線 = 100 + i*50 + 24
  const base = 100;
  assert.equal(dropPosFromY(100 + 24, base, 48, 50, 3), 1); // 恰在第 0 列中線 → 插第 1 位
  assert.equal(dropPosFromY(100 + 23, base, 48, 50, 3), 0); // 中線上方 → 插最前
  assert.equal(dropPosFromY(100 + 74, base, 48, 50, 3), 2); // 第 1 列中線
  assert.equal(dropPosFromY(100 + 174, base, 48, 50, 3), 3); // 尾端空白 → 鏈尾
  assert.equal(dropPosFromY(0, base, 48, 50, 3), 0); // 清單上方 → 夾 0
  assert.equal(dropPosFromY(9999, base, 48, 50, 3), 3); // 遠低於尾端 → 夾 count
  assert.equal(dropPosFromY(100, base, 48, 50, 0), 0); // 空清單
  assert.equal(dropPosFromY(100, base, 48, 0, 3), 0); // 未量得列距 → 不誤判
});

test("dragShift:被拖曳列滑到最終槽位,中間列讓位一個 pitch", () => {
  const P = 50;
  // [A0,B1,C2,D3] 拖 B(1) 插到 pos=3 → [A,C,B,D]:B +1,C -1,D -1,A 0
  assert.equal(dragShift(1, 1, 3, P), P);
  assert.equal(dragShift(2, 1, 3, P), -P);
  assert.equal(dragShift(3, 1, 3, P), 0); // D 是插入點,不動
  assert.equal(dragShift(0, 1, 3, P), 0);
  // 插到最前 pos=0:拖 B 到 0,前面卡右移
  assert.equal(dragShift(0, 1, 0, P), P);
  assert.equal(dragShift(1, 1, 0, P), -P);
  // 插到尾端 pos=count:拖曳卡到最後,中間卡左移
  assert.equal(dragShift(1, 1, 4, P), 2 * P);
  assert.equal(dragShift(2, 1, 4, P), -P);
  assert.equal(dragShift(3, 1, 4, P), -P);
});

test("dragShift:原位相鄰全 0;外部插入讓位", () => {
  const P = 50;
  // 原位(pos=from / from+1)不改變排序 → 全 0
  assert.equal(dragShift(1, 1, 1, P), 0);
  assert.equal(dragShift(1, 1, 2, P), 0);
  assert.equal(dragShift(0, 1, 2, P), 0);
  assert.equal(dragShift(2, 1, 2, P), 0);
  // 外部插入(from=-1,跨軌複製):pos 以下整批右移讓位
  assert.equal(dragShift(0, -1, 1, P), 0);
  assert.equal(dragShift(1, -1, 1, P), P);
  assert.equal(dragShift(2, -1, 1, P), P);
  // pos=0 全讓位;pos 超過尾端無卡讓位
  assert.equal(dragShift(0, -1, 0, P), P);
  assert.equal(dragShift(2, -1, 3, P), 0);
});
