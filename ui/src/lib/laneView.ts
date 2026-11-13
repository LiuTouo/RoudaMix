// 水平帶 viewport virtualization + 拖放插入位置的幾何計算(純函式)。
// 槽寬可變:軌條固定寬 STRIP_W(CSS .strip flex:0 0 同步,console-containers
// 測試把關),開 VST sidecar 的軌再多佔 SIDE_GAP + SIDE_W(sidecar 與軌同寬)。
// 所有位置由 laneLayout prefix-sum 純算 —— 不查 DOM,虛擬化後(不在 DOM 的
// strip)拖放計算依然正確。

export const STRIP_W = 176;
export const STRIP_GAP = 16;
export const SIDE_W = STRIP_W; // VST sidecar 與軌條同寬
export const SIDE_GAP = 8; // 軌條與 sidecar 的間隙(.track-slot 內)

export interface LaneLayout {
  x: number[]; // 各槽左緣(lane 內容座標)
  w: number[]; // 各槽寬(軌條 [+ sidecar])
  total: number; // 內容總寬(撐住捲軸)
}

export const slotW = (open: boolean): number =>
  open ? STRIP_W + SIDE_GAP + SIDE_W : STRIP_W;

/** open[i] = 第 i 軌的 sidecar 是否展開;回傳全槽位置(純算) */
export function laneLayout(open: readonly boolean[]): LaneLayout {
  const n = open.length;
  const x: number[] = new Array(n);
  const w: number[] = new Array(n);
  let cx = 0;
  for (let i = 0; i < n; i++) {
    x[i] = cx;
    w[i] = slotW(open[i]);
    cx += w[i] + STRIP_GAP;
  }
  return { x, w, total: n === 0 ? 0 : x[n - 1] + w[n - 1] };
}

/** [start, end) = 要 render 的 index 範圍(含 overscan;夾 [0, count]) */
export function visibleRange(
  scrollLeft: number,
  viewportW: number,
  layout: LaneLayout,
  overscan = 3,
): { start: number; end: number } {
  const count = layout.x.length;
  if (count === 0) return { start: 0, end: 0 };
  // 第一個「右緣 > scrollLeft」的 index
  let start = 0;
  while (start < count && layout.x[start] + layout.w[start] <= scrollLeft) start++;
  // 第一個「左緣 >= scrollLeft + viewportW」的 index
  let end = start;
  while (end < count && layout.x[end] < scrollLeft + viewportW) end++;
  start = Math.max(0, start - overscan);
  end = Math.min(count, end + overscan);
  if (start > end) start = end;
  return { start, end };
}

/** 左右 placeholder 寬度(撐住捲軸;虛擬化容器用 flex gap,spacer 與首/末
 *  render 槽之間各多一個 gap —— 校正:扣一個 STRIP_GAP) */
export function spacerWidths(
  start: number,
  end: number,
  layout: LaneLayout,
): { left: number; right: number } {
  const count = layout.x.length;
  return {
    left: start === 0 ? 0 : Math.max(0, layout.x[start] - STRIP_GAP),
    right: end >= count ? 0 : Math.max(0, layout.total - layout.x[end]),
  };
}

/** 拖放插入位(水平帶):第一個「槽中心在指標右側」的 index;都沒有 = count(尾端)。
 *  clientX = 指標視窗座標;laneLeft = lane 元素 getBoundingClientRect().left */
export function dropPosFromX(
  clientX: number,
  laneLeft: number,
  scrollLeft: number,
  layout: LaneLayout,
): number {
  const count = layout.x.length;
  const x = clientX - laneLeft + scrollLeft; // lane 內容座標
  for (let i = 0; i < count; ++i) {
    if (layout.x[i] + layout.w[i] / 2 > x) return i;
  }
  return count;
}

/** 拖軌預覽:卡片 from 搬到插入位 pos 後各卡位移(px);槽寬跟著卡片走
 *  (sidecar 隨軌移動)。pos 語意同 dropPosFromX(插入 index)。 */
export function moveShifts(layout: LaneLayout, from: number, pos: number): number[] {
  const n = layout.x.length;
  const shifts = new Array<number>(n).fill(0);
  if (from < 0 || from >= n) return shifts;
  const target = pos <= from ? pos : pos - 1;
  const order: number[] = [];
  for (let i = 0; i < n; i++) if (i !== from) order.push(i);
  order.splice(target, 0, from);
  let cx = 0;
  for (const idx of order) {
    shifts[idx] = cx - layout.x[idx];
    cx += layout.w[idx] + STRIP_GAP;
  }
  return shifts;
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

/** 垂直清單(插件鏈)的拖放預覽位移:固定列距 pitch。
 *  被拖曳列滑到最終槽位;from < i < pos 左移、pos <= i < from 右移一個 pitch。
 *  from = -1 表外部插入(跨軌複製):pos 以下整批讓位一個 pitch。 */
export function dragShift(i: number, from: number, pos: number, pitch: number): number {
  if (from < 0) return i >= pos ? pitch : 0;
  if (i === from) return ((pos <= from ? pos : pos - 1) - from) * pitch;
  if (i > from && i < pos) return -pitch;
  if (i >= pos && i < from) return pitch;
  return 0;
}
