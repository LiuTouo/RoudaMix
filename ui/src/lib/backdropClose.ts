import type { Action } from "svelte/action";

// 彈出視窗輕關閉:在 dialog 外圍暗底按下 mousedown 時呼叫正式關閉路徑。
// 用座標(而非 e.target === dlg)判斷:可捲動 dialog 的捲軸、以及從內容
// 拖選文字時,mousedown 的 target 也是 dialog 本體,座標判斷可避免誤關;
// 視窗內邊距與內容的點擊不受影響。
export const backdropClose: Action<HTMLDialogElement, () => void> = (dlg, close) => {
  let onClose = close;
  function onMousedown(e: MouseEvent) {
    if (!dlg.open) return;
    const r = dlg.getBoundingClientRect();
    if (e.clientX < r.left || e.clientX > r.right || e.clientY < r.top || e.clientY > r.bottom)
      onClose();
  }
  dlg.addEventListener("mousedown", onMousedown);
  return {
    update(next: () => void) {
      onClose = next;
    },
    destroy() {
      dlg.removeEventListener("mousedown", onMousedown);
    },
  };
};
