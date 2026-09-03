// P1-O/P1-J 測試:錯誤映射(結構化 {code, message} 輸入)+ 過載 debounce
// 舊 regex 版測試的規則逐條對應到 code-based 版本(規則不變,換測法):
// 1. 已知 code → 繁中主訊息、raw = "code: message"
// 2. 每個 engine/傳輸錯誤碼都有映射
// 3. plugin_load_failed 不誤報檔案損壞
// 4. 未知 code → message 原樣;非物件 rejection → internal fallback(message 保留)
import { test } from "node:test";
import assert from "node:assert";
import { errorText, friendlyError, OverloadDetector } from "./errors.ts";

test("friendlyError:已知 code → 繁中主訊息,raw 保留技術細節", () => {
  const f = friendlyError({ code: "cycle_detected", message: "routing would create a cycle" });
  assert.match(f.friendly, /迴圈/);
  assert.equal(f.raw, "cycle_detected: routing would create a cycle");
});

test("errorText:code + message 的顯示字串", () => {
  assert.equal(
    errorText({ code: "device_busy", message: "wasapi device not found: x" }),
    "device_busy: wasapi device not found: x",
  );
});

test("friendlyError:每個 engine 錯誤碼都有映射", () => {
  const codes = [
    "not_connected", "disconnected", "timeout", "unsupported_version", "bad_command",
    "not_running", "already_running", "device_open_failed", "device_lost", "device_busy",
    "track_not_found", "cycle_detected", "plugin_not_found", "plugin_load_failed",
    "plugin_no_editor", "param_not_found", "preset_io", "plugin_state_failed",
    "session_io", "app_not_found", "unsupported_windows", "internal", "bad_frame",
  ];
  for (const c of codes) {
    const f = friendlyError({ code: c, message: "x" });
    assert.notEqual(f.friendly, `${c}: x`, `${c} 應有繁中映射`);
  }
});

test("plugin_load_failed 不把 host 不相容誤報為檔案損壞", () => {
  const f = friendlyError({
    code: "plugin_load_failed",
    message: "init failed: plugin rejected stereo main-bus arrangement",
  });
  assert.doesNotMatch(f.friendly, /損毀|損壞/);
  assert.match(f.friendly, /載入或初始化失敗/);
  assert.match(f.raw, /stereo main-bus/);
});

test("friendlyError:未知 code → message 原樣(raw 帶 code)", () => {
  const f = friendlyError({ code: "unknown_code", message: "hi" });
  assert.equal(f.friendly, "hi");
  assert.equal(f.raw, "unknown_code: hi");
});

test("friendlyError:非物件 rejection → internal fallback,message 保留在 raw", () => {
  const f = friendlyError("某個隨機錯誤");
  assert.equal(f.raw, "internal: 某個隨機錯誤");
  const thrown = friendlyError(new Error("boom path"));
  assert.equal(thrown.raw, "internal: boom path");
});

test("OverloadDetector:連續 hitCount 次超標才轉警示;連續 clearCount 次正常才解除", () => {
  const d = new OverloadDetector(1.0, 3, 3);
  assert.equal(d.sample(1.2), false); // 1 次
  assert.equal(d.sample(1.5), false); // 2 次
  assert.equal(d.sample(0.9), false); // 歸零
  assert.equal(d.sample(1.2), false);
  assert.equal(d.sample(1.2), false);
  assert.equal(d.sample(1.2), true); // 連續 3 次 → 剛轉過載
  assert.equal(d.isOverloaded, true);
  assert.equal(d.sample(1.1), false); // 已在過載,不重複通知
  assert.equal(d.sample(0.5), false);
  assert.equal(d.sample(0.5), false);
  assert.equal(d.sample(0.5), true); // 連續 3 次正常 → 剛解除
  assert.equal(d.isOverloaded, false);
});
