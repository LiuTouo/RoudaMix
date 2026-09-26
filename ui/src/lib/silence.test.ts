// 輸入看門狗測試:SilenceWatcher 靜音 debounce/解除/抑制、AlarmSet 查重與嗶聲判定
import { test } from "node:test";
import assert from "node:assert";
import { AlarmSet, createAlertBeeper, SilenceWatcher } from "./silence.ts";

test("SilenceWatcher:連續 frames 個靜音 frame 才轉警示;恢復才解除", () => {
  const w = new SilenceWatcher(0.001, 3);
  assert.equal(w.sample(1, 0.0001, true), null); // 1
  assert.equal(w.sample(1, 0.0005, true), null); // 2
  assert.equal(w.sample(1, 0.0001, true), "alert"); // 3 → 剛轉警示
  assert.equal(w.sample(1, 0.0, true), null); // 已警示,不重複
  assert.equal(w.sample(1, 0.05, true), "clear"); // 恢復 → 剛解除
  assert.equal(w.sample(1, 0.05, true), null);
  assert.equal(w.sample(1, 0.0, true), null);
  assert.equal(w.sample(1, 0.0, true), null);
  assert.equal(w.sample(1, 0.0, true), "alert"); // 重新武裝後再靜音
});

test("SilenceWatcher:單次有聲 frame 重置連續計數(停頓不誤報)", () => {
  const w = new SilenceWatcher(0.001, 3);
  assert.equal(w.sample(1, 0.0, true), null);
  assert.equal(w.sample(1, 0.0, true), null);
  assert.equal(w.sample(1, 0.9, true), null); // 出聲 → 歸零
  assert.equal(w.sample(1, 0.0, true), null);
  assert.equal(w.sample(1, 0.0, true), null);
  assert.equal(w.sample(1, 0.0, true), "alert"); // 要再連續 3 次
});

test("SilenceWatcher:monitored=false 立即解除且歸零(被靜音 = 不判斷)", () => {
  const w = new SilenceWatcher(0.001, 2);
  assert.equal(w.sample(1, 0.0, true), null);
  assert.equal(w.sample(1, 0.0, true), "alert");
  assert.equal(w.sample(1, 0.0, false), "clear"); // 資格喪失 → 解除
  assert.equal(w.sample(1, 0.0, true), null); // 計數已歸零,重新累積
  assert.equal(w.sample(1, 0.0, true), "alert");
});

test("SilenceWatcher:dismiss 抑制到訊號恢復為止", () => {
  const w = new SilenceWatcher(0.001, 2);
  assert.equal(w.sample(1, 0.0, true), null);
  assert.equal(w.sample(1, 0.0, true), "alert");
  w.dismiss(1);
  assert.equal(w.sample(1, 0.0, true), null); // 抑制中:持續靜音不再警示
  assert.equal(w.sample(1, 0.3, true), "clear"); // 恢復 → 解除並重新武裝
  assert.equal(w.sample(1, 0.0, true), null);
  assert.equal(w.sample(1, 0.0, true), "alert"); // 恢復後再靜音 → 再警示
});

test("SilenceWatcher:forget 移除軌道(警示中回 clear)、reset 全歸零", () => {
  const w = new SilenceWatcher(0.001, 1);
  assert.equal(w.sample(7, 0.0, true), "alert");
  assert.deepEqual(w.ids(), [7]);
  assert.equal(w.forget(7), "clear");
  assert.equal(w.forget(7), null);
  assert.deepEqual(w.ids(), []);
  w.sample(8, 0.0, true);
  w.reset();
  assert.deepEqual(w.ids(), []);
  assert.equal(w.sample(8, 0.0, true), "alert"); // reset 後重新計數
});

test("AlarmSet:raise 查重、dismiss 停嗶、clear 後可重新 raise", () => {
  const a = new AlarmSet();
  assert.equal(a.shouldBeep, false);
  assert.equal(a.raise("silence:1"), true);
  assert.equal(a.raise("silence:1"), false); // 重複 raise = no-op
  assert.equal(a.raise("stale"), true);
  assert.equal(a.shouldBeep, true);
  a.dismiss("silence:1");
  assert.equal(a.shouldBeep, true); // stale 仍在嗶
  a.dismiss("stale");
  assert.equal(a.shouldBeep, false); // 全手動關閉 → 停嗶
  assert.deepEqual(a.activeKeys.sort(), ["silence:1", "stale"]); // 情況未解除,警示仍在
  assert.equal(a.clear("silence:1"), true);
  assert.equal(a.clear("silence:1"), false);
  assert.equal(a.raise("silence:1"), true); // 解除後可重新警示
  assert.equal(a.shouldBeep, true); // 重新 raise 清除 dismissed
  a.reset();
  assert.deepEqual(a.activeKeys, []);
  assert.equal(a.shouldBeep, false);
});

test("createAlertBeeper:start/stop 不拋(無 AudioContext 環境靜默失敗)、冪等", () => {
  const b = createAlertBeeper(10);
  b.start();
  b.start(); // 冪等:不重複排程
  b.stop();
  b.stop(); // 冪等
});
