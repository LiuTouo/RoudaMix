// P1-I:水平帶 viewport virtualization + 拖曳插入位置的幾何計算(純函式)。
// strip 固定寬(CSS flex:0 0 250px、gap 8px),位置可純算 —— 不查 DOM,
// 虛擬化後(不在 DOM 的 strip)拖放計算依然正確。

export const STRIP_W = 250;
export const STRIP_GAP = 8;
export const STRIP_PITCH = STRIP_W + STRIP_GAP;

/** [start, end) = 要 render 的 index 範圍(含 overscan;夾 [0, count]) */
export function visibleRange(
  scrollLeft: number,
  viewportW: number,
  count: number,
  overscan = 3,
  pitch = STRIP_PITCH,
): { start: number; end: number } {
  if (count === 0) return { start: 0, end: 0 };
  // 第一個「右緣 > scrollLeft」的 index
  let start = Math.floor(scrollLeft / pitch);
  if (start < 0) start = 0;
  if (start * pitch + STRIP_W <= scrollLeft) start++;
  // 第一個「左緣 >= scrollLeft + viewportW」的 index
  let end = Math.ceil((scrollLeft + viewportW) / pitch) + 1;
  start = Math.max(0, start - overscan);
  end = Math.min(count, end + overscan);
  if (start > end) start = end;
  return { start, end };
}

/** 左右 placeholder 寬度(撐住捲軸;虛擬化容器用 flex gap,spacer 自身會多一個
 *  gap —— 校正:寬 = n*pitch - gap,n = 0 時 = 0) */
export function spacerWidths(
  start: number,
  end: number,
  count: number,
  pitch = STRIP_PITCH,
  gap = STRIP_GAP,
): { left: number; right: number } {
  return {
    left: start === 0 ? 0 : Math.max(0, start * pitch - gap),
    right: end >= count ? 0 : Math.max(0, (count - end) * pitch - gap),
  };
}

/** 拖放插入位:第一個「中心點在指標右側」的 index;都沒有 = count(尾端)。
 *  clientX = 指標視窗座標;laneLeft = lane 元素 getBoundingClientRect().left */
export function dropPosFromX(
  clientX: number,
  laneLeft: number,
  scrollLeft: number,
  count: number,
  pitch = STRIP_PITCH,
  w = STRIP_W,
): number {
  const x = clientX - laneLeft + scrollLeft; // lane 內容座標
  for (let i = 0; i < count; ++i) {
    if (i * pitch + w / 2 > x) return i;
  }
  return count;
}

/** P1-N 刪除確認用:master 絕對索引( lane 相對位置 → tracks 陣列索引)。
 *  帶內拖放語意:pos >= arrLen = 插在帶內最後一條之後;pos > dragIdx = 先移除
 *  造成的左移要補回(與 App 原邏輯相同,抽成純函式可測)。 */
export function laneDropToMasterIndex(
  pos: number,
  dragIdx: number,
  idToMasterIdx: (laneIdx: number) => number,
  laneLen: number,
  masterLen: number,
): number {
  let target =
    pos >= laneLen
      ? idToMasterIdx(laneLen - 1) + 1
      : idToMasterIdx(pos);
  if (pos > dragIdx) target -= 1;
  return Math.max(0, Math.min(target, masterLen - 1));
}
