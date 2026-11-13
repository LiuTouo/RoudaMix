// 拖曳「拿起」層:真實 DOM clone 跟著指標走 —— 完全不透明、完整原高、帶陰影。
// 不用 dt.setDragImage(元素快照):Windows 會對該點陣圖套系統 alpha,怎麼調都黯淡;
// 壓掉 OS 預設拖曳影像也不行全靠 canvas 版 setDragImage(Chromium 支援不全,
// 會退回 OS 半透明快照,跟自繪層叠成淡淡重影),改用預載透明 GIF Image —— 各家實測可用。
const TRANSPARENT_PIXEL = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
let blankDragImage: HTMLImageElement | null = null; // 惰性:node/SSR 測試環境無 Image 全域
function transparentDragImage(): HTMLImageElement {
  if (!blankDragImage) {
    blankDragImage = new Image();
    blankDragImage.src = TRANSPARENT_PIXEL;
  }
  return blankDragImage;
}

let layer: HTMLElement | null = null;
let grabX = 30;
let grabY = 16;
// 幽靈目前位置（跟隨指標）
let curX = 0;
let curY = 0;

// 寫入位置前防夾：幽靈完整留在視窗內（太長/太高釘在右/下緣，不會被螢幕邊緣裁掉）；
// 視窗比幽靈還小時釘在左/上緣。
function applyGhostPos(): void {
  if (!layer) return;
  const maxX = Math.max(0, window.innerWidth - layer.offsetWidth);
  const maxY = Math.max(0, window.innerHeight - layer.offsetHeight);
  curX = Math.max(0, Math.min(curX, maxX));
  curY = Math.max(0, Math.min(curY, maxY));
  layer.style.transform = `translate(${curX}px, ${curY}px)`;
}

function followPointer(e: DragEvent): void {
  if (!layer) return;
  // 測試/合成事件可能只有 clientY:無效座標保留上一次位置,不寫 NaN
  if (Number.isFinite(e.clientX)) curX = e.clientX - grabX;
  if (Number.isFinite(e.clientY)) curY = e.clientY - grabY;
  applyGhostPos();
}

export function mountDragGhost(
  dt: DataTransfer,
  src: HTMLElement,
  width: number,
  grab?: { x: number; y: number }, // 按住點在軌內偏移:拿起後該點仍釘在指標下,不跳位
  fit?: "content", // 小項目(機架列):寬度隨內容撐開(完整名稱不被窄欄/ellipsis 裁切)、高度自適應
): void {
  removeDragGhost();
  const r = src.getBoundingClientRect();
  const w = Math.round(r.width) || width;
  const h = Math.round(r.height);
  // 負 grab = 幽靈整體偏離游標(小項目如機架列,整個挪到游標右下才不會被游標蓋住)
  grabX = grab && Number.isFinite(grab.x) ? Math.min(grab.x, w) : 30;
  grabY = grab && Number.isFinite(grab.y) ? Math.min(grab.y, h) : 16;
  layer = document.createElement("div");
  layer.setAttribute("aria-hidden", "true");
  layer.style.cssText =
    `position:fixed;left:0;top:0;pointer-events:none;z-index:9999;will-change:transform;` +
    (fit === "content" ? `width:max-content;min-width:${w}px;` : `width:${w}px;height:${h}px;`);
  curX = r.left;
  curY = r.top;
  layer.style.transform = `translate(${r.left}px, ${r.top}px)`; // 起始 = 原軌原位(從原地拿起)
  const ghost = src.cloneNode(true) as HTMLElement;
  // 拖曳中的暫態 class 不能進 ghost(原件的淡化/滑動預覽會一起被帶走)
  ghost.classList.remove("dragging", "drag-active");
  ghost.classList.add("drag-ghost");
  // 拿起樣式走 inline（console 皮膚的 ID 選擇器會壓掉 class 規則）：
  // 專業工具語彙 = 卡片原樣呈現（自帶面板色/邊框/meter），不加 accent 色框（網頁感），
  // 只以微透明 + 懸浮投影表達「提起」；音軌卡與機架列幽靈統一
  ghost.style.cssText +=
    "opacity:0.92;box-shadow:0 6px 16px rgb(0 0 0 / 0.45),0 2px 5px rgb(0 0 0 / 0.35);";
  if (fit !== "content")
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
  // 掛進 .audio-console(非 body):clone 才吃得到 console 皮膚的 grid 佈局;
  // 掛 body 下會退回 scoped flex-column,名稱(overflow:hidden → min-height 0)
  // 被 flex-shrink 壓成 0 高,只剩按鈕。position:fixed 不受影響(祖先無 transform)。
  (document.querySelector("#app .audio-console") ?? document.body).appendChild(layer);
  applyGhostPos(); // 起始位置同樣防夾（層已掛載才能量尺寸）
  window.addEventListener("drag", followPointer, true);
  window.addEventListener("dragover", followPointer, true);
  // 壓掉 OS 半透明預設拖曳影像:canvas 版在 Chromium 不可靠(退回預設快照),
  // 優先用預載透明 GIF Image;極早期未載完才退 canvas
  const dot = document.createElement("canvas");
  dot.width = 1;
  dot.height = 1;
  const img = transparentDragImage();
  dt.setDragImage(img.complete ? img : dot, 0, 0);
}

export function removeDragGhost(): void {
  window.removeEventListener("drag", followPointer, true);
  window.removeEventListener("dragover", followPointer, true);
  layer?.remove();
  layer = null;
}
