// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { installTooltip, } from "../tooltip";

const TOOLTIP_ID = "roudamix-tooltip";
const hover = (element: Element) =>
  element.dispatchEvent(new Event("pointerover", { bubbles: true }));
const tip = () => document.getElementById(TOOLTIP_ID)!;

let uninstall: (() => void) | undefined;

beforeEach(() => {
  vi.useFakeTimers();
  uninstall = installTooltip();
});
afterEach(() => {
  uninstall?.();
  document.body.replaceChildren();
  vi.useRealTimers();
});

describe("全域提示層", () => {
  it("懸停 data-tooltip 元素後延遲顯示文字", () => {
    const target = document.createElement("button");
    target.dataset.tooltip = "按鈕提示";
    document.body.append(target);
    hover(target);
    expect(tip().hidden).toBe(true);
    vi.advanceTimersByTime(450);
    expect(tip().hidden).toBe(false);
    expect(tip().textContent).toBe("按鈕提示");
  });

  it("dialog 內的元素不得繼承 dialog 外層的提示（如軌道拖曳提示）", () => {
    const outer = document.createElement("div");
    outer.dataset.tooltip = "拖曳軌道空白區調整順序";
    const dialog = document.createElement("dialog");
    dialog.open = true;
    const inner = document.createElement("summary");
    dialog.append(inner);
    outer.append(dialog);
    document.body.append(outer);

    hover(inner);
    vi.advanceTimersByTime(450);
    expect(tip().hidden).toBe(true); // 不顯示外層拖曳提示

    inner.dataset.tooltip = "廠牌提示";
    hover(inner);
    vi.advanceTimersByTime(450);
    expect(tip().textContent).toBe("廠牌提示"); // dialog 自己的提示照常
  });
});
