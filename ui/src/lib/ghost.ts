// 拖曳「拿起」層:真實 DOM clone 跟著指標走 —— 完全不透明、完整原高、帶陰影。
// 不用 dt.setDragImage(元素快照):Windows 會對該點陣圖套系統 alpha,怎麼調都黯淡;
// 改掛 1×1 透明拖曳影像壓掉 OS 預設 ghost,畫面上只留自繪跟隨層。
const TRANSPARENT_PIXEL = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

let layer: HTMLElement | null = null;
let grabX = 30;
let grabY = 16;
// 幽靈目前位置（跟隨指標）與拿起時原軌位置；settleDragGhost 落定用
let curX = 0;
let curY = 0;
let origin: { left: number; top: number } | null = null;

function followPointer(e: DragEvent): void {
  if (!layer) return;
  curX = e.clientX - grabX;
  curY = e.clientY - grabY;
  layer.style.transform = `translate(${curX}px, ${curY}px)`;
}

export function mountDragGhost(
  dt: DataTransfer,
  src: HTMLElement,
  width: number,
  grab?: { x: number; y: number }, // 按住點在軌內偏移:拿起後該點仍釘在指標下,不跳位
): void {
  removeDragGhost();
  const r = src.getBoundingClientRect();
  const w = Math.round(r.width) || width;
  const h = Math.round(r.height);
  grabX = grab && Number.isFinite(grab.x) ? Math.max(0, Math.min(grab.x, w)) : 30;
  grabY = grab && Number.isFinite(grab.y) ? Math.max(0, Math.min(grab.y, h)) : 16;
  layer = document.createElement("div");
  layer.setAttribute("aria-hidden", "true");
  layer.style.cssText =
    `position:fixed;left:0;top:0;width:${w}px;height:${h}px;pointer-events:none;` +
    "z-index:9999;will-change:transform;";
  curX = r.left;
  curY = r.top;
  origin = { left: r.left, top: r.top };
  layer.style.transform = `translate(${r.left}px, ${r.top}px)`; // 起始 = 原軌原位(從原地拿起)
  const ghost = src.cloneNode(true) as HTMLElement;
  // 拖曳中的暫態 class 不能進 ghost(原件的淡化/滑動預覽會一起被帶走)
  ghost.classList.remove("dragging", "drag-active");
  ghost.classList.add("drag-ghost");
  // 原軌被 lane 拉伸到全高;clone 預設塌成內容高,鎖回原高才是「整條實體」
  ghost.style.height = `${h}px`;
  // cloneNode 不會複製 canvas 點陣圖(meter 會空白),照抄像素
  const from = src.querySelectorAll("canvas");
  const to = ghost.querySelectorAll("canvas");
  for (let i = 0; i < from.length; ++i) {
    const dst = to[i]!;
    dst.getContext("2d")?.drawImage(from[i]!, 0, 0, dst.width || from[i]!.width, dst.height || from[i]!.height);
  }
  layer.appendChild(ghost);
  document.body.appendChild(layer);
  window.addEventListener("drag", followPointer, true);
  window.addEventListener("dragover", followPointer, true);
  // 壓掉 OS 半透明預設拖曳影像
  const dot = document.createElement("canvas");
  dot.width = 1;
  dot.height = 1;
  dt.setDragImage(dot, 0, 0);
}

export function removeDragGhost(): void {
  window.removeEventListener("drag", followPointer, true);
  window.removeEventListener("dragover", followPointer, true);
  layer?.remove();
  layer = null;
}

/**
 * 放手落定：幽靈從目前指標位置滑向目標槽位（找不到＝虛擬化視窗外或原位）
 * 後淡出移除，讓「拿起→放下」閉環；排序本身仍由狀態更新瞬間完成。
 * 一律播放，不連動 OS 動畫設定（ADR 0007）。
 */
export function settleDragGhost(target: HTMLElement | null): void {
  if (!layer) return;
  const el = layer;
  layer = null; // 移交動畫層；removeDragGhost() 之後僅清 listener
  removeDragGhost();
  const r = target?.getBoundingClientRect();
  const dest = r ?? origin;
  if (!dest) {
    el.remove();
    return;
  }
  el.style.transition =
    "transform 160ms cubic-bezier(0.16, 1, 0.3, 1), opacity 160ms ease-out";
  getComputedStyle(el).transform; // 強制 flush：transition 從目前指標位置起跑
  el.style.transform = `translate(${dest.left}px, ${dest.top}px)`;
  el.style.opacity = "0";
  el.addEventListener("transitionend", () => el.remove(), { once: true });
  window.setTimeout(() => el.remove(), 240); // 保險：transitionend 沒射也收掉
}
