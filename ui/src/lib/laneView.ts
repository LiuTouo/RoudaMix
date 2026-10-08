// P1-I:水平帶 viewport virtualization + 拖曳插入位置的幾何計算(純函式)。
// strip 固定寬(CSS flex:0 0 250px、gap 16px),位置可純算 —— 不查 DOM,
// 虛擬化後(不在 DOM 的 strip)拖放計算依然正確。

export const STRIP_W = 250;
export const STRIP_GAP = 16;
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

/** 拖放預覽:第 i 張卡在插入位 pos 下的位移(px)。
 *  被拖曳卡滑到最終槽位 L = pos <= from ? pos : pos - 1;
 *  from < i < pos 左移一個 pitch、pos <= i < from 右移一個 pitch。
 *  from = -1 表外部插入(跨軌複製):pos 以下整批讓位一個 pitch。 */
export function dragShift(i: number, from: number, pos: number, pitch = STRIP_PITCH): number {
  if (from < 0) return i >= pos ? pitch : 0;
  if (i === from) return ((pos <= from ? pos : pos - 1) - from) * pitch;
  if (i > from && i < pos) return -pitch;
  if (i >= pos && i < from) return pitch;
  return 0;
}

/** 拖放插入位(垂直清單版):第一個「layout 中線在指標下方」的 index;都沒有 = count(尾端)。
 *  baseTop = 未 transform 的清單頂端 + 首列 layout 偏移;列距/列高須在拿起當下
 *  量好傳入 —— 拖曳中即時量 rect 會量到被預覽位移/transition 的列,與位移互相
 *  回饋,落點來回亂跳。 */
export function dropPosFromY(
  clientY: number,
  baseTop: number,
  rowH: number,
  pitch: number,
  count: number,
): number {
  if (count <= 0 || pitch <= 0) return 0; // 空清單 = 插在開頭
  const y = clientY - baseTop - rowH / 2;
  return Math.max(0, Math.min(count, Math.floor(y / pitch) + 1));
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
