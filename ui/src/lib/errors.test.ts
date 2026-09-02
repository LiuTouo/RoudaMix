// P1-O/P1-J 測試:錯誤映射 + 過載 debounce
import { test } from "node:test";
import assert from "node:assert";
import { friendlyError, OverloadDetector } from "./errors.ts";

test("friendlyError:已知 code → 繁中主訊息,raw 保留技術細節", () => {
  const f = friendlyError("cycle_detected: routing would create a cycle");
  assert.match(f.friendly, /迴圈/);
  assert.equal(f.raw, "cycle_detected: routing would create a cycle");
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
    const f = friendlyError(`${c}: x`);
    assert.notEqual(f.friendly, `${c}: x`, `${c} 應有繁中映射`);
  }
});

test("plugin_load_failed 不把 host 不相容誤報為檔案損壞", () => {
  const f = friendlyError(
    "plugin_load_failed: init failed: plugin rejected stereo main-bus arrangement",
  );
  assert.doesNotMatch(f.friendly, /損毀|損壞/);
  assert.match(f.friendly, /載入或初始化失敗/);
  assert.match(f.raw, /stereo main-bus/);
});

test("friendlyError:未知/無 code 字串原樣", () => {
  assert.equal(friendlyError("某個隨機錯誤").friendly, "某個隨機錯誤");
  const f = friendlyError("unknown_code: hi");
  assert.equal(f.friendly, "unknown_code: hi");
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
