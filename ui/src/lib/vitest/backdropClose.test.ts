// @vitest-environment jsdom
import { describe, expect, it, vi } from "vitest";
import { backdropClose } from "../backdropClose";

// jsdom 的 getBoundingClientRect 恆為全 0,stub 成 10,10–110,80 的假視窗矩形。
function makeDialog(): HTMLDialogElement {
  const dlg = document.createElement("dialog");
  dlg.open = true;
  vi.spyOn(dlg, "getBoundingClientRect").mockReturnValue({
    x: 10, y: 10, width: 100, height: 70,
    top: 10, left: 10, right: 110, bottom: 80,
    toJSON: () => ({}),
  } as DOMRect);
  document.body.append(dlg);
  return dlg;
}

function press(dlg: HTMLDialogElement, x: number, y: number) {
  dlg.dispatchEvent(new MouseEvent("mousedown", { clientX: x, clientY: y, bubbles: true }));
}

describe("backdropClose", () => {
  it("rect 外按下 → 呼叫關閉;rect 內(含邊緣)→ 不呼叫", () => {
    const dlg = makeDialog();
    const close = vi.fn();
    const action = backdropClose(dlg, close);

    press(dlg, 50, 50); // 視窗內
    press(dlg, 10, 10); // 左上角(邊界算視窗內)
    press(dlg, 110, 80); // 右下角
    expect(close).not.toHaveBeenCalled();

    press(dlg, 111, 40); // 右外
    press(dlg, 9, 40); // 左外
    press(dlg, 40, 81); // 下外
    press(dlg, 40, 9); // 上外
    expect(close).toHaveBeenCalledTimes(4);

    action.destroy?.();
    dlg.remove();
  });

  it("dialog 未開啟時不反應", () => {
    const dlg = makeDialog();
    dlg.open = false;
    const close = vi.fn();
    backdropClose(dlg, close);
    press(dlg, 500, 500);
    expect(close).not.toHaveBeenCalled();
    dlg.remove();
  });

  it("update 換新 callback;destroy 後不再監聽", () => {
    const dlg = makeDialog();
    const first = vi.fn();
    const second = vi.fn();
    const action = backdropClose(dlg, first);
    action.update?.(second);

    press(dlg, 500, 500);
    expect(first).not.toHaveBeenCalled();
    expect(second).toHaveBeenCalledTimes(1);

    action.destroy?.();
    press(dlg, 500, 500);
    expect(second).toHaveBeenCalledTimes(1);
    dlg.remove();
  });
});
