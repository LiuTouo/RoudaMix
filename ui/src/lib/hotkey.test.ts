import assert from "node:assert";
import { test } from "node:test";
import { eventToShortcut } from "./hotkey.ts";

/** 最小 KeyboardEvent 假體(只帶 hotkey.ts 用到的欄位) */
const ev = (p: Partial<KeyboardEvent>): KeyboardEvent => p as KeyboardEvent;

test("字母 + Ctrl(+Shift) → 正規 Ctrl 在前的 shortcut 字串", () => {
  assert.equal(
    eventToShortcut(ev({ key: "m", code: "KeyM", ctrlKey: true, shiftKey: true })),
    "Ctrl+Shift+M",
  );
  assert.equal(eventToShortcut(ev({ key: "M", code: "KeyM", ctrlKey: true })), "Ctrl+M");
  assert.equal(eventToShortcut(ev({ key: "9", code: "Digit9", altKey: true })), "Alt+9");
});

test("F 鍵不需修飾鍵;Super 以 Meta 帶入", () => {
  assert.equal(eventToShortcut(ev({ key: "F9", code: "F9" })), "F9");
  assert.equal(eventToShortcut(ev({ key: "F1", code: "F1", metaKey: true })), "Super+F1");
});

test("純字母 / Shift-only / 純修飾鍵按下 = 拒絕(null)", () => {
  assert.equal(eventToShortcut(ev({ key: "m", code: "KeyM" })), null);
  assert.equal(eventToShortcut(ev({ key: "M", code: "KeyM", shiftKey: true })), null);
  assert.equal(eventToShortcut(ev({ key: "Control", code: "ControlLeft", ctrlKey: true })), null);
  assert.equal(eventToShortcut(ev({ key: "Shift", code: "ShiftLeft", shiftKey: true })), null);
});

test("不支援的鍵 = null;支援的符號鍵轉 friendly 名", () => {
  assert.equal(eventToShortcut(ev({ key: "AudioVolumeUp", code: "AudioVolumeUp" })), null);
  assert.equal(
    eventToShortcut(ev({ key: "Up", code: "ArrowUp", ctrlKey: true })),
    "Ctrl+Up",
  );
});
