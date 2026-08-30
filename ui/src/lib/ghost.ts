// 拖曳「拿起」幽靈圖:掛暫時 clone(旋轉+陰影)當 dragImage。
// 預設半透明截圖沒有拿起感;clone 的樣式在全域 .drag-ghost(app.css)。
let wrap: HTMLElement | null = null;

export function mountDragGhost(dt: DataTransfer, src: HTMLElement, width: number): void {
  removeDragGhost();
  wrap = document.createElement("div");
  wrap.style.cssText = `position:fixed;top:-1200px;left:-1200px;width:${width}px;pointer-events:none;z-index:-1;`;
  const ghost = src.cloneNode(true) as HTMLElement;
  // 拖曳中的暫態 class 不能進 ghost(原件的淡化/插入指示會一起被拍進去)
  ghost.classList.remove("dragging", "dropbefore", "dropafter");
  ghost.classList.add("drag-ghost");
  wrap.appendChild(ghost);
  document.body.appendChild(wrap);
  dt.setDragImage(wrap, 30, 16);
}

export function removeDragGhost(): void {
  wrap?.remove();
  wrap = null;
}
