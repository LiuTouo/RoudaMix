// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { installFocusMode } from "../focusMode";

let uninstall: (() => void) | undefined;

beforeEach(() => {
  uninstall = installFocusMode();
});
afterEach(() => {
  uninstall?.();
  document.body.replaceChildren();
});

describe("滑鼠／鍵盤焦點模式", () => {
  it("滑鼠按下加上 body.using-mouse，Tab 按下移除", () => {
    document.body.dispatchEvent(new Event("mousedown", { bubbles: true }));
    expect(document.body.classList.contains("using-mouse")).toBe(true);
    document.body.dispatchEvent(new KeyboardEvent("keydown", { key: "Tab", bubbles: true }));
    expect(document.body.classList.contains("using-mouse")).toBe(false);
  });

  it("非 Tab 按鍵（快速鍵）不得還原鍵盤導覽", () => {
    document.body.dispatchEvent(new Event("mousedown", { bubbles: true }));
    document.body.dispatchEvent(new KeyboardEvent("keydown", { key: "a", bubbles: true }));
    expect(document.body.classList.contains("using-mouse")).toBe(true);
  });
});
